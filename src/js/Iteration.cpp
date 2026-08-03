#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// Symbols, and the iteration protocol they exist to key.
//
// These are one feature rather than two. A symbol's whole purpose is to be a
// property key no page can write out, and the reason the language needed one
// is `Symbol.iterator`: a hook on every iterable that could not collide with a
// property a page had already put there. Installing them apart would separate
// the mechanism from its only current use.
//
// Iterator state lives in ordinary properties named with a leading `#`, the
// same convention private class fields already use here. That is not real
// privacy and is the right amount until something observes the difference --
// see the note in docs/adr/0007 about private names.

namespace microbrowser::js {

namespace {

// A backstop on any single iteration.
//
// The step budget in Evaluate bounds a JavaScript `next`, but a native one
// costs no steps, and a page can hand `for...of` an object whose `next` never
// reports done. This is the bound that makes that a RangeError.
constexpr std::size_t kMaxIterationSteps = 1u << 24;

// Where a built-in iterator keeps what it is walking and how far it has got.
constexpr const char* kTargetKey = "#target";
constexpr const char* kIndexKey = "#index";

}  // namespace

// --- Symbol and the built-in iterators --------------------------------------

void Interpreter::InstallIteration() {
  // `Symbol.prototype`, which a symbol's cell points at so that `sym
  // .description` and `sym.toString()` resolve without boxing -- the cell is
  // an object, so the ordinary prototype walk already works on it.
  Object* symbol_prototype = NewObject();
  if (symbol_prototype == nullptr) {
    return;
  }
  InstallNative(symbol_prototype, "toString", [](NativeCall& call) {
    return Value::String(ToString(call.self));
  });

  Object* constructor = NewNative("Symbol", [](NativeCall& call) {
    // A fresh cell every time, description or not: `Symbol('x') !==
    // Symbol('x')` is the property everything else here rests on.
    Object* cell = call.interpreter.GetHeap().AllocateObject(Object::Kind::Symbol);
    if (cell == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    const Value* prototype =
        call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
    if (prototype != nullptr && prototype->IsObject()) {
      cell->SetPrototype(prototype->object);
    }
    const Value description = Argument(call.arguments, 0);
    cell->Set("description",
              description.IsUndefined() ? Value::Undefined()
                                        : Value::String(ToString(description)));
    return Value::Sym(cell);
  });
  if (constructor == nullptr) {
    return;
  }
  constructor->Set("prototype", Value::Obj(symbol_prototype));
  symbol_prototype->Set("constructor", Value::Obj(constructor));

  // The well-known symbols. Each is one cell, allocated once, and its identity
  // is what makes it well-known -- a page can read `Symbol.iterator` and hang
  // a method on it, and cannot construct another one that compares equal.
  const auto well_known = [this, constructor, symbol_prototype](const char* name) -> Object* {
    Object* cell = heap_.AllocateObject(Object::Kind::Symbol);
    if (cell == nullptr) {
      return nullptr;
    }
    cell->SetPrototype(symbol_prototype);
    cell->Set("description", Value::String(std::string("Symbol.") + name));
    constructor->Set(name, Value::Sym(cell));
    return cell;
  };
  well_known_.symbol_iterator = well_known("iterator");
  well_known("asyncIterator");
  well_known("hasInstance");
  well_known("toPrimitive");
  well_known("toStringTag");

  // The registry behind `Symbol.for`, which is the one way two symbols with
  // the same description *are* the same symbol.
  //
  // Held on the `for` function object rather than captured, because a capture
  // in the std::function is invisible to the collector -- the same rule the
  // note on NativeCall states. `call.callee` is that function, which is why it
  // goes there and not on the constructor.
  Object* lookup = NewNative("for", [](NativeCall& call) {
    const std::string key = ToString(Argument(call.arguments, 0));
    const Value* registry_value =
        call.callee == nullptr ? nullptr : call.callee->GetOwn("#registry");
    Object* table = registry_value != nullptr && registry_value->IsObject()
                        ? registry_value->object
                        : nullptr;
    if (table == nullptr) {
      return call.Throw("TypeError", "Symbol.for is unavailable");
    }
    if (const Value* existing = table->GetOwn(key)) {
      return *existing;
    }
    Object* cell = call.interpreter.GetHeap().AllocateObject(Object::Kind::Symbol);
    if (cell == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    cell->Set("description", Value::String(key));
    cell->Set("#registered", Value::String(key));
    table->Set(key, Value::Sym(cell));
    return Value::Sym(cell);
  });
  if (lookup != nullptr) {
    Object* registry = NewObject();
    if (registry != nullptr) {
      lookup->Set("#registry", Value::Obj(registry));
    }
    constructor->Set("for", Value::Obj(lookup));
  }
  // `Symbol.for` writes the registry key onto the cell, so keyFor reads it
  // back rather than searching the table.
  InstallNative(constructor, "keyFor", [](NativeCall& call) {
    const Value symbol = Argument(call.arguments, 0);
    if (!symbol.IsSymbol()) {
      return call.Throw("TypeError", "Symbol.keyFor requires a symbol");
    }
    const Value* key = symbol.object->GetOwn("#registered");
    return key == nullptr ? Value::Undefined() : *key;
  });
  global_scope_->Declare("Symbol", Value::Obj(constructor), false);

  if (well_known_.symbol_iterator == nullptr) {
    return;
  }
  const PropertyKey iterator_key = PropertyKey::Symbol(well_known_.symbol_iterator);

  // An iterator over something indexable. The array and string iterators
  // differ only in what they read out of the target, so they share this.
  const auto make_indexed_iterator = [this](const Value& target, bool by_character) {
    Object* iterator = NewObject();
    if (iterator == nullptr) {
      return Value::Undefined();
    }
    iterator->Set(kTargetKey, target);
    iterator->Set(kIndexKey, Value::Number(0.0));
    InstallNative(iterator, "next", [by_character](NativeCall& call) {
      Value result = call.interpreter.NewObjectValue();
      if (!result.IsObject() || !call.self.IsObject()) {
        return result;
      }
      const Value* target_value = call.self.object->GetOwn(kTargetKey);
      const Value* index_value = call.self.object->GetOwn(kIndexKey);
      const std::size_t index =
          index_value == nullptr ? 0 : static_cast<std::size_t>(ToNumber(*index_value));
      const std::size_t size =
          target_value == nullptr
              ? 0
              : (by_character ? target_value->AsString().size()
                              : (target_value->IsObject() ? target_value->object->ElementCount()
                                                          : 0));
      if (target_value == nullptr || index >= size) {
        result.object->Set("value", Value::Undefined());
        result.object->Set("done", Value::Bool(true));
        return result;
      }
      result.object->Set("value",
                         by_character
                             ? Value::String(std::string(1, target_value->AsString()[index]))
                             : target_value->object->GetElement(index));
      result.object->Set("done", Value::Bool(false));
      call.self.object->Set(kIndexKey, Value::Number(static_cast<double>(index + 1)));
      return result;
    });
    // An iterator is itself iterable, which is what lets `for...of` accept the
    // result of calling one directly.
    return Value::Obj(iterator);
  };

  const auto install_iterator_hook = [this, iterator_key, make_indexed_iterator](
                                         Object* target, bool by_character) {
    Object* hook = NewNative("[Symbol.iterator]", [make_indexed_iterator,
                                                  by_character](NativeCall& call) {
      return make_indexed_iterator(call.self, by_character);
    });
    if (hook != nullptr) {
      target->Set(iterator_key, Value::Obj(hook));
    }
  };
  install_iterator_hook(well_known_.array_prototype, false);
  install_iterator_hook(well_known_.string_prototype, true);
}

// --- The protocol ----------------------------------------------------------

Result Interpreter::OpenIteration(const Value& iterable, Iteration& state) {
  state = Iteration{};
  if (iterable.IsNullish()) {
    return Throw("TypeError", ToString(iterable) + " is not iterable");
  }
  if (well_known_.symbol_iterator == nullptr) {
    return Throw("TypeError", "the iteration protocol is unavailable");
  }
  const PropertyKey iterator_key = PropertyKey::Symbol(well_known_.symbol_iterator);

  // The fast path, taken only when the object's hook is still the built-in
  // one. A page that replaces `Array.prototype[Symbol.iterator]` gets the
  // general path and the behaviour it asked for.
  if (iterable.IsObject() && iterable.object->GetKind() == Object::Kind::Array) {
    const Object::Property* found = iterable.object->GetProperty(iterator_key);
    const Object::Property* builtin =
        well_known_.array_prototype == nullptr ? nullptr : well_known_.array_prototype->GetOwnProperty(iterator_key);
    if (found != nullptr && builtin != nullptr && !found->IsAccessor() &&
        !builtin->IsAccessor() && StrictEquals(found->value, builtin->value)) {
      state.array = iterable.object;
      return Result::Normal();
    }
  }

  const Value hook = GetProperty(iterable, iterator_key);
  if (!hook.IsObject() || !hook.object->IsCallable()) {
    return Throw("TypeError", ToString(iterable) + " is not iterable");
  }
  const Result opened = CallFunction(hook, iterable, {});
  if (opened.IsAbrupt()) {
    return opened;
  }
  if (!opened.value.IsObject()) {
    return Throw("TypeError", "the iterator is not an object");
  }
  state.iterator = opened.value;
  state.next = GetProperty(state.iterator, "next");
  if (!state.next.IsObject() || !state.next.object->IsCallable()) {
    return Throw("TypeError", "the iterator has no next method");
  }
  return Result::Normal();
}

Result Interpreter::CollectIterable(const Value& iterable, std::vector<Value>& out) {
  Iteration cursor;
  const Result opened = OpenIteration(iterable, cursor);
  if (opened.IsAbrupt()) {
    return opened;
  }
  for (;;) {
    Value item;
    bool done = false;
    const Result stepped = StepIteration(cursor, item, done);
    if (stepped.IsAbrupt()) {
      return stepped;
    }
    if (done) {
      return Result::Normal();
    }
    if (out.size() >= kMaxAllocationLength) {
      return Throw("RangeError", "too many values to spread");
    }
    out.push_back(std::move(item));
  }
}

Result Interpreter::StepIteration(Iteration& state, Value& value_out, bool& done) {
  done = false;
  value_out = Value::Undefined();

  if (state.array != nullptr) {
    // Re-read the length each time rather than caching it: `for (const x of
    // xs) xs.pop()` is legal, and the built-in iterator sees the array
    // shrinking.
    if (state.index >= state.array->ElementCount()) {
      done = true;
      return Result::Normal();
    }
    value_out = state.array->GetElement(state.index);
    ++state.index;
    return Result::Normal();
  }

  if (!state.next.IsObject()) {
    done = true;
    return Result::Normal();
  }
  if (++state.index > kMaxIterationSteps) {
    return Throw("RangeError", "iteration ran too long");
  }
  const Result stepped = CallFunction(state.next, state.iterator, {});
  if (stepped.IsAbrupt()) {
    return stepped;
  }
  if (!stepped.value.IsObject()) {
    return Throw("TypeError", "the iterator returned a non-object");
  }
  if (ToBoolean(GetProperty(stepped.value, "done"))) {
    done = true;
    return Result::Normal();
  }
  value_out = GetProperty(stepped.value, "value");
  return Result::Normal();
}

}  // namespace microbrowser::js

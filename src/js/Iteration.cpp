#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "js/StringUnits.h"

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
  symbol_prototype->SetHidden("constructor", Value::Obj(constructor));

  // The well-known symbols. Each is one cell, allocated once, and its identity
  // is what makes it well-known -- a page can read `Symbol.iterator` and hang
  // a method on it, and cannot construct another one that compares equal.
  // One cell per name for the whole *interpreter*, not per realm: a second realm
  // installs the cells realm 0 allocated onto its own `Symbol` (ADR 0042 §1). Two
  // cells would break every protocol that crosses a realm -- spreading a
  // cross-origin... a cross-*realm* array would find no `Symbol.iterator` and
  // silently produce nothing.
  //
  // The prototype a re-used cell carries is realm 0's `Symbol.prototype`, which
  // is a deliberate approximation: a symbol is a primitive and nothing reads that
  // pointer except method lookup on a boxed symbol, so the cost is
  // `Object.getPrototypeOf(frames[0].Symbol.iterator)` naming the parent's
  // prototype. Per-realm cells would cost every iteration protocol instead.
  const auto well_known = [this, constructor, symbol_prototype](const char* name) -> Object* {
    if (const auto found = shared_.symbols.find(name); found != shared_.symbols.end()) {
      constructor->Set(name, Value::Sym(found->second));
      return found->second;
    }
    Object* cell = heap_.AllocateObject(Object::Kind::Symbol);
    if (cell == nullptr) {
      return nullptr;
    }
    cell->SetPrototype(symbol_prototype);
    cell->Set("description", Value::String(std::string("Symbol.") + name));
    constructor->Set(name, Value::Sym(cell));
    shared_.symbols.emplace(name, cell);
    return cell;
  };
  shared_.symbol_iterator = well_known("iterator");
  // Held for the reason `Symbol.iterator` is: `for await` resolves against it,
  // and a page can reassign the global `Symbol` but cannot make another cell
  // that compares equal to this one.
  shared_.symbol_async_iterator = well_known("asyncIterator");
  // The three an operator consults. Held for the reason the two above are:
  // `+` looks for `Symbol.toPrimitive`, `instanceof` for `Symbol.hasInstance`
  // and `Object.prototype.toString` for `Symbol.toStringTag`, and none of
  // those may stop working because a page assigned to the global `Symbol`.
  shared_.symbol_has_instance = well_known("hasInstance");
  shared_.symbol_to_primitive = well_known("toPrimitive");
  shared_.symbol_to_string_tag = well_known("toStringTag");
  // The pattern-protocol symbols. Nothing consults them yet -- the String
  // methods still test for a RegExp object rather than for these -- but a page
  // that reads one must get the same cell every time, and `Symbol.species`
  // is read by library code that never calls it.
  // The pattern protocol. Held on the constructor by name, and looked up by
  // name in PatternProtocol -- which reads `Symbol` back out of the global
  // scope, so a page that shadows it changes what its own objects answer to
  // and nothing else.
  well_known("species");
  well_known("match");
  well_known("matchAll");
  well_known("replace");
  well_known("search");
  well_known("split");
  well_known("isConcatSpreadable");
  well_known("unscopables");

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
    cell->SetHidden("#registered", Value::String(key));
    table->Set(key, Value::Sym(cell));
    return Value::Sym(cell);
  });
  if (lookup != nullptr) {
    Object* registry = NewObject();
    if (registry != nullptr) {
      lookup->SetHidden("#registry", Value::Obj(registry));
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
  realm_->global_scope->Declare("Symbol", Value::Obj(constructor), false);

  if (shared_.symbol_iterator == nullptr) {
    return;
  }
  const PropertyKey iterator_key = PropertyKey::Symbol(shared_.symbol_iterator);

  // An iterator over something indexable. The array and string iterators
  // differ only in what they read out of the target, so they share this.
  const auto make_indexed_iterator = [this, iterator_key](const Value& target,
                                                          bool by_character) {
    Object* iterator = NewObject();
    if (iterator == nullptr) {
      return Value::Undefined();
    }
    iterator->SetHidden(kTargetKey, target);
    iterator->SetHidden(kIndexKey, Value::Number(0.0));
    InstallNative(iterator, "next", [by_character](NativeCall& call) {
      Value result = call.interpreter.NewObjectValue();
      if (!result.IsObject() || !call.self.IsObject()) {
        return result;
      }
      const Value* target_value = call.self.object->GetOwn(kTargetKey);
      const Value* index_value = call.self.object->GetOwn(kIndexKey);
      const std::size_t index =
          index_value == nullptr ? 0 : static_cast<std::size_t>(ToNumber(*index_value));
      if (target_value == nullptr) {
        result.object->Set("value", Value::Undefined());
        result.object->Set("done", Value::Bool(true));
        return result;
      }
      // Array.prototype[@@iterator] is generic: it Gets `length` and the
      // indices rather than reading ElementCount. A plain object with
      // `{0: "PASS", length: 1}` is a sequence for Blob parts, and a length
      // getter that throws has to propagate.
      std::size_t size = 0;
      if (by_character) {
        size = Utf16Length(target_value->AsString());
      } else if (target_value->IsObject()) {
        Result abrupt = Result::Normal();
        const Value length_value =
            call.interpreter.GetPropertyOrThrow(*target_value, "length", abrupt);
        if (abrupt.IsAbrupt()) {
          return call.ThrowValue(abrupt.value);
        }
        double number = 0;
        const Result converted = call.interpreter.ToNumberOf(length_value, number);
        if (converted.IsAbrupt()) {
          return call.ThrowValue(converted.value);
        }
        if (std::isfinite(number) && number > 0) {
          size = static_cast<std::size_t>(
              std::min(std::trunc(number), 9007199254740991.0));
        }
      }
      if (index >= size) {
        result.object->Set("value", Value::Undefined());
        result.object->Set("done", Value::Bool(true));
        return result;
      }
      // A string iterates by *code point*, not by code unit -- which is the
      // one place the two differ and is why `[...'a\u{1F600}']` is two
      // entries and `'a\u{1F600}'.length` is three.
      std::size_t step = 1;
      Value item;
      if (by_character) {
        const std::string& text = target_value->AsString();
        const std::uint32_t code = CodePointAt(text, index);
        step = code > 0xFFFFu ? 2 : 1;
        item = Value::String(SubstringUnits(text, index, index + step));
      } else {
        Result abrupt = Result::Normal();
        item = call.interpreter.GetPropertyOrThrow(
            *target_value, NumberToString(static_cast<double>(index)), abrupt);
        if (abrupt.IsAbrupt()) {
          return call.ThrowValue(abrupt.value);
        }
      }
      result.object->Set("value", item);
      result.object->Set("done", Value::Bool(false));
      call.self.object->SetHidden(kIndexKey, Value::Number(static_cast<double>(index + step)));
      return result;
    });
    // An iterator is itself iterable, which is what lets `for...of` and a
    // spread accept the result of calling one directly. This has to be *set*
    // and not merely intended: `[...arr.values()]` is the call that finds it
    // missing.
    iterator->Set(iterator_key, NewNativeValue("[Symbol.iterator]",
                                               [](NativeCall& inner) { return inner.self; }));
    return Value::Obj(iterator);
  };

  const auto install_iterator_hook = [this, iterator_key, make_indexed_iterator](
                                         Object* target, bool by_character) {
    Object* hook = NewNative("[Symbol.iterator]", [make_indexed_iterator,
                                                  by_character](NativeCall& call) {
      if (by_character && !call.self.IsString()) {
        std::string text;
        const Result converted = call.interpreter.ToStringOf(call.self, text);
        if (converted.IsAbrupt()) {
          return call.ThrowValue(converted.value);
        }
        return make_indexed_iterator(Value::String(std::move(text)), by_character);
      }
      return make_indexed_iterator(call.self, by_character);
    });
    if (hook != nullptr) {
      target->Set(iterator_key, Value::Obj(hook));
    }
  };
  install_iterator_hook(intrinsics().array_prototype, false);
  install_iterator_hook(intrinsics().string_prototype, true);

  // `keys`, `values` and `entries` on an array are the same walk yielding a
  // different part of each step, and `values` is the same function the
  // `Symbol.iterator` hook is -- aliased rather than reimplemented, so the two
  // cannot disagree about what iterating an array means.
  enum class ArrayYield { Keys, Values, Entries };
  const auto array_iterator = [this](const char* name, ArrayYield yield) {
    InstallNative(intrinsics().array_prototype, name, [yield](NativeCall& call) {
      Value iterator = call.interpreter.NewObjectValue();
      if (!iterator.IsObject()) {
        return iterator;
      }
      iterator.object->SetHidden(kTargetKey, call.self);
      iterator.object->SetHidden(kIndexKey, Value::Number(0.0));
      iterator.object->Set(
          "next", call.interpreter.NewNativeValue("next", [yield](NativeCall& step) {
            Value result = step.interpreter.NewObjectValue();
            if (!result.IsObject() || !step.self.IsObject()) {
              return result;
            }
            const Value* target = step.self.object->GetOwn(kTargetKey);
            const Value* at = step.self.object->GetOwn(kIndexKey);
            const std::size_t index = at == nullptr ? 0 : static_cast<std::size_t>(ToNumber(*at));
            const std::size_t size =
                target != nullptr && target->IsObject() ? target->object->ElementCount() : 0;
            if (index >= size) {
              result.object->Set("value", Value::Undefined());
              result.object->Set("done", Value::Bool(true));
              return result;
            }
            const Value key = Value::Number(static_cast<double>(index));
            const Value value = target->object->GetElement(index);
            Value yielded = key;
            if (yield == ArrayYield::Values) {
              yielded = value;
            } else if (yield == ArrayYield::Entries) {
              yielded = step.interpreter.NewArrayValue({key, value});
            }
            step.self.object->SetHidden(kIndexKey, Value::Number(static_cast<double>(index + 1)));
            result.object->Set("value", yielded);
            result.object->Set("done", Value::Bool(false));
            return result;
          }));
      iterator.object->Set(
          PropertyKey::Symbol(call.interpreter.SymbolIterator()),
          call.interpreter.NewNativeValue("[Symbol.iterator]",
                                          [](NativeCall& inner) { return inner.self; }));
      return iterator;
    });
  };
  array_iterator("keys", ArrayYield::Keys);
  array_iterator("entries", ArrayYield::Entries);
  if (const Value* hook = intrinsics().array_prototype->GetOwn(iterator_key)) {
    intrinsics().array_prototype->Set("values", *hook);
  }
}

// --- The protocol ----------------------------------------------------------

Result Interpreter::OpenIteration(const Value& iterable, Iteration& state) {
  state = Iteration{};
  if (iterable.IsNullish()) {
    return Throw("TypeError", ToString(iterable) + " is not iterable");
  }
  if (shared_.symbol_iterator == nullptr) {
    return Throw("TypeError", "the iteration protocol is unavailable");
  }
  const PropertyKey iterator_key = PropertyKey::Symbol(shared_.symbol_iterator);

  // The fast path, taken only when the object's hook is still the built-in
  // one. A page that replaces `Array.prototype[Symbol.iterator]` gets the
  // general path and the behaviour it asked for.
  if (iterable.IsObject() && iterable.object->GetKind() == Object::Kind::Array) {
    const Object::Property* found = iterable.object->GetProperty(iterator_key);
    const Object::Property* builtin =
        intrinsics().array_prototype == nullptr ? nullptr : intrinsics().array_prototype->GetOwnProperty(iterator_key);
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

Result Interpreter::OpenAsyncIteration(const Value& iterable, Iteration& state) {
  // `Symbol.asyncIterator` first, and the sync protocol when there is none.
  // That fallback is the spec's own and is the reason `for await` over an
  // array of promises works: each value is awaited by the loop rather than by
  // the iterator.
  if (shared_.symbol_async_iterator != nullptr && iterable.IsObject()) {
    const Value hook =
        GetProperty(iterable, PropertyKey::Symbol(shared_.symbol_async_iterator));
    if (hook.IsObject() && hook.object->IsCallable()) {
      const Result opened = CallFunction(hook, iterable, {});
      if (opened.IsAbrupt()) {
        return opened;
      }
      if (!opened.value.IsObject()) {
        return Throw("TypeError", "Symbol.asyncIterator did not return an object");
      }
      state.iterator = opened.value;
      state.next = GetProperty(opened.value, "next");
      state.is_async = true;
      if (!state.next.IsObject() || !state.next.object->IsCallable()) {
        return Throw("TypeError", "the async iterator has no next method");
      }
      return Result::Normal();
    }
  }
  return OpenIteration(iterable, state);
}

Result Interpreter::StepAsyncIteration(Iteration& state, Value& out, bool& done) {
  out = Value::Undefined();
  done = false;
  if (state.is_async) {
    if (++state.index > kMaxIterationSteps) {
      return Throw("RangeError", "iteration ran too long");
    }
    // Whatever `next` returned, unexamined. It is a promise of `{value, done}`
    // and the Await the loop emits next is what turns it into the pair, which
    // is why `done` cannot be answered here -- it is inside the promise.
    const Result stepped = CallFunction(state.next, state.iterator, {});
    if (stepped.IsAbrupt()) {
      return stepped;
    }
    out = stepped.value;
    return Result::Normal();
  }
  // A sync iterable walked by `for await`. Stepped now and the *value* awaited
  // by the loop, which is the wrapping the spec describes: `for await (const x
  // of [p, q])` gives the page what the promises resolved to.
  done = state.done;
  if (done) {
    return Result::Normal();
  }
  const Result advanced = StepIteration(state, out, done);
  state.done = done;
  return advanced;
}

Result Interpreter::CloseIterationCursor(Iteration& cursor) {
  // Marked done before anything runs, so a `return` that walks the same thing
  // again finds a finished cursor rather than one that closes itself twice.
  const bool closeable = !cursor.done && cursor.iterator.IsObject();
  cursor.done = true;
  if (!closeable) {
    return Result::Normal();
  }
  // The caller keeps the cursor rooted for the length of this -- on the
  // machine's cursor stack, or on the tree-walker's shadow stack. The copy
  // below is a C++ local, and a `return` that allocates can collect.
  const Value iterator = cursor.iterator;
  const Value method = GetProperty(iterator, "return");
  if (!method.IsObject() || !method.object->IsCallable()) {
    // An iterator without a `return` is closed by walking away from it, which
    // is what most of them are.
    return Result::Normal();
  }
  return CallFunction(method, iterator, {});
}

Result Interpreter::CloseIterations(std::size_t down_to) {
  while (vm_.iterations.size() > down_to) {
    const std::size_t at = vm_.iterations.size() - 1;
    // By reference into the stack, not by copy: that is what keeps the
    // iterator a root while its own `return` runs. Safe because the stack is
    // reserved once and never grows -- see kIterationCapacity.
    const Result closed = CloseIterationCursor(vm_.iterations[at]);
    vm_.iterations.resize(at);
    if (closed.IsAbrupt()) {
      // Propagated rather than swallowed. Every path that emits an
      // IterateClose is a normal completion -- a `break`, a `continue`, a
      // `return` -- and the spec propagates there. A throw goes through
      // CloseIterationsQuietly instead, which discards.
      return closed;
    }
  }
  return Result::Normal();
}

void Interpreter::CloseIterationsQuietly(std::size_t down_to) {
  while (vm_.iterations.size() > down_to) {
    const std::size_t at = vm_.iterations.size() - 1;
    // By reference into the stack for the reason CloseIterations takes one:
    // that is what keeps the iterator a root while its own `return` runs.
    const Result closed = CloseIterationCursor(vm_.iterations[at]);
    vm_.iterations.resize(at);
    // Discarded on purpose. See the note on the declaration.
    (void)closed;
  }
}

Result Interpreter::StepIteration(Iteration& state, Value& value_out, bool& done) {
  return StepIterationWith(state, Value::Undefined(), value_out, done);
}

Result Interpreter::StepIterationWith(Iteration& state, const Value& sent, Value& value_out,
                                      bool& done) {
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
  const Result stepped = CallFunction(state.next, state.iterator, {sent});
  if (stepped.IsAbrupt()) {
    return stepped;
  }
  if (!stepped.value.IsObject()) {
    return Throw("TypeError", "the iterator returned a non-object");
  }
  if (ToBoolean(GetProperty(stepped.value, "done"))) {
    done = true;
    // The `value` of the final result, which every other caller ignores and
    // `yield*` is defined in terms of: `const x = yield* g()` is what `g`
    // returned, not what it last yielded. Read here rather than left undefined
    // because the result object is in hand and reading it costs one lookup on
    // the step that ends an iteration.
    value_out = GetProperty(stepped.value, "value");
    return Result::Normal();
  }
  value_out = GetProperty(stepped.value, "value");
  return Result::Normal();
}

Result Interpreter::ForwardToDelegate(const Value& thrown, bool& finished) {
  // What `yield*` does with a throw or a forced return that arrived at its
  // `yield`. The cursor is the delegate's, still open on the machine's stack
  // -- the handler that caught this recorded a depth that includes it, which
  // is what keeps it from being closed before this runs.
  finished = false;
  Iteration& cursor = vm_.iterations.back();
  const bool is_return = IsReturnSignal(thrown);
  Value payload = thrown;
  if (is_return) {
    const Value* held = thrown.object->GetOwn("value");
    payload = held == nullptr ? Value::Undefined() : *held;
  }
  Result answered;
  bool done = false;
  if (!cursor.done && ForwardToIterator(cursor, payload, is_return, answered, done)) {
    if (answered.IsAbrupt()) {
      return answered;
    }
    if (done) {
      // The delegate finished answering. `return` ends the outer generator
      // too, carrying what the delegate returned; `throw` ends only the
      // delegation, and its value is what the expression is worth.
      if (is_return) {
        return Result{Completion::Throw, NewReturnSignal(answered.value), {}};
      }
      finished = true;
    }
    return Result::Normal(answered.value);
  }
  // Nothing to forward to. The delegate is closed and the original completion
  // carries on outwards -- except for a throw at an iterator with no `throw`
  // method, which the spec makes a TypeError after closing it.
  const Result closed = CloseIterationCursor(cursor);
  (void)closed;
  const bool had_iterator = cursor.iterator.IsObject();
  cursor.done = true;
  if (is_return || !had_iterator) {
    return Result{Completion::Throw, thrown, {}};
  }
  return Throw("TypeError", "the iterator has no throw method");
}

bool Interpreter::ForwardToIterator(Iteration& state, const Value& thrown, bool is_return,
                                    Result& out, bool& done) {
  done = false;
  if (state.array != nullptr || !state.iterator.IsObject()) {
    // The fast path walks an array by index and has no iterator object to
    // forward to. Nothing is owed: an array has no `finally` to run.
    return false;
  }
  const Value method = GetProperty(state.iterator, is_return ? "return" : "throw");
  if (!method.IsObject() || !method.object->IsCallable()) {
    // No method to forward to. A `return` this way is the ordinary case -- most
    // iterators have none -- and the caller closes and rethrows. A `throw`
    // this way is the spec's TypeError, which the caller raises after closing:
    // an iterator that cannot be thrown at must still be told to stop.
    return false;
  }
  const Result answered = CallFunction(method, state.iterator, {thrown});
  if (answered.IsAbrupt()) {
    out = answered;
    return true;
  }
  if (!answered.value.IsObject()) {
    out = Throw("TypeError", "the iterator returned a non-object");
    return true;
  }
  done = ToBoolean(GetProperty(answered.value, "done"));
  if (done) {
    state.done = true;
  }
  out = Result::Normal(GetProperty(answered.value, "value"));
  return true;
}

}  // namespace microbrowser::js

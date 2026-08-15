#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Collections.h"
#include "js/Interpreter.h"

// Map and Set.
//
// The storage is split deliberately. Entries live in an ordinary JavaScript
// array hanging off the collection object, which buys insertion order and gets
// every key and value marked by the collector without a line of new marking
// code. The index from key to position lives beside the object in the heap,
// holds no references, and is what stops `map.get(k)` from being a scan --
// without it, filling a ten-thousand-entry Map is fifty million comparisons,
// which a framework does on page load.
//
// A deleted entry leaves a hole rather than shifting what follows: every
// position in the index would otherwise be wrong, and re-indexing per delete
// turns clearing a Map into O(n^2). Holes are compacted once they outnumber
// what is left.

namespace microbrowser::js {

namespace {

// The entries array, and the one slot on the object that is not a property a
// page put there.
constexpr const char* kEntriesKey = "#entries";

// An entry is a two-element array for a Map and a one-element array for a Set.
// A hole is null, which no entry ever is -- which is what lets a Set hold
// `undefined` and `null` as ordinary members.
bool IsHole(const Value& entry) { return !entry.IsObject(); }

Object* EntriesOf(const Value& self) {
  if (!self.IsObject()) {
    return nullptr;
  }
  const Value* entries = self.object->GetOwn(kEntriesKey);
  return entries != nullptr && entries->IsObject() ? entries->object : nullptr;
}

// The live entry count, which is the array's length less its holes.
double SizeOf(NativeCall& call) {
  Object* entries = EntriesOf(call.self);
  const MapIndex* index = call.self.IsObject()
                              ? call.interpreter.GetHeap().FindMapIndex(call.self.object)
                              : nullptr;
  if (entries == nullptr || index == nullptr) {
    return 0.0;
  }
  return static_cast<double>(entries->ElementCount() - index->holes);
}

// Removes the holes and rebuilds the index. Runs only when holes outnumber
// live entries, so the total work stays linear in the entries ever added.
void Compact(Object* entries, MapIndex& index, bool with_values) {
  std::vector<Value> kept;
  kept.reserve(entries->ElementCount() - index.holes);
  for (std::size_t i = 0; i < entries->ElementCount(); ++i) {
    const Value entry = entries->GetElement(i);
    if (!IsHole(entry)) {
      kept.push_back(entry);
    }
  }
  index.positions.clear();
  index.holes = 0;
  for (std::size_t i = 0; i < kept.size(); ++i) {
    index.positions.emplace(ValueKey::From(kept[i].object->GetElement(0)), i);
  }
  (void)with_values;
  entries->SetElements(std::move(kept), {});
}

// Everything a collection method needs, or nothing when the receiver is not
// one of ours. `map.get.call({})` is a TypeError, not a crash.
struct Access {
  Object* entries = nullptr;
  MapIndex* index = nullptr;
  bool ok = false;
};

Access Open(NativeCall& call) {
  Access access;
  access.entries = EntriesOf(call.self);
  access.index = call.self.IsObject()
                     ? call.interpreter.GetHeap().FindMapIndex(call.self.object)
                     : nullptr;
  access.ok = access.entries != nullptr && access.index != nullptr;
  return access;
}

// The entry for `key`, or null. Also reports where it sits, which delete needs.
Value FindEntry(const Access& access, const Value& key, std::size_t& position) {
  const auto found = access.index->positions.find(ValueKey::From(key));
  if (found == access.index->positions.end()) {
    return Value::Undefined();
  }
  position = found->second;
  if (position >= access.entries->ElementCount()) {
    return Value::Undefined();
  }
  const Value entry = access.entries->GetElement(position);
  return IsHole(entry) ? Value::Undefined() : entry;
}

}  // namespace

void Interpreter::InstallCollections() {
  // One prototype each, but almost one implementation: a Set is a Map whose
  // entries have no second element, and every method below that differs
  // between them differs only in that.
  const auto build = [this](const char* name, bool with_values) -> Object* {
    Object* prototype = NewObject();
    if (prototype == nullptr) {
      return nullptr;
    }

    Object* constructor = NewNative(name, [with_values](NativeCall& call) {
      // `class Cache extends Map` reaches here through `super()` with the
      // instance already allocated, and wants the entries and the index
      // attached to *that* -- a second object allocated here is one nobody
      // would ever see, and every method would then throw "not a Map".
      Value collection = Value::Undefined();
      if (Object* target = ConstructionTarget(call)) {
        collection = Value::Obj(target);
      } else {
        collection = call.interpreter.NewObjectValue();
        if (!collection.IsObject()) {
          return call.Throw("RangeError", "out of memory");
        }
        const Value* prototype_value =
            call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
        if (prototype_value != nullptr && prototype_value->IsObject()) {
          collection.object->SetPrototype(prototype_value->object);
        }
      }
      collection.object->SetHidden(kEntriesKey, call.interpreter.NewArrayValue({}));
      call.interpreter.GetHeap().AttachMapIndex(collection.object);

      // `new Map(pairs)` and `new Set(values)` both take any iterable, which
      // is the whole reason this landed after the iteration protocol.
      const Value source = Argument(call.arguments, 0);
      if (source.IsNullish()) {
        return collection;
      }
      std::vector<Value> initial;
      const Result collected = call.interpreter.CollectIterable(source, initial);
      if (collected.IsAbrupt()) {
        return call.ThrowValue(collected.value);
      }
      const Value adder = call.interpreter.GetPropertyValue(
          collection, with_values ? PropertyKey("set") : PropertyKey("add"));
      for (const Value& item : initial) {
        std::vector<Value> arguments;
        if (with_values) {
          // Each item of a Map's source is itself a [key, value] pair, and it
          // is read with the protocol too rather than by index -- the pairs a
          // page passes are often not arrays.
          std::vector<Value> pair;
          const Result unpacked = call.interpreter.CollectIterable(item, pair);
          if (unpacked.IsAbrupt()) {
            return call.ThrowValue(unpacked.value);
          }
          arguments.push_back(pair.empty() ? Value::Undefined() : pair[0]);
          arguments.push_back(pair.size() < 2 ? Value::Undefined() : pair[1]);
        } else {
          arguments.push_back(item);
        }
        const Result added = call.interpreter.CallFunction(adder, collection, arguments);
        if (added.IsAbrupt()) {
          return call.ThrowValue(added.value);
        }
      }
      return collection;
    });
    if (constructor == nullptr) {
      return nullptr;
    }
    constructor->Set("prototype", Value::Obj(prototype));
    prototype->SetHidden("constructor", Value::Obj(constructor));
    realm_->global_scope->Declare(name, Value::Obj(constructor), false);

    // `size` is a getter, not a stored count: a stored one is a second thing
    // that can disagree with the entries, and it would have to be right after
    // every add, delete and clear rather than in one place.
    if (Object* size = NewNative("size", [](NativeCall& call) {
          return Value::Number(SizeOf(call));
        })) {
      prototype->DefineAccessor("size", size, nullptr);
    }

    const auto method = [this, prototype](const char* method_name, NativeFunction function) {
      InstallNative(prototype, method_name, std::move(function));
    };

    method("has", [](NativeCall& call) {
      const Access access = Open(call);
      if (!access.ok) {
        return call.Throw("TypeError", "not a Map or Set");
      }
      std::size_t position = 0;
      return Value::Bool(FindEntry(access, Argument(call.arguments, 0), position).IsObject());
    });

    method("delete", [](NativeCall& call) {
      const Access access = Open(call);
      if (!access.ok) {
        return call.Throw("TypeError", "not a Map or Set");
      }
      const Value key = Argument(call.arguments, 0);
      std::size_t position = 0;
      if (!FindEntry(access, key, position).IsObject()) {
        return Value::Bool(false);
      }
      access.entries->SetElement(position, Value::Null());
      access.index->positions.erase(ValueKey::From(key));
      ++access.index->holes;
      if (access.index->holes > access.entries->ElementCount() - access.index->holes) {
        Compact(access.entries, *access.index, true);
      }
      return Value::Bool(true);
    });

    method("clear", [](NativeCall& call) {
      const Access access = Open(call);
      if (!access.ok) {
        return call.Throw("TypeError", "not a Map or Set");
      }
      access.entries->SetElements({}, {});
      access.index->positions.clear();
      access.index->holes = 0;
      return Value::Undefined();
    });

    // `forEach` and the three iterator-producing methods walk the entries
    // array directly rather than through the protocol, because they *are* what
    // the protocol is implemented in terms of.
    method("forEach", [with_values](NativeCall& call) {
      const Access access = Open(call);
      if (!access.ok) {
        return call.Throw("TypeError", "not a Map or Set");
      }
      const Value callback = Argument(call.arguments, 0);
      if (!callback.IsObject() || !callback.object->IsCallable()) {
        return call.Throw("TypeError", "forEach requires a function");
      }
      const Value receiver = Argument(call.arguments, 1);
      // Re-read the length each step: adding during a forEach is legal and the
      // spec visits what was added.
      for (std::size_t i = 0; i < access.entries->ElementCount(); ++i) {
        const Value entry = access.entries->GetElement(i);
        if (IsHole(entry)) {
          continue;
        }
        const Value key = entry.object->GetElement(0);
        const Value item = with_values ? entry.object->GetElement(1) : key;
        // (value, key, collection). A Set passes its member as both, which is
        // what makes a callback written for a Map work on one.
        const Result called =
            call.interpreter.CallFunction(callback, receiver, {item, key, call.self});
        if (called.IsAbrupt()) {
          return call.ThrowValue(called.value);
        }
      }
      return Value::Undefined();
    });

    return prototype;
  };

  // --- WeakRef --------------------------------------------------------------
  //
  // A reference the collector does not follow -- except that here it does.
  //
  // The honest version needs the mark phase to skip this edge and the sweep to
  // clear it, which is exactly what the weak tables already do. What it also
  // needs, and what makes it different from them, is a rule about *when* the
  // cleared reference becomes visible: the spec says a WeakRef must keep its
  // target alive for the rest of the turn once `deref` has returned it, or a
  // page can observe collection timing and two runs of the same script can
  // disagree. Implementing that requires a per-turn keepalive set this engine
  // does not have.
  //
  // So this one holds its target strongly and `deref` always answers. A page
  // that uses a WeakRef as a cache gets a cache that never evicts, which is a
  // memory cost and not a wrong answer; a page that uses one to *detect*
  // collection sees nothing collected, which is the one thing the spec
  // promises it may not rely on anyway.
  if (Object* weak_ref = NewNative("WeakRef", [](NativeCall& call) {
        const Value target = Argument(call.arguments, 0);
        if (!target.IsObject() && !target.IsSymbol()) {
          return call.Throw("TypeError", "a WeakRef target must be an object");
        }
        Object* made = ConstructionTarget(call);
        if (made == nullptr) {
          made = call.interpreter.GetHeap().AllocateObject(Object::Kind::Plain);
          if (made == nullptr) {
            return call.Throw("RangeError", "out of memory");
          }
          const Value* prototype =
              call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
          if (prototype != nullptr && prototype->IsObject()) {
            made->SetPrototype(prototype->object);
          }
        }
        made->SetHidden("#target", target);
        return Value::Obj(made);
      })) {
    Object* prototype = NewObject();
    if (prototype != nullptr) {
      InstallNative(prototype, "deref", [](NativeCall& call) {
        const Value* target =
            call.self.IsObject() ? call.self.object->GetOwn("#target") : nullptr;
        return target == nullptr ? Value::Undefined() : *target;
      });
      weak_ref->Set("prototype", Value::Obj(prototype));
      prototype->SetHidden("constructor", Value::Obj(weak_ref));
    }
    realm_->global_scope->Declare("WeakRef", Value::Obj(weak_ref), false);
  }

  // --- FinalizationRegistry -------------------------------------------------
  //
  // Registers a callback to run after a value is collected. Nothing here ever
  // calls one, and that is a conforming implementation: the spec says a
  // registry *may* never call its callback, precisely so that an engine is
  // free to collect on its own schedule. A page that treats it as a
  // notification would be relying on something no engine promises.
  //
  // It exists so that a page can construct one and register with it without
  // getting a ReferenceError, which is what a feature detection needs.
  if (Object* registry = NewNative("FinalizationRegistry", [](NativeCall& call) {
        const Value callback = Argument(call.arguments, 0);
        if (!callback.IsObject() || !callback.object->IsCallable()) {
          return call.Throw("TypeError", "a FinalizationRegistry needs a callback");
        }
        Object* made = ConstructionTarget(call);
        if (made == nullptr) {
          made = call.interpreter.GetHeap().AllocateObject(Object::Kind::Plain);
          if (made == nullptr) {
            return call.Throw("RangeError", "out of memory");
          }
          const Value* prototype =
              call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
          if (prototype != nullptr && prototype->IsObject()) {
            made->SetPrototype(prototype->object);
          }
        }
        return Value::Obj(made);
      })) {
    Object* prototype = NewObject();
    if (prototype != nullptr) {
      InstallNative(prototype, "register", [](NativeCall&) { return Value::Undefined(); });
      InstallNative(prototype, "unregister", [](NativeCall&) { return Value::Bool(false); });
      registry->Set("prototype", Value::Obj(prototype));
      prototype->SetHidden("constructor", Value::Obj(registry));
    }
    realm_->global_scope->Declare("FinalizationRegistry", Value::Obj(registry), false);
  }

  Object* map_prototype = build("Map", true);
  Object* set_prototype = build("Set", false);
  if (map_prototype == nullptr || set_prototype == nullptr) {
    return;
  }

  // --- What the two do not share -------------------------------------------

  InstallNative(map_prototype, "set", [](NativeCall& call) {
    const Access access = Open(call);
    if (!access.ok) {
      return call.Throw("TypeError", "not a Map");
    }
    const Value key = Argument(call.arguments, 0);
    const Value value = Argument(call.arguments, 1);
    std::size_t position = 0;
    const Value existing = FindEntry(access, key, position);
    if (existing.IsObject()) {
      // An existing key keeps its place. `map.set(k, 1); map.set(k, 2)` leaves
      // one entry where the first one was, not two, and not one at the end.
      existing.object->SetElement(1, value);
      return call.self;
    }
    if (access.entries->ElementCount() >= kMaxAllocationLength) {
      return call.Throw("RangeError", "too many entries");
    }
    const Value entry = call.interpreter.NewArrayValue({key, value});
    if (!entry.IsObject()) {
      return call.Throw("RangeError", "out of memory");
    }
    access.index->positions.emplace(ValueKey::From(key), access.entries->ElementCount());
    access.entries->PushElement(entry);
    return call.self;
  });

  InstallNative(map_prototype, "get", [](NativeCall& call) {
    const Access access = Open(call);
    if (!access.ok) {
      return call.Throw("TypeError", "not a Map");
    }
    std::size_t position = 0;
    const Value entry = FindEntry(access, Argument(call.arguments, 0), position);
    return entry.IsObject() ? entry.object->GetElement(1) : Value::Undefined();
  });

  InstallNative(set_prototype, "add", [](NativeCall& call) {
    const Access access = Open(call);
    if (!access.ok) {
      return call.Throw("TypeError", "not a Set");
    }
    const Value member = Argument(call.arguments, 0);
    std::size_t position = 0;
    if (FindEntry(access, member, position).IsObject()) {
      return call.self;
    }
    if (access.entries->ElementCount() >= kMaxAllocationLength) {
      return call.Throw("RangeError", "too many entries");
    }
    const Value entry = call.interpreter.NewArrayValue({member});
    if (!entry.IsObject()) {
      return call.Throw("RangeError", "out of memory");
    }
    access.index->positions.emplace(ValueKey::From(member), access.entries->ElementCount());
    access.entries->PushElement(entry);
    return call.self;
  });

  // --- WeakMap and WeakSet --------------------------------------------------
  //
  // Nothing above is reused, and that is the point. A Map holds its keys
  // alive; a WeakMap does not, and the difference is not a policy on top of
  // the same storage but a different place to store things -- the collector's,
  // because only the collector can say whether a key is still reachable.
  //
  // What they cannot do follows from that: there is no `size`, no iteration
  // and no `clear`. Every one of those would let a page observe *when* a
  // collection ran, which is exactly the observation weak references exist to
  // withhold.
  const auto weak = [this](const char* name, bool with_values) {
    Object* prototype = NewObject();
    if (prototype == nullptr) {
      return;
    }
    Object* constructor = NewNative(name, [with_values](NativeCall& call) {
      Value collection = call.interpreter.NewObjectValue();
      if (!collection.IsObject()) {
        return call.Throw("RangeError", "out of memory");
      }
      const Value* prototype_value =
          call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
      if (prototype_value != nullptr && prototype_value->IsObject()) {
        collection.object->SetPrototype(prototype_value->object);
      }
      call.interpreter.GetHeap().MakeWeakTable(collection.object);

      const Value source = Argument(call.arguments, 0);
      if (source.IsNullish()) {
        return collection;
      }
      std::vector<Value> initial;
      const Result collected = call.interpreter.CollectIterable(source, initial);
      if (collected.IsAbrupt()) {
        return call.ThrowValue(collected.value);
      }
      for (const Value& item : initial) {
        Value key = item;
        Value value = Value::Bool(true);
        if (with_values) {
          std::vector<Value> pair;
          const Result unpacked = call.interpreter.CollectIterable(item, pair);
          if (unpacked.IsAbrupt()) {
            return call.ThrowValue(unpacked.value);
          }
          key = pair.empty() ? Value::Undefined() : pair[0];
          value = pair.size() < 2 ? Value::Undefined() : pair[1];
        }
        if (!key.IsObject()) {
          return call.Throw("TypeError", "a weak key must be an object");
        }
        call.interpreter.GetHeap().WeakSet(collection.object, key.object, value);
      }
      return collection;
    });
    if (constructor == nullptr) {
      return;
    }
    prototype->SetPrototype(intrinsics().object_prototype);
    constructor->Set("prototype", Value::Obj(prototype));
    prototype->SetHidden("constructor", Value::Obj(constructor));
    realm_->global_scope->Declare(name, Value::Obj(constructor), false);

    // A key must be an object. A primitive has no identity to be weak about --
    // there is nothing for the collector to notice the death of -- so the spec
    // makes it a TypeError rather than a key that never collects.
    const auto key_of = [](NativeCall& call) -> Object* {
      const Value key = Argument(call.arguments, 0);
      return key.IsObject() ? key.object : nullptr;
    };
    InstallNative(prototype, "has", [key_of](NativeCall& call) {
      Object* key = key_of(call);
      return Value::Bool(key != nullptr && call.self.IsObject() &&
                         call.interpreter.GetHeap().WeakGet(call.self.object, key) != nullptr);
    });
    InstallNative(prototype, "delete", [key_of](NativeCall& call) {
      Object* key = key_of(call);
      return Value::Bool(key != nullptr && call.self.IsObject() &&
                         call.interpreter.GetHeap().WeakDelete(call.self.object, key));
    });
    if (with_values) {
      InstallNative(prototype, "get", [key_of](NativeCall& call) {
        Object* key = key_of(call);
        if (key == nullptr || !call.self.IsObject()) {
          return Value::Undefined();
        }
        const Value* found = call.interpreter.GetHeap().WeakGet(call.self.object, key);
        return found == nullptr ? Value::Undefined() : *found;
      });
      InstallNative(prototype, "set", [key_of](NativeCall& call) {
        Object* key = key_of(call);
        if (key == nullptr) {
          return call.Throw("TypeError", "a WeakMap key must be an object");
        }
        if (call.self.IsObject()) {
          call.interpreter.GetHeap().WeakSet(call.self.object, key,
                                             Argument(call.arguments, 1));
        }
        return call.self;
      });
    } else {
      InstallNative(prototype, "add", [key_of](NativeCall& call) {
        Object* key = key_of(call);
        if (key == nullptr) {
          return call.Throw("TypeError", "a WeakSet member must be an object");
        }
        if (call.self.IsObject()) {
          call.interpreter.GetHeap().WeakSet(call.self.object, key, Value::Bool(true));
        }
        return call.self;
      });
    }
  };
  weak("WeakMap", true);
  weak("WeakSet", false);

  // --- The iterators --------------------------------------------------------

  // What one step yields: the key, the value, or both as a pair. A Set's
  // `entries()` yields `[member, member]`, which is what makes a Set usable
  // wherever a Map is.
  enum class Yield { Keys, Values, Entries };

  const auto install_iterator = [this](Object* prototype, const char* name, Yield yield,
                                       bool with_values) {
    InstallNative(prototype, name, [yield, with_values](NativeCall& call) {
      Value iterator = call.interpreter.NewObjectValue();
      if (!iterator.IsObject() || !call.self.IsObject()) {
        return iterator;
      }
      // The collection itself, so the iterator sees entries added while it
      // runs -- which the spec requires -- and so the entries stay reachable.
      iterator.object->SetHidden("#of", call.self);
      iterator.object->SetHidden("#at", Value::Number(0.0));
      iterator.object->Set(
          "next", call.interpreter.NewNativeValue("next", [yield, with_values](NativeCall& step) {
            Value result = step.interpreter.NewObjectValue();
            if (!result.IsObject() || !step.self.IsObject()) {
              return result;
            }
            const Value* of = step.self.object->GetOwn("#of");
            const Value* at = step.self.object->GetOwn("#at");
            Object* entries = of == nullptr ? nullptr : EntriesOf(*of);
            std::size_t position = at == nullptr ? 0 : static_cast<std::size_t>(ToNumber(*at));
            while (entries != nullptr && position < entries->ElementCount() &&
                   IsHole(entries->GetElement(position))) {
              ++position;  // skip the holes a delete left
            }
            if (entries == nullptr || position >= entries->ElementCount()) {
              result.object->Set("value", Value::Undefined());
              result.object->Set("done", Value::Bool(true));
              return result;
            }
            const Value entry = entries->GetElement(position);
            const Value key = entry.object->GetElement(0);
            const Value value = with_values ? entry.object->GetElement(1) : key;
            Value yielded = key;
            if (yield == Yield::Values) {
              yielded = value;
            } else if (yield == Yield::Entries) {
              yielded = step.interpreter.NewArrayValue({key, value});
            }
            step.self.object->SetHidden("#at", Value::Number(static_cast<double>(position + 1)));
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

  install_iterator(map_prototype, "keys", Yield::Keys, true);
  install_iterator(map_prototype, "values", Yield::Values, true);
  install_iterator(map_prototype, "entries", Yield::Entries, true);
  install_iterator(set_prototype, "keys", Yield::Keys, false);
  install_iterator(set_prototype, "values", Yield::Values, false);
  install_iterator(set_prototype, "entries", Yield::Entries, false);

  // `for...of` over a Map yields pairs and over a Set yields members, which is
  // exactly `entries` and `values`. Aliased rather than reimplemented, so the
  // two can never drift apart.
  if (shared_.symbol_iterator != nullptr) {
    const PropertyKey hook = PropertyKey::Symbol(shared_.symbol_iterator);
    if (const Value* entries = map_prototype->GetOwn("entries")) {
      map_prototype->Set(hook, *entries);
    }
    if (const Value* values = set_prototype->GetOwn("values")) {
      set_prototype->Set(hook, *values);
    }
  }
}

}  // namespace microbrowser::js

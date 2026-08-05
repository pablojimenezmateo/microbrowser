// `sessionStorage` and `localStorage`.
//
// ADR 0021 §1-4. Both are the same object with a different `Kind`, and neither can
// name the partition its data lives in -- see Storage.h for why that is the point
// rather than a limitation.
//
// Built as a `Proxy`, for the reason `element.style` is one: the API is half methods
// and half arbitrary property names. `storage.getItem("token")` and `storage.token`
// are the same read, `storage.token = "x"` is a write, `delete storage.token` is a
// removal, and `Object.keys(storage)` enumerates. A plain object cannot answer a name
// nobody enumerated in advance, and real pages use the property form constantly --
// `localStorage.theme` is more common in the wild than `getItem`.
//
// The synchronous cost ADR 0021 §4 names is not paid here: every read is a lookup in
// memory. That is the whole reason the persistence tier loads a store *before* the
// document exists rather than on first `getItem`, and why no part of this file can
// block.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Storage.h"
#include "js/Interpreter.h"
#include "js/Value.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Which store this object is. On the *function* object rather than in a capture, for
// the reason BindingSupport gives: a capture holding state the collector cannot see is
// the bug this module has had once.
constexpr const char* kStorageKindSlot = "#storage-kind";

// The method names, which are properties of the object rather than of a prototype
// because a `Proxy`'s `get` trap is the only thing that ever answers here. A page that
// asks for `getItem` gets a function; a page that asks for `getItemm` gets undefined,
// which is what a typo should do.
bool IsMethodName(const std::string& name) {
  return name == "getItem" || name == "setItem" || name == "removeItem" ||
         name == "clear" || name == "key";
}

StorageSource::Kind KindOf(const NativeCall& call) {
  const Value* slot = call.callee == nullptr ? nullptr : call.callee->GetOwn(kStorageKindSlot);
  return slot != nullptr && slot->IsNumber() && slot->number != 0.0 ? StorageSource::Kind::Local
                                                                    : StorageSource::Kind::Session;
}

Value KindValue(StorageSource::Kind kind) {
  return Value::Number(kind == StorageSource::Kind::Local ? 1.0 : 0.0);
}

}  // namespace

void DomBindings::InstallStorage() {
  // An opaque origin -- a `data:` URL, `about:blank` -- is not a site and has no
  // partition to key a store by, so **neither name is declared for one**. Chrome and
  // Firefox instead throw `SecurityError` on access; absence is the same answer in the
  // form this codebase argues for (ADR 0012) and it is the one that survives feature
  // detection: `if (window.localStorage)` takes the fallback path rather than the
  // native one.
  //
  // It is decided here, at install time, because it is a fact about the document and
  // the document does not change under one interpreter. A per-operation refusal was
  // written first and could not report itself: an exception thrown from a `Proxy` trap
  // is currently swallowed by `Interpreter::GetProperty`, which has no way to return an
  // abrupt completion -- see docs/js-conformance-roadmap.md.
  if (storage_ == nullptr || !storage_->Available(StorageSource::Kind::Local)) {
    // No store behind this layer, so neither name exists. An absence rather than a
    // stub, and this is a case where the difference is visible in one line of real
    // code: `try { localStorage.setItem(k, v) } catch { useCookie() }` recovers from a
    // throw, but `if (window.localStorage)` followed by an unguarded write does not --
    // and the second shape is what a store that exists and always fails would find.
    return;
  }

  const auto make = [this](StorageSource::Kind kind) -> Value {
    const Value target = interpreter_->NewObjectValue();
    const Value handler = interpreter_->NewObjectValue();
    if (!target.IsObject() || !handler.IsObject()) {
      return Value::Undefined();
    }

    // --- get ---------------------------------------------------------------
    const Value getter =
        interpreter_->NewNativeValue("get", [](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          if (owner == nullptr || owner->storage_ == nullptr) {
            return Value::Undefined();
          }
          const StorageSource::Kind this_kind = KindOf(call);
          const Value key = Argument(call.arguments, 1);
          if (key.IsSymbol()) {
            // No protocol hooks on a storage object: `Symbol.iterator` and the rest
            // are not part of the interface, and answering one would make
            // `for (const x of localStorage)` do something no other browser does.
            return Value::Undefined();
          }
          const std::string name = js::ToString(key);
          if (name == "length") {
            return Value::Number(static_cast<double>(owner->storage_->Length(this_kind)));
          }
          if (!IsMethodName(name)) {
            // A plain property read *is* `getItem`, and a missing key reads back as
            // `undefined` here where `getItem` returns `null`. That asymmetry is the
            // specification's, not an oversight: `storage.missing` is a property that
            // is not there and `getItem("missing")` is a lookup that found nothing.
            const std::optional<std::string> value = owner->storage_->GetItem(this_kind, name);
            return value.has_value() ? Value::String(*value) : Value::Undefined();
          }
          const Value method = owner->interpreter_->NewNativeValue(
              name.c_str(), [](NativeCall& inner) -> Value {
                DomBindings* method_owner = OwnerOf(inner);
                if (method_owner == nullptr || method_owner->storage_ == nullptr) {
                  return Value::Undefined();
                }
                StorageSource* store = method_owner->storage_;
                const StorageSource::Kind inner_kind = KindOf(inner);
                const Value* which =
                    inner.callee == nullptr ? nullptr : inner.callee->GetOwn("#storage-method");
                const std::string method_name = which == nullptr ? std::string() : js::ToString(*which);

                if (method_name == "getItem") {
                  const std::string key_name = js::ToString(Argument(inner.arguments, 0));
                  const std::optional<std::string> value = store->GetItem(inner_kind, key_name);
                  // `null`, not `undefined`, and a page tests it with `=== null`.
                  return value.has_value() ? Value::String(*value) : Value::Null();
                }
                if (method_name == "setItem") {
                  const std::string key_name = js::ToString(Argument(inner.arguments, 0));
                  const std::string value = js::ToString(Argument(inner.arguments, 1));
                  const StorageSource::WriteResult result =
                      store->SetItem(inner_kind, key_name, value);
                  if (result == StorageSource::WriteResult::QuotaExceeded) {
                    // The specified failure, and one real pages handle because
                    // Safari's quotas trained them to. Thrown rather than returned:
                    // a `setItem` that silently did nothing is a page that believes it
                    // saved. `call.Throw` returns the abrupt result and it has to be
                    // *returned* -- a native function that calls Throw and then returns
                    // a value has thrown nothing, which is how this first landed.
                    return inner.Throw("QuotaExceededError",
                                       "storage quota exceeded for '" + key_name + "'");
                  }
                  return Value::Undefined();
                }
                if (method_name == "removeItem") {
                  store->RemoveItem(inner_kind, js::ToString(Argument(inner.arguments, 0)));
                  return Value::Undefined();
                }
                if (method_name == "clear") {
                  store->Clear(inner_kind);
                  return Value::Undefined();
                }
                // `key(n)`. A non-integer index is 0 after ToNumber, which is what
                // the specification's `unsigned long` conversion does, and an index
                // past the end is `null` rather than an error.
                const double index = js::ToNumber(Argument(inner.arguments, 0));
                if (index < 0.0 || index != index) {
                  return Value::Null();
                }
                const std::optional<std::string> found =
                    store->KeyAt(inner_kind, static_cast<std::size_t>(index));
                return found.has_value() ? Value::String(*found) : Value::Null();
              });
          if (method.IsObject()) {
            method.object->Set(kOwnerSlot, PointerValue(owner));
            method.object->Set(kStorageKindSlot, KindValue(this_kind));
            method.object->Set("#storage-method", Value::String(name));
          }
          return method;
        });
    if (!getter.IsObject()) {
      return Value::Undefined();
    }
    getter.object->Set(kOwnerSlot, PointerValue(this));
    getter.object->Set(kStorageKindSlot, KindValue(kind));
    handler.object->Set("get", getter);

    // --- set ---------------------------------------------------------------
    const Value setter = interpreter_->NewNativeValue("set", [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr || owner->storage_ == nullptr) {
        return Value::Bool(false);
      }
      const Value key = Argument(call.arguments, 1);
      if (key.IsSymbol()) {
        return Value::Bool(false);
      }
      const std::string name = js::ToString(key);
      const std::string value = js::ToString(Argument(call.arguments, 2));
      const StorageSource::WriteResult result =
          owner->storage_->SetItem(KindOf(call), name, value);
      if (result == StorageSource::WriteResult::QuotaExceeded) {
        return call.Throw("QuotaExceededError", "storage quota exceeded for '" + name + "'");
      }
      // True even for `length`, which is read-only in the specification and whose
      // assignment is silently ignored rather than an error. Returning false here
      // would make `storage.length = 3` throw in strict mode, which it does not.
      return Value::Bool(true);
    });
    if (setter.IsObject()) {
      setter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kStorageKindSlot, KindValue(kind));
      handler.object->Set("set", setter);
    }

    // --- deleteProperty ----------------------------------------------------
    const Value deleter =
        interpreter_->NewNativeValue("deleteProperty", [](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          if (owner == nullptr || owner->storage_ == nullptr) {
            return Value::Bool(false);
          }
          const Value key = Argument(call.arguments, 1);
          if (key.IsSymbol()) {
            return Value::Bool(false);
          }
          owner->storage_->RemoveItem(KindOf(call), js::ToString(key));
          // True whether or not the key was there: `delete` on a missing property is
          // true everywhere else in the language, and a page that deletes twice must
          // not see a difference.
          return Value::Bool(true);
        });
    if (deleter.IsObject()) {
      deleter.object->Set(kOwnerSlot, PointerValue(this));
      deleter.object->Set(kStorageKindSlot, KindValue(kind));
      handler.object->Set("deleteProperty", deleter);
    }

    // --- has ---------------------------------------------------------------
    const Value has = interpreter_->NewNativeValue("has", [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr || owner->storage_ == nullptr) {
        return Value::Bool(false);
      }
      const Value key = Argument(call.arguments, 1);
      if (key.IsSymbol()) {
        return Value::Bool(false);
      }
      const std::string name = js::ToString(key);
      if (name == "length" || IsMethodName(name)) {
        return Value::Bool(true);
      }
      return Value::Bool(owner->storage_->GetItem(KindOf(call), name).has_value());
    });
    if (has.IsObject()) {
      has.object->Set(kOwnerSlot, PointerValue(this));
      has.object->Set(kStorageKindSlot, KindValue(kind));
      handler.object->Set("has", has);
    }

    // --- ownKeys -----------------------------------------------------------
    // `Object.keys(localStorage)` is how a page enumerates what it saved, and the
    // *stored* keys are all it returns -- not `length`, not the methods. That is what
    // every other browser does and what a page that iterates its own keys expects.
    const Value own_keys =
        interpreter_->NewNativeValue("ownKeys", [](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          if (owner == nullptr || owner->storage_ == nullptr) {
            return Value::Undefined();
          }
          const StorageSource::Kind listed = KindOf(call);
          std::vector<Value> keys;
          const std::size_t length = owner->storage_->Length(listed);
          for (std::size_t i = 0; i < length; ++i) {
            const std::optional<std::string> key = owner->storage_->KeyAt(listed, i);
            if (key.has_value()) {
              keys.push_back(Value::String(*key));
            }
          }
          return call.interpreter.NewArrayValue(std::move(keys));
        });
    if (own_keys.IsObject()) {
      own_keys.object->Set(kOwnerSlot, PointerValue(this));
      own_keys.object->Set(kStorageKindSlot, KindValue(kind));
      handler.object->Set("ownKeys", own_keys);
    }

    js::Value* constructor = interpreter_->GlobalScope()->Lookup("Proxy");
    if (constructor == nullptr || !constructor->IsObject()) {
      return Value::Undefined();
    }
    const js::Result made =
        interpreter_->CallFunction(*constructor, Value::Undefined(), {target, handler});
    return made.IsAbrupt() ? Value::Undefined() : made.value;
  };

  const Value session = make(StorageSource::Kind::Session);
  const Value local = make(StorageSource::Kind::Local);
  if (session.IsObject()) {
    interpreter_->Global()->Set("sessionStorage", session);
    interpreter_->GlobalScope()->Declare("sessionStorage", session, false);
  }
  if (local.IsObject()) {
    interpreter_->Global()->Set("localStorage", local);
    interpreter_->GlobalScope()->Declare("localStorage", local, false);
  }
}

}  // namespace microbrowser::bindings

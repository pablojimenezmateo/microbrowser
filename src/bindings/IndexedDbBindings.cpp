// `indexedDB`: databases, object stores, indexes and the request/transaction
// objects a page drives them through. `IDBIndex` and `IDBCursor` are in
// IndexedDbRequests.cpp, split off once this file reached the module's cap.
//
// ADR 0038. Everything here is a *synchronous* call into
// `bindings::IndexedDbSource` -- the store is in memory, so there is nothing
// to wait for -- and the only thing `TimerQueue::QueueTask` defers is the
// completion *event*, exactly the way `MessagePort::postMessage` defers
// delivery rather than the send. That is what makes a promise built on one of
// these settle on a later turn rather than inside the call that made it,
// which is the specification's rule and the property a page's own scheduler
// relies on.
//
// **What is deliberately narrow.** No `autoIncrement`, no cross-type key
// ordering (see storage::IndexedDbKey::Encode), no versionchange transaction
// distinct from a normal one -- `createObjectStore`/`createIndex` are callable
// on any `IDBDatabase` this binds rather than only inside `upgradeneeded`,
// which is honest about what this engine actually checks rather than a
// half-enforced version of the restriction. Each absence is deliberate rather
// than a stub: see ADR 0012 and the note on `IDBKeyRange` in
// IndexedDbRequests.cpp.
//
// **A reload loses an index's keyPath.** `src/storage` cannot see `js`, so it
// cannot extract a key from a value -- only this binding layer can, which
// means an index's keyPath lives only as *this document's* metadata (see
// `RememberIdbIndexMeta`) and not in the partition's own schema. A second
// document of the same origin that reopens an existing database at the same
// version never calls `createIndex` again (nothing fires `upgradeneeded`),
// so `put()` there cannot populate that index. Recorded rather than fixed:
// fixing it means teaching `storage::IndexedDbIndexDef` a keyPath, which is a
// real widening of that module's contract and not a one-line change.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/IndexedDb.h"
#include "bindings/IndexedDbSupport.h"
#include "bindings/Timers.h"
#include "js/StructuredClone.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

constexpr const char* kIdbIndexMetaKey = "#idbIndexMeta";

std::string SlotString(const Value& value, const char* slot) {
  const Value* found = value.IsObject() ? value.object->GetOwn(slot) : nullptr;
  return found == nullptr ? std::string() : js::ToString(*found);
}

Value SlotValue(const Value& value, const char* slot) {
  const Value* found = value.IsObject() ? value.object->GetOwn(slot) : nullptr;
  return found == nullptr ? Value::Undefined() : *found;
}

double PendingCount(const Value& transaction) {
  const Value* found = transaction.IsObject() ? transaction.object->GetOwn(kIdbPendingSlot) : nullptr;
  return found == nullptr ? 0.0 : found->number;
}

bool FlagOn(const Value& value, const char* slot) {
  const Value* found = value.IsObject() ? value.object->GetOwn(slot) : nullptr;
  return found != nullptr && js::ToBoolean(*found);
}

std::string IdbIndexMetaName(const std::string& db, const std::string& store,
                             const std::string& index) {
  return db + "\x1e" + store + "\x1e" + index;
}

}  // namespace

js::Value DomBindings::IdbIndexMetaTable() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kIdbIndexMetaKey);
      existing != nullptr && existing->IsObject()) {
    return *existing;
  }
  const Value table = interpreter_->NewObjectValue();
  if (table.IsObject()) {
    interfaces_.object->Set(kIdbIndexMetaKey, table);
  }
  return table;
}

void DomBindings::RememberIdbIndexMeta(const std::string& db, const std::string& store,
                                       const std::string& index, const IndexedDbKeyPath& key_path,
                                       bool unique) {
  const Value table = IdbIndexMetaTable();
  if (!table.IsObject()) {
    return;
  }
  const Value entry = interpreter_->NewObjectValue();
  if (!entry.IsObject()) {
    return;
  }
  entry.object->Set("hasPath", Value::Bool(key_path.has_path));
  entry.object->Set("isArray", Value::Bool(key_path.is_array));
  entry.object->Set("unique", Value::Bool(unique));
  std::vector<Value> parts;
  parts.reserve(key_path.parts.size());
  for (const std::string& part : key_path.parts) {
    parts.push_back(Value::String(part));
  }
  entry.object->Set("parts", interpreter_->NewArrayValue(std::move(parts)));
  table.object->Set(IdbIndexMetaName(db, store, index), entry);
}

std::optional<IndexedDbKeyPath> DomBindings::IdbIndexKeyPath(const std::string& db,
                                                             const std::string& store,
                                                             const std::string& index) {
  const Value table = IdbIndexMetaTable();
  if (!table.IsObject()) {
    return std::nullopt;
  }
  const Value* entry = table.object->GetOwn(IdbIndexMetaName(db, store, index));
  if (entry == nullptr || !entry->IsObject() || !FlagOn(*entry, "hasPath")) {
    return std::nullopt;
  }
  IndexedDbKeyPath path;
  path.has_path = true;
  path.is_array = FlagOn(*entry, "isArray");
  if (const Value* parts = entry->object->GetOwn("parts"); parts != nullptr && parts->IsObject()) {
    for (std::size_t i = 0; i < parts->object->ElementCount(); ++i) {
      path.parts.push_back(js::ToString(parts->object->GetElement(i)));
    }
  }
  return path;
}

bool DomBindings::IdbIndexIsUnique(const std::string& db, const std::string& store,
                                   const std::string& index) {
  const Value table = IdbIndexMetaTable();
  if (!table.IsObject()) {
    return false;
  }
  const Value* entry = table.object->GetOwn(IdbIndexMetaName(db, store, index));
  return entry != nullptr && entry->IsObject() && FlagOn(*entry, "unique");
}

js::Value DomBindings::MakeIdbRequest(const char* interface_name, const js::Value& source,
                                      const js::Value& transaction) {
  const Value request = interpreter_->NewObjectValue();
  if (!request.IsObject()) {
    return Value::Undefined();
  }
  if (const Value proto = InterfaceNamed(interface_name); proto.IsObject()) {
    request.object->SetPrototype(proto.object);
  }
  request.object->Set("result", Value::Undefined());
  request.object->Set("error", Value::Null());
  request.object->Set("readyState", Value::String(std::string("pending")));
  request.object->Set("source", source);
  // Spec uses `null` when the request is not tied to a transaction. Leaving
  // `undefined` here made youtube's `a.transaction === null` check pass and
  // then `new v_(a.transaction)` throw on `addEventListener` of undefined.
  request.object->Set("transaction", transaction.IsObject() ? transaction : Value::Null());
  InstallEventMethods(request);
  if (transaction.IsObject()) {
    request.object->SetHidden(kIdbTxnSlot, transaction);
    transaction.object->SetHidden(kIdbPendingSlot, Value::Number(PendingCount(transaction) + 1.0));
  }
  return request;
}

void DomBindings::MaybeCompleteIdbTransaction(const js::Value& transaction) {
  if (!transaction.IsObject() || FlagOn(transaction, kIdbDoneSlot) ||
      PendingCount(transaction) > 0.0) {
    return;
  }
  transaction.object->SetHidden(kIdbDoneSlot, Value::Bool(true));
  const Value event = interpreter_->NewObjectValue();
  if (!event.IsObject()) {
    return;
  }
  if (const Value proto = InterfaceNamed("Event"); proto.IsObject()) {
    event.object->SetPrototype(proto.object);
  }
  event.object->Set("type", Value::String(std::string("complete")));
  event.object->Set("target", transaction);
  if (const Value* handler = transaction.object->GetOwn("#oncomplete");
      handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
    const js::Result outcome = interpreter_->CallFunction(*handler, transaction, {event});
    if (outcome.completion == js::Completion::Throw) {
      interpreter_->ReportUncaught(outcome.value, "IDBTransaction complete handler");
    }
  }
  if (const Value* dispatch = transaction.object->Get("dispatchEvent");
      dispatch != nullptr && dispatch->IsObject()) {
    const js::Result outcome = interpreter_->CallFunction(*dispatch, transaction, {event});
    if (outcome.completion == js::Completion::Throw) {
      interpreter_->ReportUncaught(outcome.value, "IDBTransaction complete listener");
    }
  }
}

void DomBindings::DeliverIdbEvent(const js::Value& request, const char* type,
                                  const char* handler_name) {
  const Value event = interpreter_->NewObjectValue();
  if (event.IsObject()) {
    if (const Value proto = InterfaceNamed("Event"); proto.IsObject()) {
      event.object->SetPrototype(proto.object);
    }
    event.object->Set("type", Value::String(std::string(type)));
    event.object->Set("target", request);
    event.object->Set("bubbles", Value::Bool(true));
    event.object->Set("cancelable", Value::Bool(true));
    if (const Value* handler = request.object->GetOwn(std::string("#") + handler_name);
        handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
      const js::Result outcome = interpreter_->CallFunction(*handler, request, {event});
      if (outcome.completion == js::Completion::Throw) {
        interpreter_->ReportUncaught(outcome.value, "IDBRequest handler");
      }
    }
    if (const Value* dispatch = request.object->Get("dispatchEvent");
        dispatch != nullptr && dispatch->IsObject()) {
      const js::Result outcome = interpreter_->CallFunction(*dispatch, request, {event});
      if (outcome.completion == js::Completion::Throw) {
        interpreter_->ReportUncaught(outcome.value, "IDBRequest listener");
      }
    }
  }
  const Value transaction = SlotValue(request, kIdbTxnSlot);
  if (transaction.IsObject()) {
    const double remaining = PendingCount(transaction) - 1.0;
    transaction.object->SetHidden(kIdbPendingSlot, Value::Number(remaining < 0.0 ? 0.0 : remaining));
    MaybeCompleteIdbTransaction(transaction);
  }
}

void DomBindings::DeliverIdbSuccess(const js::Value& request, const js::Value& result) {
  DomBindings* self = this;
  const Value deliver = interpreter_->NewNativeValue(
      "idbSuccess", [self, request, result](NativeCall&) -> Value {
        if (!request.IsObject()) {
          return Value::Undefined();
        }
        request.object->Set("result", result);
        request.object->Set("error", Value::Null());
        request.object->Set("readyState", Value::String(std::string("done")));
        self->DeliverIdbEvent(request, "success", "onsuccess");
        return Value::Undefined();
      });
  if (deliver.IsObject()) {
    // `request` and `result` are captures, invisible to the collector -- see
    // MessageChannels.cpp's `DispatchPortMessage` for the same line. Rooted
    // again as properties of the function object the timer queue keeps.
    deliver.object->Set("#request", request);
    deliver.object->Set("#result", result);
    TimerQueue::QueueTask(*interpreter_, deliver);
  }
}

void DomBindings::DeliverIdbError(const js::Value& request, const std::string& name,
                                  const std::string& message) {
  DomBindings* self = this;
  const Value deliver = interpreter_->NewNativeValue(
      "idbError", [self, request, name, message](NativeCall& call) -> Value {
        if (!request.IsObject()) {
          return Value::Undefined();
        }
        request.object->Set("result", Value::Undefined());
        request.object->Set("error", MakeDomException(call.interpreter, name, message));
        request.object->Set("readyState", Value::String(std::string("done")));
        self->DeliverIdbEvent(request, "error", "onerror");
        return Value::Undefined();
      });
  if (deliver.IsObject()) {
    deliver.object->Set("#request", request);
    TimerQueue::QueueTask(*interpreter_, deliver);
  }
}

js::Value DomBindings::MakeIdbObjectStore(const std::string& db, const std::string& store,
                                          const js::Value& transaction) {
  const Value wrapper = interpreter_->NewObjectValue();
  if (!wrapper.IsObject()) {
    return Value::Undefined();
  }
  if (const Value proto = InterfaceNamed("IDBObjectStore"); proto.IsObject()) {
    wrapper.object->SetPrototype(proto.object);
  }
  wrapper.object->SetHidden(kIdbDbSlot, Value::String(db));
  wrapper.object->SetHidden(kIdbStoreSlot, Value::String(store));
  wrapper.object->SetHidden(kIdbTxnSlot, transaction);
  wrapper.object->Set("name", Value::String(store));
  wrapper.object->Set("transaction", transaction);
  return wrapper;
}

js::Value DomBindings::MakeIdbTransaction(const js::Value& database, const std::string& db,
                                          const std::vector<std::string>& store_names,
                                          const std::string& mode) {
  const Value transaction = interpreter_->NewObjectValue();
  if (!transaction.IsObject()) {
    return Value::Undefined();
  }
  if (const Value proto = InterfaceNamed("IDBTransaction"); proto.IsObject()) {
    transaction.object->SetPrototype(proto.object);
  }
  transaction.object->SetHidden(kIdbDbSlot, Value::String(db));
  transaction.object->SetHidden(kIdbPendingSlot, Value::Number(0.0));
  transaction.object->Set("db", database);
  transaction.object->Set("mode", Value::String(mode));
  std::vector<Value> name_values;
  for (const std::string& name : store_names) {
    name_values.push_back(Value::String(name));
  }
  // Own data would make `in` true on the instance and false on the prototype —
  // which is exactly the shape `yPS` rejects. The names live here; the getter
  // on the prototype surfaces them.
  transaction.object->SetHidden(kIdbTxnStoreNamesSlot, interpreter_->NewArrayValue(name_values));
  InstallEventMethods(transaction);
  return transaction;
}

namespace {

// An array with `.contains`/`.item` beside the indices and `.length` a real
// array already has -- the "array-like is fine" shape `objectStoreNames` and
// `indexNames` need, without a shared `DOMStringList` prototype nothing else
// in this browser is an instance of.
Value MakeDomStringList(js::Interpreter& interpreter, const std::vector<Value>& names) {
  const Value list = interpreter.NewArrayValue(names);
  if (!list.IsObject()) {
    return list;
  }
  const Value contains = interpreter.NewNativeValue("contains", [](NativeCall& call) {
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    if (!call.self.IsObject()) {
      return Value::Bool(false);
    }
    for (std::size_t i = 0; i < call.self.object->ElementCount(); ++i) {
      if (js::ToString(call.self.object->GetElement(i)) == wanted) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  const Value item = interpreter.NewNativeValue("item", [](NativeCall& call) {
    const double index = js::ToNumber(Argument(call.arguments, 0));
    if (!call.self.IsObject() || index < 0.0 ||
        static_cast<std::size_t>(index) >= call.self.object->ElementCount()) {
      return Value::Null();
    }
    return call.self.object->GetElement(static_cast<std::size_t>(index));
  });
  if (contains.IsObject()) {
    list.object->Set("contains", contains);
  }
  if (item.IsObject()) {
    list.object->Set("item", item);
  }
  return list;
}

}  // namespace

void DomBindings::InstallIndexedDb() {
  if (indexed_db_ == nullptr || !indexed_db_->Available()) {
    // Absent rather than a store that refuses every write, for the reason
    // `localStorage` follows this rule (ADR 0012): a page that finds
    // `indexedDB` and gets one that always rejects has no fallback path left.
    return;
  }
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }

  const Value request_interface = MakeInterface("IDBRequest", InterfaceNamed("EventTarget"));
  const Value open_request_interface = MakeInterface("IDBOpenDBRequest", request_interface);
  const Value database_interface = MakeInterface("IDBDatabase", InterfaceNamed("EventTarget"));
  const Value transaction_interface = MakeInterface("IDBTransaction", InterfaceNamed("EventTarget"));
  const Value store_interface = MakeInterface("IDBObjectStore", Value::Undefined());
  if (!request_interface.IsObject() || !database_interface.IsObject() ||
      !transaction_interface.IsObject() || !store_interface.IsObject()) {
    return;
  }

  // `on<type>` handler properties, as accessors over a hidden slot rather
  // than plain data -- see `InstallOnEventAccessor` in EventBindings.cpp for
  // why a plain data property fires a handler twice. Defined once on each
  // shared prototype rather than per instance.
  InstallOnEventAccessor(request_interface, "onsuccess");
  InstallOnEventAccessor(request_interface, "onerror");
  InstallOnEventAccessor(open_request_interface, "onupgradeneeded");
  InstallOnEventAccessor(open_request_interface, "onblocked");
  InstallOnEventAccessor(transaction_interface, "oncomplete");
  InstallOnEventAccessor(transaction_interface, "onerror");
  InstallOnEventAccessor(transaction_interface, "onabort");

  DomBindings* self = this;
  IndexedDbSource* source = indexed_db_;

  // --- IDBDatabase -----------------------------------------------------------
  {
    const auto method = [this, &database_interface](const char* name, js::NativeFunction fn) {
      const Value native = interpreter_->NewNativeValue(name, std::move(fn));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, PointerValue(this));
        database_interface.object->Set(name, native);
      }
    };
    method("createObjectStore", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = js::ToString(Argument(call.arguments, 0));
      const Value options = Argument(call.arguments, 1);
      Value key_path_option = Value::Undefined();
      if (options.IsObject()) {
        if (const Value* found = options.object->Get("keyPath"); found != nullptr) {
          key_path_option = *found;
        }
      }
      const IndexedDbKeyPath key_path = ParseIdbKeyPath(key_path_option);
      if (!source->CreateObjectStore(db, store, key_path)) {
        return ThrowDom(call, "ConstraintError", "object store '" + store + "' already exists");
      }
      return self->MakeIdbObjectStore(db, store, Value::Undefined());
    });
    method("transaction", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const Value names_arg = Argument(call.arguments, 0);
      std::vector<std::string> store_names;
      if (names_arg.IsString()) {
        store_names.push_back(js::ToString(names_arg));
      } else if (names_arg.IsObject() && names_arg.object->GetKind() == js::Object::Kind::Array) {
        for (std::size_t i = 0; i < names_arg.object->ElementCount(); ++i) {
          store_names.push_back(js::ToString(names_arg.object->GetElement(i)));
        }
      }
      const std::string mode = call.arguments.size() > 1 ? js::ToString(call.arguments[1]) : "readonly";
      return self->MakeIdbTransaction(call.self, db, store_names, mode);
    });
    method("close", [](NativeCall& call) -> Value {
      if (call.self.IsObject()) {
        call.self.object->SetHidden("#idbClosed", Value::Bool(true));
      }
      return Value::Undefined();
    });
    const Value name_getter = interpreter_->NewNativeValue(
        "name", [](NativeCall& call) { return Value::String(SlotString(call.self, kIdbDbSlot)); });
    if (name_getter.IsObject()) {
      database_interface.object->DefineAccessor("name", name_getter.object, nullptr);
    }
    const Value version_getter = interpreter_->NewNativeValue("version", [](NativeCall& call) {
      const Value* v = call.self.IsObject() ? call.self.object->GetOwn("#idbVersion") : nullptr;
      return v == nullptr ? Value::Number(1.0) : *v;
    });
    if (version_getter.IsObject()) {
      database_interface.object->DefineAccessor("version", version_getter.object, nullptr);
    }
    const Value store_names_getter = interpreter_->NewNativeValue(
        "objectStoreNames", [self, source](NativeCall& call) {
          std::vector<Value> names;
          for (const std::string& name : source->ObjectStoreNames(SlotString(call.self, kIdbDbSlot))) {
            names.push_back(Value::String(name));
          }
          return MakeDomStringList(call.interpreter, names);
        });
    if (store_names_getter.IsObject()) {
      store_names_getter.object->Set(kOwnerSlot, PointerValue(this));
      database_interface.object->DefineAccessor("objectStoreNames", store_names_getter.object, nullptr);
    }
    for (const char* name : {"createObjectStore", "transaction", "close"}) {
      if (const Value* fn = database_interface.object->GetOwn(name); fn != nullptr && fn->IsObject()) {
        fn->object->Set(kOwnerSlot, PointerValue(this));
      }
    }
  }

  // --- IDBTransaction ---------------------------------------------------------
  {
    const Value object_store_method = interpreter_->NewNativeValue(
        "objectStore", [self](NativeCall& call) -> Value {
          const std::string db = SlotString(call.self, kIdbDbSlot);
          const std::string store = js::ToString(Argument(call.arguments, 0));
          return self->MakeIdbObjectStore(db, store, call.self);
        });
    if (object_store_method.IsObject()) {
      object_store_method.object->Set(kOwnerSlot, PointerValue(this));
      transaction_interface.object->Set("objectStore", object_store_method);
    }
    const Value abort_method = interpreter_->NewNativeValue("abort", [](NativeCall&) {
      return Value::Undefined();
    });
    if (abort_method.IsObject()) {
      transaction_interface.object->Set("abort", abort_method);
    }
    // Must be on the prototype: `yPS` does
    // `"objectStoreNames" in IDBTransaction.prototype` before any open, and
    // returns false for the whole feature if the answer is no — which is how
    // watch never reached `idb.opens` or `crypto.subtle_import_key` despite
    // every constructor name being present.
    const Value txn_names_getter = interpreter_->NewNativeValue(
        "objectStoreNames", [](NativeCall& call) {
          const Value names = SlotValue(call.self, kIdbTxnStoreNamesSlot);
          if (names.IsObject() && names.object->GetKind() == js::Object::Kind::Array) {
            std::vector<Value> values;
            for (std::size_t i = 0; i < names.object->ElementCount(); ++i) {
              values.push_back(names.object->GetElement(i));
            }
            return MakeDomStringList(call.interpreter, values);
          }
          return MakeDomStringList(call.interpreter, {});
        });
    if (txn_names_getter.IsObject()) {
      transaction_interface.object->DefineAccessor("objectStoreNames", txn_names_getter.object,
                                                   nullptr);
    }
  }

  // --- IDBObjectStore ----------------------------------------------------------
  {
    const auto method = [this, &store_interface](const char* name, js::NativeFunction fn) {
      const Value native = interpreter_->NewNativeValue(name, std::move(fn));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, PointerValue(this));
        store_interface.object->Set(name, native);
      }
    };
    method("get", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const std::optional<IndexedDbKeyValue> key = ValueToIdbKey(Argument(call.arguments, 0));
      const Value transaction = SlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      if (!key.has_value()) {
        self->DeliverIdbError(request, "DataError", "invalid key");
        return request;
      }
      util::AddPerformanceCounter(util::PerfCounterId::IdbGets);
      const std::optional<std::vector<std::uint8_t>> bytes = source->Get(db, store, *key);
      const Value value = bytes.has_value()
                              ? js::StructuredDeserialize(call.interpreter,
                                                          js::SerializedValue{*bytes})
                              : Value::Undefined();
      self->DeliverIdbSuccess(request, value);
      return request;
    });
    method("put", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const Value transaction = SlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      const Value record = Argument(call.arguments, 0);
      const IndexedDbKeyPath store_key_path = source->ObjectStoreKeyPath(db, store);
      std::optional<IndexedDbKeyValue> key;
      if (store_key_path.has_path) {
        key = ExtractIdbKey(record, store_key_path);
      } else {
        key = ValueToIdbKey(Argument(call.arguments, 1));
      }
      if (!key.has_value()) {
        self->DeliverIdbError(request, "DataError", "no key could be derived for this record");
        return request;
      }
      const std::optional<js::SerializedValue> serialized =
          js::StructuredSerialize(call.interpreter, record);
      if (!serialized.has_value()) {
        self->DeliverIdbError(request, "DataCloneError", "the record could not be cloned");
        return request;
      }
      std::vector<IndexedDbSource::IndexKeyEntry> index_keys;
      for (const std::string& index_name : source->IndexNames(db, store)) {
        const std::optional<IndexedDbKeyPath> index_path = self->IdbIndexKeyPath(db, store, index_name);
        if (!index_path.has_value()) {
          continue;
        }
        if (const std::optional<IndexedDbKeyValue> index_key = ExtractIdbKey(record, *index_path);
            index_key.has_value()) {
          index_keys.push_back(IndexedDbSource::IndexKeyEntry{index_name, *index_key});
        }
      }
      util::AddPerformanceCounter(util::PerfCounterId::IdbPuts);
      const IndexedDbSource::PutResult result =
          source->Put(db, store, *key, serialized->bytes, std::move(index_keys));
      switch (result) {
        case IndexedDbSource::PutResult::ConstraintError:
          util::AddPerformanceCounter(util::PerfCounterId::IdbConstraintErrors);
          self->DeliverIdbError(request, "ConstraintError", "key already exists in a unique index");
          break;
        case IndexedDbSource::PutResult::QuotaExceeded:
          util::AddPerformanceCounter(util::PerfCounterId::IdbQuotaRefusals);
          self->DeliverIdbError(request, "QuotaExceededError", "IndexedDB quota exceeded");
          break;
        case IndexedDbSource::PutResult::NotFound:
          self->DeliverIdbError(request, "InvalidStateError", "object store no longer exists");
          break;
        case IndexedDbSource::PutResult::Stored:
          self->DeliverIdbSuccess(request, IdbKeyToValue(call.interpreter, *key));
          break;
      }
      return request;
    });
    method("delete", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const std::optional<IndexedDbKeyValue> key = ValueToIdbKey(Argument(call.arguments, 0));
      const Value transaction = SlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      if (!key.has_value()) {
        self->DeliverIdbError(request, "DataError", "invalid key");
        return request;
      }
      util::AddPerformanceCounter(util::PerfCounterId::IdbDeletes);
      source->Delete(db, store, *key);
      self->DeliverIdbSuccess(request, Value::Undefined());
      return request;
    });
    method("clear", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const Value transaction = SlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      for (const IndexedDbSource::CursorEntry& entry : source->Query(db, store, "", std::nullopt)) {
        source->Delete(db, store, entry.primary_key);
      }
      self->DeliverIdbSuccess(request, Value::Undefined());
      return request;
    });
    method("getAll", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const Value transaction = SlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      const std::optional<IndexedDbKeyValue> only_key = QueryToIdbKey(Argument(call.arguments, 0));
      std::vector<Value> values;
      for (const IndexedDbSource::CursorEntry& entry : source->Query(db, store, "", only_key)) {
        values.push_back(
            js::StructuredDeserialize(call.interpreter, js::SerializedValue{entry.value}));
      }
      self->DeliverIdbSuccess(request, call.interpreter.NewArrayValue(std::move(values)));
      return request;
    });
    method("createIndex", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const std::string index = js::ToString(Argument(call.arguments, 0));
      const IndexedDbKeyPath key_path = ParseIdbKeyPath(Argument(call.arguments, 1));
      const Value options = Argument(call.arguments, 2);
      bool unique = false;
      if (options.IsObject()) {
        if (const Value* found = options.object->Get("unique"); found != nullptr) {
          unique = js::ToBoolean(*found);
        }
      }
      if (!source->CreateIndex(db, store, index, unique)) {
        return ThrowDom(call, "ConstraintError", "index '" + index + "' already exists");
      }
      self->RememberIdbIndexMeta(db, store, index, key_path, unique);
      return self->MakeIdbIndex(db, store, index, SlotValue(call.self, kIdbTxnSlot));
    });
    method("index", [self, source](NativeCall& call) -> Value {
      const std::string db = SlotString(call.self, kIdbDbSlot);
      const std::string store = SlotString(call.self, kIdbStoreSlot);
      const std::string index = js::ToString(Argument(call.arguments, 0));
      const std::vector<std::string> names = source->IndexNames(db, store);
      if (std::find(names.begin(), names.end(), index) == names.end()) {
        return ThrowDom(call, "NotFoundError", "index '" + index + "' does not exist");
      }
      return self->MakeIdbIndex(db, store, index, SlotValue(call.self, kIdbTxnSlot));
    });
    const Value key_path_getter = interpreter_->NewNativeValue(
        "keyPath", [self, source](NativeCall& call) -> Value {
          const IndexedDbKeyPath path =
              source->ObjectStoreKeyPath(SlotString(call.self, kIdbDbSlot),
                                        SlotString(call.self, kIdbStoreSlot));
          if (!path.has_path) {
            return Value::Null();
          }
          if (!path.is_array && path.parts.size() == 1) {
            return Value::String(path.parts[0]);
          }
          std::vector<Value> parts;
          for (const std::string& part : path.parts) parts.push_back(Value::String(part));
          return call.interpreter.NewArrayValue(std::move(parts));
        });
    if (key_path_getter.IsObject()) {
      key_path_getter.object->Set(kOwnerSlot, PointerValue(this));
      store_interface.object->DefineAccessor("keyPath", key_path_getter.object, nullptr);
    }
    const Value index_names_getter = interpreter_->NewNativeValue(
        "indexNames", [self, source](NativeCall& call) {
          std::vector<Value> names;
          for (const std::string& name :
               source->IndexNames(SlotString(call.self, kIdbDbSlot), SlotString(call.self, kIdbStoreSlot))) {
            names.push_back(Value::String(name));
          }
          return MakeDomStringList(call.interpreter, names);
        });
    if (index_names_getter.IsObject()) {
      index_names_getter.object->Set(kOwnerSlot, PointerValue(this));
      store_interface.object->DefineAccessor("indexNames", index_names_getter.object, nullptr);
    }
  }

  // --- indexedDB / IDBFactory --------------------------------------------------
  const Value factory = interpreter_->NewObjectValue();
  if (!factory.IsObject()) {
    return;
  }
  const Value open_method = interpreter_->NewNativeValue(
      "open", [self, source](NativeCall& call) -> Value {
        const std::string name = js::ToString(Argument(call.arguments, 0));
        const Value version_arg = Argument(call.arguments, 1);
        const std::uint64_t requested_version =
            version_arg.IsUndefined() ? 0u : static_cast<std::uint64_t>(js::ToNumber(version_arg));
        const Value request =
            self->MakeIdbRequest("IDBOpenDBRequest", Value::Undefined(), Value::Undefined());
        util::AddPerformanceCounter(util::PerfCounterId::IdbOpens);
        const IndexedDbSource::OpenResult open_result = source->OpenDatabase(name, requested_version);
        if (requested_version != 0 && requested_version < open_result.new_version) {
          self->DeliverIdbError(request, "VersionError",
                                "the requested version is behind the database's current version");
          return request;
        }
        const Value database = call.interpreter.NewObjectValue();
        if (database.IsObject()) {
          if (const Value proto = self->InterfaceNamed("IDBDatabase"); proto.IsObject()) {
            database.object->SetPrototype(proto.object);
          }
          database.object->SetHidden(kIdbDbSlot, Value::String(name));
          database.object->SetHidden("#idbVersion", Value::Number(static_cast<double>(open_result.new_version)));
          self->InstallEventMethods(database);
        }
        DomBindings* bindings = self;
        const std::string db_name = name;
        const Value deliver = call.interpreter.NewNativeValue(
            "idbOpen", [bindings, request, database, open_result, db_name](NativeCall& inner) -> Value {
              if (open_result.needs_upgrade) {
                util::AddPerformanceCounter(util::PerfCounterId::IdbUpgrades);
                // Spec: during upgradeneeded the open request's `.transaction`
                // is the versionchange transaction. youtube's EntityStore does
                // `new v_(a.transaction)` and throws if that is undefined —
                // which is how watch logged `addEventListener of undefined`
                // and never finished creating its object stores.
                const Value upgrade_txn = bindings->MakeIdbTransaction(
                    database, db_name, {}, "versionchange");
                if (upgrade_txn.IsObject()) {
                  request.object->Set("transaction", upgrade_txn);
                }
                const Value event = inner.interpreter.NewObjectValue();
                if (event.IsObject()) {
                  if (const Value proto = bindings->InterfaceNamed("Event"); proto.IsObject()) {
                    event.object->SetPrototype(proto.object);
                  }
                  event.object->Set("type", Value::String(std::string("upgradeneeded")));
                  event.object->Set("target", request);
                  event.object->Set("oldVersion", Value::Number(static_cast<double>(open_result.old_version)));
                  event.object->Set("newVersion", Value::Number(static_cast<double>(open_result.new_version)));
                  request.object->Set("result", database);
                  // Still "pending" through the upgrade; success flips it later.
                  request.object->Set("readyState", Value::String(std::string("pending")));
                  if (const Value* handler = request.object->GetOwn("#onupgradeneeded");
                      handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
                    const js::Result outcome =
                        inner.interpreter.CallFunction(*handler, request, {event});
                    if (outcome.completion == js::Completion::Throw) {
                      inner.interpreter.ReportUncaught(outcome.value, "IDBOpenDBRequest upgrade handler");
                    }
                  }
                  if (const Value* dispatch = request.object->Get("dispatchEvent");
                      dispatch != nullptr && dispatch->IsObject()) {
                    const js::Result outcome =
                        inner.interpreter.CallFunction(*dispatch, request, {event});
                    if (outcome.completion == js::Completion::Throw) {
                      inner.interpreter.ReportUncaught(outcome.value, "IDBOpenDBRequest upgrade listener");
                    }
                  }
                }
                if (upgrade_txn.IsObject()) {
                  bindings->MaybeCompleteIdbTransaction(upgrade_txn);
                  // After the upgrade the open request no longer carries a
                  // transaction — same as browsers once success fires.
                  request.object->Set("transaction", Value::Null());
                }
              }
              bindings->DeliverIdbSuccess(request, database);
              return Value::Undefined();
            });
        if (deliver.IsObject()) {
          deliver.object->Set("#request", request);
          deliver.object->Set("#database", database);
          TimerQueue::QueueTask(*self->interpreter_, deliver);
        }
        return request;
      });
  if (open_method.IsObject()) {
    open_method.object->Set(kOwnerSlot, PointerValue(this));
    factory.object->Set("open", open_method);
  }
  const Value delete_method = interpreter_->NewNativeValue(
      "deleteDatabase", [self, source](NativeCall& call) -> Value {
        const std::string name = js::ToString(Argument(call.arguments, 0));
        source->DeleteDatabase(name);
        const Value request =
            self->MakeIdbRequest("IDBOpenDBRequest", Value::Undefined(), Value::Undefined());
        self->DeliverIdbSuccess(request, Value::Undefined());
        return request;
      });
  if (delete_method.IsObject()) {
    delete_method.object->Set(kOwnerSlot, PointerValue(this));
    factory.object->Set("deleteDatabase", delete_method);
  }
  interpreter_->Global()->Set("indexedDB", factory);
  interpreter_->GlobalScope()->Declare("indexedDB", factory, false);

  // `IDBKeyRange`, `IDBIndex` and the two cursor interfaces are in
  // IndexedDbRequests.cpp.
  InstallIndexedDbCursors();
}

}  // namespace microbrowser::bindings

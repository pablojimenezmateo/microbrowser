// `IDBIndex`, `IDBKeyRange`, `IDBCursor` and `IDBCursorWithValue` -- split
// from IndexedDbBindings.cpp once that file reached the module's line cap.
// ADR 0038.
//
// **`IDBKeyRange` is `.only()` and nothing else.** No `.bound()`,
// `.lowerBound()`, `.upperBound()`, and no open/closed range on a cursor --
// EntityStore's own use is `IDBKeyRange.only(key)` on an index lookup and a
// cursor walk that deletes every record matching one, and a half-built range
// comparison across three key *types* (see storage::IndexedDbKey::Encode) is
// the shape ADR 0012 calls a stub: a page that finds `.bound()` and gets one
// that silently answers nothing is worse off than a page that finds no
// `IDBKeyRange` at all.
//
// A cursor's `continue()`/`delete()` re-run `IndexedDbSource::Query` rather
// than holding an iterator: the store behind the seam is a map a page's own
// handler can mutate between two cursor steps -- EntityStore's own eviction
// does exactly that on a `get` inside the same transaction -- so re-querying
// is what stays correct rather than what is fast against a store this small.

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

// Every primary key this cursor has already delivered, encoded with
// `EncodeIdbKeyValue`. `continue()` re-runs the same query and this is how it
// finds the next entry rather than the next *position*: a `delete()` on the
// entry just delivered (EntityStore's own cursor-walk shape, and the test
// that caught this) removes it from that query, which shifts every later
// entry's index down by one -- a position counter would skip exactly the
// entry that shift moved into the position it already visited. A set of
// what has been seen is immune to the shift because it does not name a
// position at all.
constexpr const char* kIdbCursorVisitedSlot = "#idbCursorVisited";
constexpr const char* kIdbCursorOnlySlot = "#idbCursorOnly";
constexpr const char* kIdbCursorHasValueSlot = "#idbCursorHasValue";
// The `IDBRequest` a cursor was opened by -- `continue()` and `delete()`
// deliver on it, exactly the way `openCursor` itself did for the first entry.
constexpr const char* kIdbCursorRequestSlot = "#idbCursorRequest";

// Re-runs the query a cursor was opened with and positions it on the first
// entry not already named in `#idbCursorVisited`, writing `key`/`primaryKey`
// (and `value`, for `IDBCursorWithValue`) and adding that entry's primary key
// to the visited set. False -- with every property left as it was -- once
// nothing in a fresh query is left unvisited, which is `continue()`'s cue to
// deliver `null` rather than the cursor itself.
bool RefreshIdbCursor(js::Interpreter& interpreter, IndexedDbSource& source, const Value& cursor) {
  if (!cursor.IsObject()) {
    return false;
  }
  const std::string db = IdbSlotString(cursor, kIdbDbSlot);
  const std::string store = IdbSlotString(cursor, kIdbStoreSlot);
  const std::string index = IdbSlotString(cursor, kIdbIndexSlot);
  const std::optional<IndexedDbKeyValue> only = QueryToIdbKey(IdbSlotValue(cursor, kIdbCursorOnlySlot));
  util::AddPerformanceCounter(util::PerfCounterId::IdbCursorQueries);
  const std::vector<IndexedDbSource::CursorEntry> entries = source.Query(db, store, index, only);
  const Value visited = IdbSlotValue(cursor, kIdbCursorVisitedSlot);
  for (const IndexedDbSource::CursorEntry& entry : entries) {
    const std::string encoded_primary = EncodeIdbKeyValue(entry.primary_key);
    bool already_seen = false;
    if (visited.IsObject()) {
      for (std::size_t i = 0; i < visited.object->ElementCount(); ++i) {
        if (js::ToString(visited.object->GetElement(i)) == encoded_primary) {
          already_seen = true;
          break;
        }
      }
    }
    if (already_seen) {
      continue;
    }
    if (visited.IsObject()) {
      visited.object->PushElement(Value::String(encoded_primary));
    }
    cursor.object->Set("key", IdbKeyToValue(interpreter, entry.key));
    cursor.object->Set("primaryKey", IdbKeyToValue(interpreter, entry.primary_key));
    if (cursor.object->GetOwn(kIdbCursorHasValueSlot) != nullptr) {
      cursor.object->Set("value",
                         js::StructuredDeserialize(interpreter, js::SerializedValue{entry.value}));
    }
    return true;
  }
  return false;
}

}  // namespace

// A fresh `IDBCursor` (or `IDBCursorWithValue`) over `db`/`store`, filtered by
// `index` (empty for the store's own primary key) and `only` (the query
// `openCursor` was called with). Delivers as `request`'s result on the
// caller's turn, positioned on the first matching entry or `null` when there
// is none -- `openCursor` never errors just because nothing matched.
void DomBindings::OpenIdbCursor(IndexedDbSource& source, const std::string& db,
                                const std::string& store, const std::string& index,
                                const js::Value& only, const js::Value& transaction,
                                const js::Value& request, bool with_value) {
  const Value cursor = interpreter_->NewObjectValue();
  if (!cursor.IsObject()) {
    DeliverIdbSuccess(request, Value::Null());
    return;
  }
  if (const Value proto = InterfaceNamed(with_value ? "IDBCursorWithValue" : "IDBCursor");
      proto.IsObject()) {
    cursor.object->SetPrototype(proto.object);
  }
  cursor.object->SetHidden(kIdbDbSlot, Value::String(db));
  cursor.object->SetHidden(kIdbStoreSlot, Value::String(store));
  cursor.object->SetHidden(kIdbIndexSlot, Value::String(index));
  cursor.object->SetHidden(kIdbTxnSlot, transaction);
  cursor.object->SetHidden(kIdbCursorOnlySlot, only);
  cursor.object->SetHidden(kIdbCursorVisitedSlot, interpreter_->NewArrayValue({}));
  cursor.object->SetHidden(kIdbCursorRequestSlot, request);
  if (with_value) {
    cursor.object->SetHidden(kIdbCursorHasValueSlot, Value::Bool(true));
  }
  const Value* request_source = request.object->GetOwn("source");
  cursor.object->Set("source", request_source == nullptr ? Value::Undefined() : *request_source);
  if (!RefreshIdbCursor(*interpreter_, source, cursor)) {
    DeliverIdbSuccess(request, Value::Null());
    return;
  }
  DeliverIdbSuccess(request, cursor);
}

js::Value DomBindings::MakeIdbIndex(const std::string& db, const std::string& store,
                                    const std::string& index, const js::Value& transaction) {
  const Value wrapper = interpreter_->NewObjectValue();
  if (!wrapper.IsObject()) {
    return Value::Undefined();
  }
  if (const Value proto = InterfaceNamed("IDBIndex"); proto.IsObject()) {
    wrapper.object->SetPrototype(proto.object);
  }
  wrapper.object->SetHidden(kIdbDbSlot, Value::String(db));
  wrapper.object->SetHidden(kIdbStoreSlot, Value::String(store));
  wrapper.object->SetHidden(kIdbIndexSlot, Value::String(index));
  wrapper.object->SetHidden(kIdbTxnSlot, transaction);
  wrapper.object->Set("name", Value::String(index));
  wrapper.object->Set("unique", Value::Bool(IdbIndexIsUnique(db, store, index)));
  const std::optional<IndexedDbKeyPath> key_path = IdbIndexKeyPath(db, store, index);
  if (!key_path.has_value() || !key_path->has_path) {
    wrapper.object->Set("keyPath", Value::Null());
  } else if (!key_path->is_array && key_path->parts.size() == 1) {
    wrapper.object->Set("keyPath", Value::String(key_path->parts[0]));
  } else {
    std::vector<Value> parts;
    parts.reserve(key_path->parts.size());
    for (const std::string& part : key_path->parts) {
      parts.push_back(Value::String(part));
    }
    wrapper.object->Set("keyPath", interpreter_->NewArrayValue(std::move(parts)));
  }
  return wrapper;
}

void DomBindings::InstallIndexedDbCursors() {
  if (indexed_db_ == nullptr) {
    return;
  }
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  DomBindings* self = this;
  IndexedDbSource* source = indexed_db_;

  const Value index_interface = MakeInterface("IDBIndex", Value::Undefined());
  const Value cursor_interface = MakeInterface("IDBCursor", Value::Undefined());
  const Value cursor_value_interface = MakeInterface("IDBCursorWithValue", cursor_interface);
  const Value range_interface = MakeInterface("IDBKeyRange", Value::Undefined());
  if (!index_interface.IsObject() || !cursor_interface.IsObject() ||
      !cursor_value_interface.IsObject() || !range_interface.IsObject()) {
    return;
  }

  // --- IDBKeyRange, `.only()` only -----------------------------------------
  if (js::Value* ctor = interpreter_->GlobalScope()->Lookup("IDBKeyRange");
      ctor != nullptr && ctor->IsObject()) {
    const Value only_method = interpreter_->NewNativeValue(
        "only", [range_interface](NativeCall& call) -> Value {
          const Value range = call.interpreter.NewObjectValue();
          if (!range.IsObject()) {
            return Value::Undefined();
          }
          range.object->SetPrototype(range_interface.object);
          const Value key = Argument(call.arguments, 0);
          range.object->SetHidden(kIdbRangeOnlySlot, key);
          range.object->Set("lower", key);
          range.object->Set("upper", key);
          range.object->Set("lowerOpen", Value::Bool(false));
          range.object->Set("upperOpen", Value::Bool(false));
          return range;
        });
    if (only_method.IsObject()) {
      ctor->object->Set("only", only_method);
    }
  }

  // --- IDBIndex --------------------------------------------------------------
  {
    const auto method = [this, &index_interface](const char* name, js::NativeFunction fn) {
      const Value native = interpreter_->NewNativeValue(name, std::move(fn));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, OwnerValue(this));
        index_interface.object->Set(name, native);
      }
    };
    method("get", [self, source](NativeCall& call) -> Value {
      const std::string db = IdbSlotString(call.self, kIdbDbSlot);
      const std::string store = IdbSlotString(call.self, kIdbStoreSlot);
      const std::string index = IdbSlotString(call.self, kIdbIndexSlot);
      const Value transaction = IdbSlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      const std::optional<IndexedDbKeyValue> only = QueryToIdbKey(Argument(call.arguments, 0));
      if (!only.has_value()) {
        self->DeliverIdbError(request, "DataError", "invalid key");
        return request;
      }
      util::AddPerformanceCounter(util::PerfCounterId::IdbGets);
      const std::vector<IndexedDbSource::CursorEntry> entries = source->Query(db, store, index, only);
      const Value value = entries.empty()
                              ? Value::Undefined()
                              : js::StructuredDeserialize(call.interpreter,
                                                          js::SerializedValue{entries.front().value});
      self->DeliverIdbSuccess(request, value);
      return request;
    });
    method("getAll", [self, source](NativeCall& call) -> Value {
      const std::string db = IdbSlotString(call.self, kIdbDbSlot);
      const std::string store = IdbSlotString(call.self, kIdbStoreSlot);
      const std::string index = IdbSlotString(call.self, kIdbIndexSlot);
      const Value transaction = IdbSlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      const std::optional<IndexedDbKeyValue> only = QueryToIdbKey(Argument(call.arguments, 0));
      std::vector<Value> values;
      for (const IndexedDbSource::CursorEntry& entry : source->Query(db, store, index, only)) {
        values.push_back(
            js::StructuredDeserialize(call.interpreter, js::SerializedValue{entry.value}));
      }
      self->DeliverIdbSuccess(request, call.interpreter.NewArrayValue(std::move(values)));
      return request;
    });
    method("openCursor", [self, source](NativeCall& call) -> Value {
      const std::string db = IdbSlotString(call.self, kIdbDbSlot);
      const std::string store = IdbSlotString(call.self, kIdbStoreSlot);
      const std::string index = IdbSlotString(call.self, kIdbIndexSlot);
      const Value transaction = IdbSlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      self->OpenIdbCursor(*source, db, store, index, Argument(call.arguments, 0), transaction, request,
                         /*with_value=*/true);
      return request;
    });
  }

  // --- IDBObjectStore.openCursor, over the store's own primary key -----------
  if (const Value store_interface = InterfaceNamed("IDBObjectStore"); store_interface.IsObject()) {
    const Value open_cursor = interpreter_->NewNativeValue(
        "openCursor", [self, source](NativeCall& call) -> Value {
          const std::string db = IdbSlotString(call.self, kIdbDbSlot);
          const std::string store = IdbSlotString(call.self, kIdbStoreSlot);
          const Value transaction = IdbSlotValue(call.self, kIdbTxnSlot);
          const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
          self->OpenIdbCursor(*source, db, store, "", Argument(call.arguments, 0), transaction, request,
                             /*with_value=*/true);
          return request;
        });
    if (open_cursor.IsObject()) {
      open_cursor.object->Set(kOwnerSlot, OwnerValue(this));
      store_interface.object->Set("openCursor", open_cursor);
    }
  }

  // --- IDBCursor / IDBCursorWithValue -----------------------------------------
  {
    const auto method = [this, &cursor_interface](const char* name, js::NativeFunction fn) {
      const Value native = interpreter_->NewNativeValue(name, std::move(fn));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, OwnerValue(this));
        cursor_interface.object->Set(name, native);
      }
    };
    method("continue", [self, source](NativeCall& call) -> Value {
      if (!call.self.IsObject()) {
        return Value::Undefined();
      }
      const Value request = IdbSlotValue(call.self, kIdbCursorRequestSlot);
      if (RefreshIdbCursor(call.interpreter, *source, call.self)) {
        self->DeliverIdbSuccess(request, call.self);
      } else {
        self->DeliverIdbSuccess(request, Value::Null());
      }
      return Value::Undefined();
    });
    method("delete", [self, source](NativeCall& call) -> Value {
      const std::string db = IdbSlotString(call.self, kIdbDbSlot);
      const std::string store = IdbSlotString(call.self, kIdbStoreSlot);
      const Value transaction = IdbSlotValue(call.self, kIdbTxnSlot);
      const Value request = self->MakeIdbRequest("IDBRequest", call.self, transaction);
      const Value* primary_key = call.self.IsObject() ? call.self.object->GetOwn("primaryKey") : nullptr;
      const std::optional<IndexedDbKeyValue> key =
          primary_key == nullptr ? std::nullopt : ValueToIdbKey(*primary_key);
      if (!key.has_value()) {
        self->DeliverIdbError(request, "InvalidStateError", "cursor has no current value");
        return request;
      }
      util::AddPerformanceCounter(util::PerfCounterId::IdbDeletes);
      source->Delete(db, store, *key);
      self->DeliverIdbSuccess(request, Value::Undefined());
      return request;
    });
  }
}

}  // namespace microbrowser::bindings

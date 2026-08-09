#pragma once

// Shared by the two IndexedDB translation units, and private to the module for
// the reason BindingSupport.h is: a binding is an implementation detail of the
// seam, not part of its interface, and this header is deliberately absent from
// MODULE.deps' `public:` list.
//
// What is here is the one thing both files need and neither owns: turning a
// JavaScript value into the key `bindings::IndexedDbSource` crosses the seam
// with, and back. `src/storage` has its own `IndexedDbKey` and its own
// `Encode()`; this is the JavaScript-value half of the same idea, kept on this
// side because this module may see `js` and `src/storage` may not.

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "bindings/IndexedDb.h"
#include "js/Interpreter.h"
#include "js/Value.h"

namespace microbrowser::bindings {

// Hidden slots shared by IndexedDbBindings.cpp and IndexedDbRequests.cpp --
// every wrapper this seam builds (`IDBDatabase`, `IDBTransaction`,
// `IDBObjectStore`, `IDBIndex`, `IDBCursor`, `IDBRequest`) names the database,
// store or index it is bound to the same way, so a helper written against one
// works against all of them.
inline constexpr const char* kIdbDbSlot = "#idbDb";
inline constexpr const char* kIdbStoreSlot = "#idbStore";
inline constexpr const char* kIdbIndexSlot = "#idbIndex";
inline constexpr const char* kIdbTxnSlot = "#idbTxn";
inline constexpr const char* kIdbPendingSlot = "#idbPending";
inline constexpr const char* kIdbDoneSlot = "#idbDone";
// Store names the transaction was opened against. Lives in a hidden slot
// rather than as an own data property so `objectStoreNames` can sit on
// `IDBTransaction.prototype` — youtube's `yPS` feature-detect refuses the
// whole IndexedDB surface unless `"objectStoreNames" in IDBTransaction.prototype`.
inline constexpr const char* kIdbTxnStoreNamesSlot = "#idbTxnStoreNames";

inline std::string IdbSlotString(const js::Value& value, const char* slot) {
  const js::Value* found = value.IsObject() ? value.object->GetOwn(slot) : nullptr;
  return found == nullptr ? std::string() : js::ToString(*found);
}

inline js::Value IdbSlotValue(const js::Value& value, const char* slot) {
  const js::Value* found = value.IsObject() ? value.object->GetOwn(slot) : nullptr;
  return found == nullptr ? js::Value::Undefined() : *found;
}

// `options.keyPath`: absent, a single dotted string, or an array of them for a
// compound key. `is_array` is what a page's `Array.isArray(keyPath)` would
// answer, kept because it changes what a matching value looks like -- an array
// of one element still produces an Array-typed key.
inline IndexedDbKeyPath ParseIdbKeyPath(const js::Value& option) {
  IndexedDbKeyPath path;
  if (option.IsString()) {
    path.has_path = true;
    path.parts.push_back(js::ToString(option));
    return path;
  }
  if (option.IsObject() && option.object->GetKind() == js::Object::Kind::Array) {
    path.has_path = true;
    path.is_array = true;
    for (std::size_t i = 0; i < option.object->ElementCount(); ++i) {
      path.parts.push_back(js::ToString(option.object->GetElement(i)));
    }
    return path;
  }
  return path;
}

// One step of a dotted path (`"name.first"` reads `.name` then `.first`).
// Undefined the moment a step is not an object, which is what a page's own
// `keyPath` mismatch should answer with -- neither an error nor a guess.
inline js::Value GetByDottedPath(const js::Value& value, const std::string& dotted) {
  js::Value current = value;
  std::size_t start = 0;
  while (start <= dotted.size()) {
    const std::size_t dot = dotted.find('.', start);
    const std::string part = dotted.substr(start, dot == std::string::npos ? dot : dot - start);
    if (!current.IsObject()) {
      return js::Value::Undefined();
    }
    const js::Value* found = current.object->Get(part);
    current = found == nullptr ? js::Value::Undefined() : *found;
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  return current;
}

// The specified failure is `nullopt`, which every call site turns into a
// `DataError` -- a function, a symbol, `NaN`, `undefined` and a plain object
// are none of them keys. A Date is not one either: this browser's IndexedDB
// only has the three shapes `storage::IndexedDbKey` does (ADR 0038), and a
// page that relies on Date ordering gets a legible refusal rather than a key
// silently treated as something else.
inline std::optional<IndexedDbKeyValue> ValueToIdbKey(const js::Value& value) {
  if (value.IsNumber()) {
    if (std::isnan(value.number)) {
      return std::nullopt;
    }
    IndexedDbKeyValue key;
    key.type = IndexedDbKeyValue::Type::Number;
    key.number = value.number;
    return key;
  }
  if (value.IsString()) {
    IndexedDbKeyValue key;
    key.type = IndexedDbKeyValue::Type::String;
    key.text = js::ToString(value);
    return key;
  }
  if (value.IsObject() && value.object->GetKind() == js::Object::Kind::Array) {
    IndexedDbKeyValue key;
    key.type = IndexedDbKeyValue::Type::Array;
    for (std::size_t i = 0; i < value.object->ElementCount(); ++i) {
      const std::optional<IndexedDbKeyValue> part = ValueToIdbKey(value.object->GetElement(i));
      if (!part.has_value()) {
        return std::nullopt;
      }
      key.parts.push_back(*part);
    }
    return key;
  }
  return std::nullopt;
}

// A string unique to this key's *value*, for the one thing a cursor needs
// that ordering does not answer: "have I already delivered this record?"
// (see `RefreshIdbCursor` in IndexedDbRequests.cpp). Not a canonical
// cross-type ordering the way `storage::IndexedDbKey::Encode()` is -- this
// side of the seam only ever compares it for equality.
inline std::string EncodeIdbKeyValue(const IndexedDbKeyValue& key) {
  switch (key.type) {
    case IndexedDbKeyValue::Type::Number:
      return "n:" + std::to_string(key.number);
    case IndexedDbKeyValue::Type::String:
      return "s:" + key.text;
    case IndexedDbKeyValue::Type::Array: {
      std::string out = "a:";
      for (const IndexedDbKeyValue& part : key.parts) {
        out += EncodeIdbKeyValue(part);
        out += '\x1f';
      }
      return out;
    }
  }
  return {};
}

// Back to a value a page reads off `.key`, `.primaryKey` or a cursor -- the
// inverse of `ValueToIdbKey`.
inline js::Value IdbKeyToValue(js::Interpreter& interpreter, const IndexedDbKeyValue& key) {
  switch (key.type) {
    case IndexedDbKeyValue::Type::Number:
      return js::Value::Number(key.number);
    case IndexedDbKeyValue::Type::String:
      return js::Value::String(key.text);
    case IndexedDbKeyValue::Type::Array: {
      std::vector<js::Value> parts;
      parts.reserve(key.parts.size());
      for (const IndexedDbKeyValue& part : key.parts) {
        parts.push_back(IdbKeyToValue(interpreter, part));
      }
      return interpreter.NewArrayValue(std::move(parts));
    }
  }
  return js::Value::Undefined();
}

// Where an `IDBKeyRange` instance keeps the key `.only()` built it from. A
// page reads none of this directly, and the two supported query shapes --
// undefined (every record) and a bare key (`getAll(k)` is shorthand for
// `getAll(IDBKeyRange.only(k))`) -- both go through `QueryToIdbKey` below
// rather than repeating this check at every call site.
inline constexpr const char* kIdbRangeOnlySlot = "#idbRangeOnly";

// What `get`, `getAll` and `openCursor` all accept as a query: nothing (every
// record), an `IDBKeyRange` built by `.only()`, or a bare key. `nullopt` here
// means "no filter", which is a real answer and not a `DataError` -- a
// malformed query is a `nullopt` from `ValueToIdbKey` on a *key*, and the two
// are told apart by the caller checking `query.IsUndefined()` first if it
// needs to.
inline std::optional<IndexedDbKeyValue> QueryToIdbKey(const js::Value& query) {
  if (query.IsUndefined()) {
    return std::nullopt;
  }
  if (query.IsObject()) {
    if (const js::Value* only = query.object->GetOwn(kIdbRangeOnlySlot); only != nullptr) {
      return ValueToIdbKey(*only);
    }
  }
  return ValueToIdbKey(query);
}

// The key a `put()` uses when the caller did not pass one explicitly --
// extracted from the value being stored via the store's own `keyPath`.
// Nothing (rather than a DataError) when the path is absent, which is the
// caller's cue to fall back to the key argument instead.
inline std::optional<IndexedDbKeyValue> ExtractIdbKey(const js::Value& record,
                                                       const IndexedDbKeyPath& key_path) {
  if (!key_path.has_path) {
    return std::nullopt;
  }
  if (!key_path.is_array && key_path.parts.size() == 1) {
    return ValueToIdbKey(GetByDottedPath(record, key_path.parts[0]));
  }
  IndexedDbKeyValue key;
  key.type = IndexedDbKeyValue::Type::Array;
  for (const std::string& part : key_path.parts) {
    const std::optional<IndexedDbKeyValue> extracted = ValueToIdbKey(GetByDottedPath(record, part));
    if (!extracted.has_value()) {
      return std::nullopt;
    }
    key.parts.push_back(*extracted);
  }
  return key;
}

}  // namespace microbrowser::bindings

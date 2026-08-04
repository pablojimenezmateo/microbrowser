#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "js/Value.h"

// The hash index behind Map and Set.
//
// Module-private. A Map's *entries* are an ordinary JavaScript array hanging
// off the Map object, which is what gives them insertion order and gets them
// marked by the collector for free. This is the other half: the index from key
// to position, which is what stops `map.get(k)` from being a scan.
//
// It holds no references the collector needs to know about -- a position is a
// number and a key is a copy -- so its only lifetime requirement is that it
// goes when the Map does. The Heap owns it for exactly that reason.

namespace microbrowser::js {

// A key, compared the way Map and Set compare keys.
//
// SameValueZero, which differs from `===` in one place and from `Object.is` in
// another: NaN is a usable key and finds itself, while +0 and -0 are the same
// key. Both are what a page expects and neither is what `===` does.
struct ValueKey {
  ValueType type = ValueType::Undefined;
  bool boolean = false;
  double number = 0.0;
  bool is_nan = false;
  std::string text;
  const Object* object = nullptr;

  static ValueKey From(const Value& value) {
    ValueKey key;
    key.type = value.type;
    switch (value.type) {
      case ValueType::Boolean:
        key.boolean = value.boolean;
        break;
      case ValueType::Number:
        // NaN is a key that finds itself, and the two zeros are one key.
        // Normalising here is what makes both true without a special case at
        // every lookup.
        key.is_nan = std::isnan(value.number);
        key.number = key.is_nan ? 0.0 : (value.number == 0.0 ? 0.0 : value.number);
        break;
      case ValueType::String:
        key.text = value.AsString();
        break;
      case ValueType::BigInt:
        // By *value*, unlike a symbol: `m.set(1n, x); m.get(1n)` has to find
        // it, and the two literals are different cells. The decimal form is
        // the key, which is exact and is already the equality the type has.
        key.text = BigIntText(value);
        break;
      case ValueType::Object:
      case ValueType::Symbol:
        key.object = value.object;
        break;
      case ValueType::Undefined:
      case ValueType::Null:
        break;
    }
    return key;
  }

  bool operator==(const ValueKey& other) const {
    if (type != other.type) {
      return false;
    }
    switch (type) {
      case ValueType::Boolean:
        return boolean == other.boolean;
      case ValueType::Number:
        return is_nan == other.is_nan && (is_nan || number == other.number);
      case ValueType::String:
      case ValueType::BigInt:
        return text == other.text;
      case ValueType::Object:
      case ValueType::Symbol:
        return object == other.object;
      case ValueType::Undefined:
      case ValueType::Null:
        return true;
    }
    return false;
  }

  struct Hash {
    std::size_t operator()(const ValueKey& key) const {
      const std::size_t tag = static_cast<std::size_t>(key.type) * 0x9E3779B9u;
      switch (key.type) {
        case ValueType::Boolean:
          return tag ^ (key.boolean ? 1u : 2u);
        case ValueType::Number:
          return tag ^ (key.is_nan ? 0x7FF8u : std::hash<double>{}(key.number));
        case ValueType::String:
        case ValueType::BigInt:
          return tag ^ std::hash<std::string>{}(key.text);
        case ValueType::Object:
        case ValueType::Symbol:
          return tag ^ std::hash<const void*>{}(key.object);
        case ValueType::Undefined:
        case ValueType::Null:
          return tag;
      }
      return tag;
    }
  };
};

// Key to position in the entries array.
//
// A deleted entry leaves a hole rather than shifting everything after it: the
// positions in here would all be wrong otherwise, and re-indexing on every
// delete is what turns removing n entries into O(n^2). Holes are compacted
// when they outnumber the live entries.
struct MapIndex {
  std::unordered_map<ValueKey, std::size_t, ValueKey::Hash> positions;
  std::size_t holes = 0;
};

}  // namespace microbrowser::js

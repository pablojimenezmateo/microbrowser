#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microbrowser::storage {

// A structured-clone key, restricted to the three shapes ADR 0038 needs: a
// number, a string, or an array of either (a compound key -- EntityStore's
// `["parentEntityKey", "childEntityKey"]`). A Date, a binary key and an
// object key are not part of this browser's IndexedDB: `src/bindings` refuses
// to build one of those into a key at all, so a page that relies on one gets
// a `DataError` rather than a silently wrong record. See bindings/IndexedDb.h,
// which is where a JavaScript value becomes one of these.
struct IndexedDbKey {
  enum class Type : std::uint8_t { Number, String, Array };

  Type type = Type::Number;
  double number = 0.0;
  std::string text;
  std::vector<IndexedDbKey> parts;

  static IndexedDbKey OfNumber(double value);
  static IndexedDbKey OfString(std::string value);
  static IndexedDbKey OfArray(std::vector<IndexedDbKey> values);

  // A byte string unique to this key's value, used as a map key everywhere
  // below. It orders keys of the same shape consistently -- a number by IEEE
  // order, a string or an array by its encoded bytes -- but it is **not**
  // the specification's full cross-type comparison (number < date < string <
  // binary < array), because this browser has neither of the two missing
  // types. Nothing here promises a page that mixed key types in one index an
  // iteration order the specification would recognise; it promises a
  // deterministic one.
  std::string Encode() const;

  friend bool operator==(const IndexedDbKey&, const IndexedDbKey&) = default;
};

}  // namespace microbrowser::storage

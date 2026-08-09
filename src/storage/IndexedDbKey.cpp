#include "storage/IndexedDbKey.h"

#include <bit>
#include <cstring>
#include <utility>

namespace microbrowser::storage {

namespace {

// Big-endian bytes of `value`, transformed so that unsigned byte comparison
// matches IEEE-754 ordering: flip the sign bit for a non-negative value, and
// flip every bit for a negative one. The standard trick for making a float's
// bit pattern sort like the number it represents.
std::string EncodeNumber(double value) {
  std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  if ((bits & 0x8000000000000000ULL) == 0) {
    bits |= 0x8000000000000000ULL;
  } else {
    bits = ~bits;
  }
  std::string out(8, '\0');
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = static_cast<char>(bits & 0xFFu);
    bits >>= 8;
  }
  return out;
}

// A length-prefixed string, so that a text key never runs into the type tag
// or length field that follows it in an array's concatenation.
std::string EncodeString(const std::string& text) {
  std::string out;
  const std::uint32_t length = static_cast<std::uint32_t>(text.size());
  out.resize(4);
  out[0] = static_cast<char>((length >> 24) & 0xFFu);
  out[1] = static_cast<char>((length >> 16) & 0xFFu);
  out[2] = static_cast<char>((length >> 8) & 0xFFu);
  out[3] = static_cast<char>(length & 0xFFu);
  out += text;
  return out;
}

}  // namespace

IndexedDbKey IndexedDbKey::OfNumber(double value) {
  IndexedDbKey key;
  key.type = Type::Number;
  key.number = value;
  return key;
}

IndexedDbKey IndexedDbKey::OfString(std::string value) {
  IndexedDbKey key;
  key.type = Type::String;
  key.text = std::move(value);
  return key;
}

IndexedDbKey IndexedDbKey::OfArray(std::vector<IndexedDbKey> values) {
  IndexedDbKey key;
  key.type = Type::Array;
  key.parts = std::move(values);
  return key;
}

std::string IndexedDbKey::Encode() const {
  switch (type) {
    case Type::Number:
      return std::string(1, static_cast<char>(0x01)) + EncodeNumber(number);
    case Type::String:
      return std::string(1, static_cast<char>(0x02)) + EncodeString(text);
    case Type::Array: {
      // No count prefix: each element is already self-delimited (a fixed
      // 9 bytes for a number, a length-prefixed run for a string or a
      // nested array), so a shorter array whose elements are a prefix of a
      // longer one's still sorts before it -- running out of bytes compares
      // less than any byte that follows.
      std::string out(1, static_cast<char>(0x03));
      for (const IndexedDbKey& part : parts) {
        out += part.Encode();
      }
      return out;
    }
  }
  return {};
}

}  // namespace microbrowser::storage

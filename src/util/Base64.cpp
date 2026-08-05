#include "util/Base64.h"

#include <cstdint>

namespace microbrowser::util {

namespace {

constexpr std::string_view kStandard =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// -1 for "not a base64 character at all", -2 for "the other alphabet's". The
// two are separate because a string that mixes them is rejected rather than
// decoded, and a single table cannot say which alphabet a character came from
// without one of these markers.
constexpr int kInvalid = -1;

struct Decoded {
  int value = kInvalid;
  // 0 for a character both alphabets share, 1 for `+/`, 2 for `-_`.
  int alphabet = 0;
};

Decoded DecodeChar(char c) {
  if (c >= 'A' && c <= 'Z') {
    return Decoded{c - 'A', 0};
  }
  if (c >= 'a' && c <= 'z') {
    return Decoded{c - 'a' + 26, 0};
  }
  if (c >= '0' && c <= '9') {
    return Decoded{c - '0' + 52, 0};
  }
  if (c == '+') {
    return Decoded{62, 1};
  }
  if (c == '/') {
    return Decoded{63, 1};
  }
  if (c == '-') {
    return Decoded{62, 2};
  }
  if (c == '_') {
    return Decoded{63, 2};
  }
  return Decoded{kInvalid, 0};
}

}  // namespace

std::optional<std::string> Base64Decode(std::string_view text) {
  std::string out;
  out.reserve(text.size() / 4 * 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  int alphabet = 0;
  std::size_t characters = 0;
  std::size_t padding = 0;
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '\f') {
      // ASCII whitespace is stripped before decoding, which is what every
      // consumer of a wrapped base64 blob needs and what the `data:` URL
      // this replaced already did.
      continue;
    }
    if (c == '=') {
      ++padding;
      if (padding > 2) {
        return std::nullopt;
      }
      continue;
    }
    if (padding > 0) {
      // A character after padding. `YQ==Yg==` is two values for one string,
      // and an integrity check with two spellings is not one.
      return std::nullopt;
    }
    const Decoded decoded = DecodeChar(c);
    if (decoded.value == kInvalid) {
      return std::nullopt;
    }
    if (decoded.alphabet != 0) {
      if (alphabet != 0 && alphabet != decoded.alphabet) {
        return std::nullopt;
      }
      alphabet = decoded.alphabet;
    }
    ++characters;
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(decoded.value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
    }
  }
  // Whatever is left over must be zero: `YQ=` has two leftover bits and they
  // must both be clear, or two different encodings decode to the same bytes.
  if (bits >= 6 || (accumulator & ((1U << bits) - 1U)) != 0) {
    return std::nullopt;
  }
  const std::size_t remainder = characters % 4;
  if (remainder == 1) {
    // Six bits is never a byte.
    return std::nullopt;
  }
  if (padding > 0 && remainder + padding != 4) {
    return std::nullopt;
  }
  return out;
}

std::string Base64Encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  const auto byte_at = [&bytes](std::size_t index) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[index]));
  };
  std::size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const std::uint32_t triple =
        (byte_at(i) << 16) | (byte_at(i + 1) << 8) | byte_at(i + 2);
    out.push_back(kStandard[(triple >> 18) & 0x3FU]);
    out.push_back(kStandard[(triple >> 12) & 0x3FU]);
    out.push_back(kStandard[(triple >> 6) & 0x3FU]);
    out.push_back(kStandard[triple & 0x3FU]);
    i += 3;
  }
  const std::size_t left = bytes.size() - i;
  if (left == 1) {
    const std::uint32_t triple = byte_at(i) << 16;
    out.push_back(kStandard[(triple >> 18) & 0x3FU]);
    out.push_back(kStandard[(triple >> 12) & 0x3FU]);
    out.push_back('=');
    out.push_back('=');
  } else if (left == 2) {
    const std::uint32_t triple = (byte_at(i) << 16) | (byte_at(i + 1) << 8);
    out.push_back(kStandard[(triple >> 18) & 0x3FU]);
    out.push_back(kStandard[(triple >> 12) & 0x3FU]);
    out.push_back(kStandard[(triple >> 6) & 0x3FU]);
    out.push_back('=');
  }
  return out;
}

}  // namespace microbrowser::util

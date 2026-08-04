#include "js/StringUnits.h"

#include <algorithm>
#include <cstring>

namespace microbrowser::js {

namespace {

// One decoded scalar and how many bytes it took.
//
// Accepts the surrogate range, which well-formed UTF-8 does not contain: a lone
// surrogate is representable here on purpose, because `charCodeAt` on an emoji
// hands one out and `fromCharCode` has to be able to take it back. Rejects
// nothing else either -- the bytes always came from this engine's own encoder,
// and a malformed sequence is read one byte at a time rather than refused, so
// that no string method can be made to walk off the end.
struct Decoded {
  std::uint32_t code = 0;
  std::size_t width = 1;
};

Decoded DecodeAt(std::string_view text, std::size_t at) {
  const auto lead = static_cast<unsigned char>(text[at]);
  if (lead < 0x80u) {
    return {lead, 1};
  }
  std::size_t extra = 0;
  std::uint32_t value = 0;
  if ((lead & 0xE0u) == 0xC0u) {
    extra = 1;
    value = lead & 0x1Fu;
  } else if ((lead & 0xF0u) == 0xE0u) {
    extra = 2;
    value = lead & 0x0Fu;
  } else if ((lead & 0xF8u) == 0xF0u) {
    extra = 3;
    value = lead & 0x07u;
  } else {
    return {lead, 1};  // a stray continuation byte stands for itself
  }
  if (at + extra >= text.size()) {
    return {lead, 1};  // truncated: one byte, and the walk keeps its bounds
  }
  for (std::size_t i = 1; i <= extra; ++i) {
    const auto byte = static_cast<unsigned char>(text[at + i]);
    if ((byte & 0xC0u) != 0x80u) {
      return {lead, 1};
    }
    value = (value << 6) | (byte & 0x3Fu);
  }
  return {value, extra + 1};
}

// How many UTF-16 code units a scalar takes: two above the basic plane, one
// otherwise -- including for a lone surrogate, which is already a code unit.
std::size_t UnitsFor(std::uint32_t code) { return code > 0xFFFFu ? 2 : 1; }

void AppendScalar(std::string& out, std::uint32_t code) {
  if (code < 0x80u) {
    out.push_back(static_cast<char>(code));
    return;
  }
  if (code < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (code >> 6)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    return;
  }
  if (code < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (code >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    return;
  }
  out.push_back(static_cast<char>(0xF0u | (code >> 18)));
  out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
}

}  // namespace

bool IsAscii(std::string_view text) {
  // A word at a time. The high bit of every byte in the word is tested at once,
  // which is what makes this cheap enough to run on every `.length`.
  const char* data = text.data();
  std::size_t at = 0;
  constexpr std::size_t kWord = sizeof(std::uint64_t);
  constexpr std::uint64_t kHighBits = 0x8080808080808080ull;
  for (; at + kWord <= text.size(); at += kWord) {
    std::uint64_t word = 0;
    std::memcpy(&word, data + at, kWord);
    if ((word & kHighBits) != 0) {
      return false;
    }
  }
  for (; at < text.size(); ++at) {
    if (static_cast<unsigned char>(data[at]) >= 0x80u) {
      return false;
    }
  }
  return true;
}

std::size_t Utf16Length(std::string_view text) {
  if (IsAscii(text)) {
    return text.size();
  }
  std::size_t units = 0;
  for (std::size_t at = 0; at < text.size();) {
    const Decoded decoded = DecodeAt(text, at);
    units += UnitsFor(decoded.code);
    at += decoded.width;
  }
  return units;
}

std::size_t ByteOffsetOfUnit(std::string_view text, std::size_t unit) {
  if (IsAscii(text)) {
    return unit < text.size() ? unit : text.size();
  }
  std::size_t units = 0;
  for (std::size_t at = 0; at < text.size();) {
    if (units >= unit) {
      return at;
    }
    const Decoded decoded = DecodeAt(text, at);
    units += UnitsFor(decoded.code);
    at += decoded.width;
  }
  return text.size();
}

std::size_t UnitOffsetOfByte(std::string_view text, std::size_t at) {
  if (IsAscii(text)) {
    return at < text.size() ? at : text.size();
  }
  std::size_t units = 0;
  for (std::size_t scan = 0; scan < text.size() && scan < at;) {
    const Decoded decoded = DecodeAt(text, scan);
    units += UnitsFor(decoded.code);
    scan += decoded.width;
  }
  return units;
}

std::uint16_t CodeUnitAt(std::string_view text, std::size_t unit) {
  if (IsAscii(text)) {
    return unit < text.size() ? static_cast<std::uint16_t>(text[unit]) : 0;
  }
  std::size_t units = 0;
  for (std::size_t at = 0; at < text.size();) {
    const Decoded decoded = DecodeAt(text, at);
    const std::size_t width = UnitsFor(decoded.code);
    if (unit < units + width) {
      if (width == 1) {
        return static_cast<std::uint16_t>(decoded.code);
      }
      // The surrogate pair, split. Which half depends on where the index
      // landed, and this is the one place a code unit is not a whole scalar.
      const std::uint32_t offset = decoded.code - 0x10000u;
      return unit == units ? static_cast<std::uint16_t>(0xD800u + (offset >> 10))
                           : static_cast<std::uint16_t>(0xDC00u + (offset & 0x3FFu));
    }
    units += width;
    at += decoded.width;
  }
  return 0;
}

std::uint32_t CodePointAt(std::string_view text, std::size_t unit) {
  if (IsAscii(text)) {
    return unit < text.size() ? static_cast<std::uint32_t>(text[unit]) : 0;
  }
  const std::size_t at = ByteOffsetOfUnit(text, unit);
  if (at >= text.size()) {
    return 0;
  }
  const Decoded decoded = DecodeAt(text, at);
  // An index that landed on the low half of a pair reports that half alone,
  // which is what the spec says: only the leading index sees the whole point.
  if (UnitsFor(decoded.code) == 2 && UnitOffsetOfByte(text, at) != unit) {
    return CodeUnitAt(text, unit);
  }
  return decoded.code;
}

std::string SubstringUnits(std::string_view text, std::size_t begin, std::size_t end) {
  if (IsAscii(text)) {
    const std::size_t from = std::min(begin, text.size());
    const std::size_t to = std::min(std::max(end, from), text.size());
    return std::string(text.substr(from, to - from));
  }
  std::string out;
  std::size_t units = 0;
  for (std::size_t at = 0; at < text.size();) {
    const Decoded decoded = DecodeAt(text, at);
    const std::size_t width = UnitsFor(decoded.code);
    if (units >= end) {
      break;
    }
    if (units >= begin) {
      // A pair that straddles the boundary contributes only the half that is
      // inside it, which is why a slice can produce a lone surrogate -- and
      // why the encoder above has to be able to write one.
      if (width == 2 && units + 1 == end) {
        const std::uint32_t offset = decoded.code - 0x10000u;
        AppendScalar(out, 0xD800u + (offset >> 10));
      } else {
        AppendScalar(out, decoded.code);
      }
    } else if (width == 2 && units + 1 == begin) {
      const std::uint32_t offset = decoded.code - 0x10000u;
      AppendScalar(out, 0xDC00u + (offset & 0x3FFu));
    }
    units += width;
    at += decoded.width;
  }
  return out;
}

void AppendCodeUnit(std::string& out, std::uint16_t unit, std::uint32_t& pending) {
  if (pending != 0) {
    if (unit >= 0xDC00u && unit <= 0xDFFFu) {
      // The pair completes. One four-byte sequence rather than two three-byte
      // ones, which is what makes the round trip through charCodeAt exact.
      const std::uint32_t code =
          0x10000u + ((pending - 0xD800u) << 10) + (unit - 0xDC00u);
      pending = 0;
      AppendScalar(out, code);
      return;
    }
    // The high surrogate never found a partner. Written as itself, because
    // replacing it would lose what the page actually asked for.
    AppendScalar(out, pending);
    pending = 0;
  }
  if (unit >= 0xD800u && unit <= 0xDBFFu) {
    pending = unit;
    return;
  }
  AppendScalar(out, unit);
}

namespace {

// One scalar, cased.
//
// The ranges here are the ones where case is an arithmetic relationship rather
// than a table lookup, which is most of the alphabetic planes a page's text
// uses. The exceptions inside them are written out: they are what a table
// would otherwise be needed for, and there are few enough to read.
std::uint32_t UpperScalar(std::uint32_t code) {
  if (code < 0x80u) {
    return code >= 'a' && code <= 'z' ? code - 32 : code;
  }
  // Latin-1 Supplement. 0xDF is the sharp s, whose uppercase is two letters --
  // a length change this interface cannot express, so it is left as itself,
  // which is what every engine did before ES6 and what the spec still allows
  // for a locale-independent mapping.
  if (code >= 0xE0u && code <= 0xFEu && code != 0xF7u) {
    return code - 32;
  }
  if (code == 0xFFu) {
    return 0x178u;  // y with diaeresis, whose capital is outside the block
  }
  if (code == 0xB5u) {
    return 0x39Cu;  // micro sign uppercases to capital mu
  }
  // Latin Extended-A: pairs, except for a run near the end that is offset.
  if (code >= 0x100u && code <= 0x137u) {
    return (code % 2 == 1) ? code - 1 : code;
  }
  if (code >= 0x139u && code <= 0x148u) {
    return (code % 2 == 0) ? code - 1 : code;
  }
  if (code >= 0x14Au && code <= 0x177u) {
    return (code % 2 == 1) ? code - 1 : code;
  }
  if (code >= 0x179u && code <= 0x17Eu) {
    return (code % 2 == 0) ? code - 1 : code;
  }
  // Greek, with the two final-sigma forms both mapping to capital sigma.
  if (code >= 0x3B1u && code <= 0x3C1u) {
    return code - 32;
  }
  if (code == 0x3C2u) {
    return 0x3A3u;
  }
  if (code >= 0x3C3u && code <= 0x3CBu) {
    return code - 32;
  }
  // Cyrillic.
  if (code >= 0x430u && code <= 0x44Fu) {
    return code - 32;
  }
  if (code >= 0x450u && code <= 0x45Fu) {
    return code - 80;
  }
  return code;
}

std::uint32_t LowerScalar(std::uint32_t code) {
  if (code < 0x80u) {
    return code >= 'A' && code <= 'Z' ? code + 32 : code;
  }
  if (code >= 0xC0u && code <= 0xDEu && code != 0xD7u) {
    return code + 32;
  }
  if (code == 0x178u) {
    return 0xFFu;
  }
  if (code >= 0x100u && code <= 0x137u) {
    return (code % 2 == 0) ? code + 1 : code;
  }
  if (code >= 0x139u && code <= 0x148u) {
    return (code % 2 == 1) ? code + 1 : code;
  }
  if (code >= 0x14Au && code <= 0x177u) {
    return (code % 2 == 0) ? code + 1 : code;
  }
  if (code >= 0x179u && code <= 0x17Eu) {
    return (code % 2 == 1) ? code + 1 : code;
  }
  if (code >= 0x391u && code <= 0x3A1u) {
    return code + 32;
  }
  if (code >= 0x3A3u && code <= 0x3ABu) {
    return code + 32;
  }
  if (code >= 0x410u && code <= 0x42Fu) {
    return code + 32;
  }
  if (code >= 0x400u && code <= 0x40Fu) {
    return code + 80;
  }
  return code;
}

std::string MapCase(std::string_view text, std::uint32_t (*map)(std::uint32_t)) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t at = 0; at < text.size();) {
    const Decoded decoded = DecodeAt(text, at);
    AppendScalar(out, map(decoded.code));
    at += decoded.width;
  }
  return out;
}

}  // namespace

std::string ToUpper(std::string_view text) { return MapCase(text, UpperScalar); }
std::string ToLower(std::string_view text) { return MapCase(text, LowerScalar); }

void FlushCodeUnit(std::string& out, std::uint32_t& pending) {
  if (pending != 0) {
    AppendScalar(out, pending);
    pending = 0;
  }
}

}  // namespace microbrowser::js

// The legacy multi-byte decoders: Shift_JIS, EUC-JP, EUC-KR, Big5 and GB18030.
//
// ADR 0025 §2, session 32. Its own translation unit because the tables are 648KB of generated data and
// the four byte-structure algorithms over them are the only thing here -- Encoding.cpp is the
// *algorithm* (sniffing, substitution, single-byte tables) and this is the part that is a table.
//
// **Why these five and not others.** ADR 0025 §2 chose them by usage: they are the encodings a page
// from before UTF-8 won is actually in. Everything about them is the same shape -- a lead byte, a trail
// byte, and a pointer arithmetic that indexes one of four indexes -- so the difficulty is not the
// decoding, it is that each has its own ranges and its own exceptions, and getting a range wrong
// produces *plausible wrong characters* rather than a failure. A Japanese page decoded with a
// one-off-by-one lead range renders as a different Japanese sentence.
//
// Every one of them refuses rather than guesses. An ill-formed sequence is U+FFFD, and the byte that
// ended it is not consumed -- the same rule Encoding.cpp's UTF-8 decoder follows and for the same
// reason: swallowing it hides the character a sanitiser was looking for.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "html/Encoding.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

#include "html/EncodingIndexes.inc"

constexpr std::uint16_t kHole = 0xFFFF;
constexpr std::uint32_t kWideHole = 0xFFFFFFFFu;

void AppendReplacement(std::string& out) {
  out += "\xEF\xBF\xBD";
  util::AddPerformanceCounter(util::PerfCounterId::EncodingReplacements);
}

// An index lookup that cannot read past the end and cannot return a hole as a character. Every
// decoder below goes through it, so the bound is stated once -- a pointer computed from two bytes a
// stranger wrote is exactly the arithmetic that overruns a table.
template <typename Table>
bool IndexLookup(const Table& table, std::size_t count, std::size_t pointer, std::uint32_t hole,
                 std::uint32_t& out) {
  if (pointer >= count) {
    return false;
  }
  const std::uint32_t value = table[pointer];
  if (value == hole) {
    return false;
  }
  out = value;
  return true;
}

// Shift_JIS. Single bytes are ASCII up to 0x80, halfwidth katakana from 0xA1 to 0xDF; a two-byte
// sequence is a lead in 0x81-0x9F or 0xE0-0xFC with a trail in 0x40-0x7E or 0x80-0xFC.
//
// The `pointer < 8836` guard exists because Shift_JIS's pointer space is larger than the JIS0208
// index: pointers 8836 and above are the "extension" area, which the standard maps to a private-use
// range rather than to the index. Those are the characters a Japanese vendor added and a page may
// legitimately contain.
std::string DecodeShiftJis(std::string_view bytes) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte < 0x80) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    // 0x80 is U+0080, and it is written *through AppendUtf8* rather than pushed. It looks like a
    // pass-through and is not: U+0080 is two bytes in UTF-8, and pushing the raw byte emits a lone
    // continuation byte -- ill-formed output from a decoder whose entire job is to produce UTF-8.
    // Found by the differential sweep in the same commit, on all 66 sequences that reach it.
    if (byte == 0x80) {
      util::AppendUtf8(out, 0x80u);
      ++at;
      continue;
    }
    if (byte >= 0xA1 && byte <= 0xDF) {
      util::AppendUtf8(out, 0xFF61u + byte - 0xA1u);
      ++at;
      continue;
    }
    if ((byte < 0x81 || byte > 0x9F) && (byte < 0xE0 || byte > 0xFC)) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    if (at + 1 >= bytes.size()) {
      AppendReplacement(out);
      break;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if ((trail < 0x40 || trail > 0x7E) && (trail < 0x80 || trail > 0xFC)) {
      // The trail is not a trail, so the sequence is one byte long and this byte starts whatever
      // comes next -- which may be an ASCII `<`.
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::size_t lead_offset = byte < 0xA0 ? 0x81u : 0xC1u;
    const std::size_t trail_offset = trail < 0x7F ? 0x40u : 0x41u;
    const std::size_t pointer =
        (static_cast<std::size_t>(byte) - lead_offset) * 188u +
        (static_cast<std::size_t>(trail) - trail_offset);
    std::uint32_t code = 0;
    if (pointer >= 8836 && pointer <= 10715) {
      // The extension area: a private-use code point rather than an index entry, which is what the
      // standard says and what keeps a vendor character from becoming U+FFFD.
      code = 0xE000u + static_cast<std::uint32_t>(pointer) - 8836u;
    } else if (!IndexLookup(kJis0208, std::size(kJis0208), pointer, kHole, code)) {
      AppendReplacement(out);
      at += 2;
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// EUC-JP. 0x8E introduces halfwidth katakana, 0x8F introduces JIS X 0212 -- which this browser does
// not have an index for, so those three-byte sequences are U+FFFD. That is a stated gap rather than a
// silent one: JIS0212 is a supplementary kanji set, rare on the web, and guessing a character from
// the wrong index would be worse than a replacement.
std::string DecodeEucJp(std::string_view bytes) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte == 0x8E && at + 1 < bytes.size()) {
      const std::uint8_t katakana = static_cast<std::uint8_t>(bytes[at + 1]);
      if (katakana >= 0xA1 && katakana <= 0xDF) {
        util::AppendUtf8(out, 0xFF61u + katakana - 0xA1u);
        at += 2;
        continue;
      }
      AppendReplacement(out);
      ++at;
      continue;
    }
    if (byte == 0x8F) {
      // JIS X 0212, which needs an index this browser does not carry -- so the answer is U+FFFD, and
      // **the shape is checked before three bytes are consumed.** That is not tidiness: consuming
      // blindly would let `8F 3C` delete a `<`, and a decoder that deletes a `<` hides the character
      // a sanitiser was looking for. When the trail bytes are not trail bytes, one byte is consumed
      // and everything after it is decoded as text, which is what the standard's pushback does.
      const bool shaped = at + 2 < bytes.size() &&
                          static_cast<std::uint8_t>(bytes[at + 1]) >= 0xA1 &&
                          static_cast<std::uint8_t>(bytes[at + 1]) <= 0xFE &&
                          static_cast<std::uint8_t>(bytes[at + 2]) >= 0xA1 &&
                          static_cast<std::uint8_t>(bytes[at + 2]) <= 0xFE;
      AppendReplacement(out);
      at += shaped ? std::size_t{3} : std::size_t{1};
      continue;
    }
    if (byte < 0xA1 || byte > 0xFE || at + 1 >= bytes.size()) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if (trail < 0xA1 || trail > 0xFE) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0xA1u) * 94u +
                                (static_cast<std::size_t>(trail) - 0xA1u);
    std::uint32_t code = 0;
    if (!IndexLookup(kJis0208, std::size(kJis0208), pointer, kHole, code)) {
      AppendReplacement(out);
      at += 2;
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// EUC-KR. One index, one range, and the widest trail range of the five: 0x41-0xFE, which is why its
// index is 23,750 entries for a 94x94 character set.
std::string DecodeEucKr(std::string_view bytes) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte < 0x81 || byte > 0xFE || at + 1 >= bytes.size()) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if (trail < 0x41 || trail > 0xFE) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0x81u) * 190u +
                                (static_cast<std::size_t>(trail) - 0x41u);
    std::uint32_t code = 0;
    if (!IndexLookup(kEucKr, std::size(kEucKr), pointer, kHole, code)) {
      AppendReplacement(out);
      at += 2;
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// Big5. Four pointers decode to *two* code points each -- a base letter plus a combining diacritic --
// which is the one place in these five where one sequence is not one character. Handled explicitly
// because a decoder that emitted only the base would silently drop a tone mark, and Taiwanese
// Mandarin transcription is exactly what those four are for.
std::string DecodeBig5(std::string_view bytes) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte < 0x81 || byte > 0xFE || at + 1 >= bytes.size()) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if ((trail < 0x40 || trail > 0x7E) && (trail < 0xA1 || trail > 0xFE)) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::size_t offset = trail < 0x7F ? 0x40u : 0x62u;
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0x81u) * 157u +
                                (static_cast<std::size_t>(trail) - offset);
    // The four two-code-point pointers, from the standard's own table.
    switch (pointer) {
      case 1133:
        util::AppendUtf8(out, 0x00CA);
        util::AppendUtf8(out, 0x0304);
        at += 2;
        continue;
      case 1135:
        util::AppendUtf8(out, 0x00CA);
        util::AppendUtf8(out, 0x030C);
        at += 2;
        continue;
      case 1164:
        util::AppendUtf8(out, 0x00EA);
        util::AppendUtf8(out, 0x0304);
        at += 2;
        continue;
      case 1166:
        util::AppendUtf8(out, 0x00EA);
        util::AppendUtf8(out, 0x030C);
        at += 2;
        continue;
      default:
        break;
    }
    std::uint32_t code = 0;
    if (!IndexLookup(kBig5, std::size(kBig5), pointer, kWideHole, code)) {
      AppendReplacement(out);
      at += 2;
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// GB18030, two-byte form. 0x80 alone is the euro sign -- a single-byte exception no other encoding
// here has -- and the two-byte form covers the BMP.
//
// **The four-byte form is refused**, and that is a stated gap: it encodes the code points the
// two-byte form cannot reach, which for a web page means rare ideographs and everything above the
// BMP. Implementing it needs the standard's `index-gb18030-ranges` table and an offset search; a
// four-byte sequence therefore produces one U+FFFD rather than a plausible wrong character, which is
// the same trade EUC-JP's JIS0212 makes.
std::string DecodeGb18030(std::string_view bytes) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte == 0x80) {
      util::AppendUtf8(out, 0x20AC);  // the euro sign, GB18030's one single-byte extension
      ++at;
      continue;
    }
    if (byte == 0xFF || at + 1 >= bytes.size()) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::uint8_t second = static_cast<std::uint8_t>(bytes[at + 1]);
    if (second >= 0x30 && second <= 0x39) {
      // A four-byte sequence, which this decoder refuses -- see the note above. **Its whole shape is
      // checked before four bytes are consumed**, for the reason EUC-JP's 0x8F path states: the third
      // and fourth bytes of a *malformed* four-byte sequence are ordinary text, and `81 30 3C 3C`
      // consumed blindly deletes a `<`. Only a sequence whose four bytes are all in range is taken as
      // one unit; anything else costs one byte, and the rest is decoded.
      const bool shaped = at + 3 < bytes.size() &&
                          static_cast<std::uint8_t>(bytes[at + 2]) >= 0x81 &&
                          static_cast<std::uint8_t>(bytes[at + 2]) <= 0xFE &&
                          static_cast<std::uint8_t>(bytes[at + 3]) >= 0x30 &&
                          static_cast<std::uint8_t>(bytes[at + 3]) <= 0x39;
      AppendReplacement(out);
      at += shaped ? std::size_t{4} : std::size_t{1};
      continue;
    }
    if (second == 0x7F || second < 0x40 || second > 0xFE) {
      AppendReplacement(out);
      ++at;
      continue;
    }
    const std::size_t offset = second < 0x7F ? 0x40u : 0x41u;
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0x81u) * 190u +
                                (static_cast<std::size_t>(second) - offset);
    std::uint32_t code = 0;
    if (!IndexLookup(kGb18030, std::size(kGb18030), pointer, kHole, code)) {
      AppendReplacement(out);
      at += 2;
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

}  // namespace

std::string DecodeMultiByte(std::string_view bytes, Encoding encoding) {
  switch (encoding) {
    case Encoding::ShiftJis:
      return DecodeShiftJis(bytes);
    case Encoding::EucJp:
      return DecodeEucJp(bytes);
    case Encoding::EucKr:
      return DecodeEucKr(bytes);
    case Encoding::Big5:
      return DecodeBig5(bytes);
    case Encoding::Gb18030:
      return DecodeGb18030(bytes);
    default:
      return std::string();
  }
}

}  // namespace microbrowser::html

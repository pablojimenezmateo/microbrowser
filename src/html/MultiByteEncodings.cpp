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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
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

bool HoldIncomplete(std::string_view bytes, std::size_t at, std::string* leftover) {
  if (leftover == nullptr) {
    return false;
  }
  leftover->assign(bytes.substr(at));
  return true;
}

// TextDecoder's fatal flag: a replacement is a failure rather than U+FFFD.
bool EmitError(std::string& out, bool fatal, bool* failed) {
  if (fatal) {
    if (failed != nullptr) {
      *failed = true;
    }
    return false;
  }
  AppendReplacement(out);
  return true;
}

// A failed two-byte sequence consumes the lead; an ASCII trail is restored.
void AdvanceAfterTwoByteError(std::size_t& at, std::uint8_t trail) {
  ++at;
  if (trail > 0x7F) {
    ++at;
  }
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
std::string DecodeShiftJis(std::string_view bytes, std::string* leftover, bool fatal = false,
                           bool* failed = nullptr) {
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
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    if (at + 1 >= bytes.size()) {
      if (HoldIncomplete(bytes, at, leftover)) {
        break;
      }
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      break;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if ((trail < 0x40 || trail > 0x7E) && (trail < 0x80 || trail > 0xFC)) {
      // The trail is not a trail, so the sequence is one byte long and this byte starts whatever
      // comes next -- which may be an ASCII `<`.
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, trail);
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
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, trail);
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// EUC-JP. 0x8E introduces halfwidth katakana and 0x8F introduces JIS X 0212, the supplementary kanji
// set -- a second index, and the only one of the six that is *decode only*: no encoder in the
// standard produces a 0x8F sequence, so a document containing one was written by something older
// than the web.
std::string DecodeEucJp(std::string_view bytes, std::string* leftover, bool fatal = false,
                        bool* failed = nullptr) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte == 0x8E) {
      if (at + 1 >= bytes.size()) {
        if (HoldIncomplete(bytes, at, leftover)) {
          break;
        }
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        ++at;
        continue;
      }
      const std::uint8_t katakana = static_cast<std::uint8_t>(bytes[at + 1]);
      if (katakana >= 0xA1 && katakana <= 0xDF) {
        util::AppendUtf8(out, 0xFF61u + katakana - 0xA1u);
        at += 2;
        continue;
      }
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, katakana);
      continue;
    }
    if (byte == 0x8F) {
      // JIS X 0212. **The shape is checked before three bytes are consumed**, and that is not
      // tidiness: consuming blindly would let `8F 3C` delete a `<`, and a decoder that deletes a `<`
      // hides the character a sanitiser was looking for. When the trail bytes are not trail bytes,
      // one byte is consumed and everything after it is decoded as text, which is what the
      // standard's pushback does. End-of-queue with a 0x8F prefix still in flight is one error for
      // the whole remainder -- `8F A1` at flush is one U+FFFD, not two.
      if (at + 1 >= bytes.size()) {
        if (HoldIncomplete(bytes, at, leftover)) {
          break;
        }
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        at = bytes.size();
        continue;
      }
      const std::uint8_t first_trail = static_cast<std::uint8_t>(bytes[at + 1]);
      if (first_trail < 0xA1 || first_trail > 0xFE) {
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        AdvanceAfterTwoByteError(at, first_trail);
        continue;
      }
      if (at + 2 >= bytes.size()) {
        if (HoldIncomplete(bytes, at, leftover)) {
          break;
        }
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        at = bytes.size();
        continue;
      }
      const std::uint8_t second_trail = static_cast<std::uint8_t>(bytes[at + 2]);
      if (second_trail < 0xA1 || second_trail > 0xFE) {
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        at += 2;
        if (second_trail > 0x7F) {
          ++at;
        }
        continue;
      }
      const std::size_t pointer = (static_cast<std::size_t>(first_trail) - 0xA1u) * 94u +
                                  (static_cast<std::size_t>(second_trail) - 0xA1u);
      std::uint32_t code = 0;
      if (IndexLookup(kJis0212, std::size(kJis0212), pointer, kHole, code)) {
        util::AppendUtf8(out, code);
      } else if (!EmitError(out, fatal, failed)) {
        return out;
      }
      at += 3;
      continue;
    }
    if (byte < 0xA1 || byte > 0xFE) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    if (at + 1 >= bytes.size()) {
      if (HoldIncomplete(bytes, at, leftover)) {
        break;
      }
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if (trail < 0xA1 || trail > 0xFE) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, trail);
      continue;
    }
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0xA1u) * 94u +
                                (static_cast<std::size_t>(trail) - 0xA1u);
    std::uint32_t code = 0;
    if (!IndexLookup(kJis0208, std::size(kJis0208), pointer, kHole, code)) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, trail);
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// EUC-KR. One index, one range, and the widest trail range of the five: 0x41-0xFE, which is why its
// index is 23,750 entries for a 94x94 character set.
std::string DecodeEucKr(std::string_view bytes, std::string* leftover, bool fatal = false,
                        bool* failed = nullptr) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte < 0x81 || byte > 0xFE) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    if (at + 1 >= bytes.size()) {
      if (HoldIncomplete(bytes, at, leftover)) {
        break;
      }
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if (trail < 0x41 || trail > 0xFE) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, trail);
      continue;
    }
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0x81u) * 190u +
                                (static_cast<std::size_t>(trail) - 0x41u);
    std::uint32_t code = 0;
    if (!IndexLookup(kEucKr, std::size(kEucKr), pointer, kHole, code)) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, trail);
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
std::string DecodeBig5(std::string_view bytes, std::string* leftover, bool fatal = false,
                       bool* failed = nullptr) {
  std::string out;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t byte = static_cast<std::uint8_t>(bytes[at]);
    if (byte <= 0x7F) {
      out.push_back(static_cast<char>(byte));
      ++at;
      continue;
    }
    if (byte < 0x81 || byte > 0xFE) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    if (at + 1 >= bytes.size()) {
      // A lead with nothing after it. Streaming holds it; a flush is U+FFFD.
      if (leftover != nullptr) {
        leftover->assign(bytes.substr(at));
        break;
      }
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    const std::uint8_t trail = static_cast<std::uint8_t>(bytes[at + 1]);
    if ((trail < 0x40 || trail > 0x7E) && (trail < 0xA1 || trail > 0xFE)) {
      // Pointer stays null. An ASCII trail is restored; a non-ASCII one is consumed.
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      if (trail > 0x7F) {
        ++at;
      }
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
      // Encoding Standard: a hole is an error, and an ASCII trail is restored so it is
      // decoded on its own. `0x81 0x40` is U+FFFD then `@`, not a swallowed `@`.
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      if (trail > 0x7F) {
        ++at;
      }
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// The standard's "index gb18030 ranges code point": the four-byte form's pointer turned back into a
// character. 207 ranges cover a million code points, so the table is a list of starts and the answer
// is a start plus a distance -- which is also why the two guard clauses are not optional. The gap
// between 39419 and 189000 is the hole between the BMP and the astral planes, and a pointer landing
// in it must be an error rather than a start-plus-distance that walks off the end of a range.
std::optional<std::uint32_t> Gb18030RangesCodePoint(std::size_t pointer) {
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
    return std::nullopt;
  }
  if (pointer == 7457) {
    return 0xE7C7;  // the GB18030-2005 revision's one inline exception
  }
  const auto* begin = std::begin(kGb18030RangePointers);
  const auto* end = begin + std::size(kGb18030RangePointers);
  const auto* above = std::upper_bound(begin, end, static_cast<std::uint32_t>(pointer));
  if (above == begin) {
    return std::nullopt;
  }
  const std::size_t at = static_cast<std::size_t>(above - begin) - 1;
  return static_cast<std::uint32_t>(kGb18030RangeCodePoints[at] +
                                    (pointer - kGb18030RangePointers[at]));
}

// GB18030. 0x80 alone is the euro sign -- a single-byte exception no other encoding here has -- the
// two-byte form covers the BMP, and the four-byte form covers everything else, including every code
// point above it. The four-byte form is what makes this the only legacy encoding that can say
// anything Unicode can.
std::string DecodeGb18030(std::string_view bytes, std::string* leftover, bool fatal = false,
                         bool* failed = nullptr) {
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
    if (byte == 0xFF) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    if (at + 1 >= bytes.size()) {
      if (HoldIncomplete(bytes, at, leftover)) {
        break;
      }
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      ++at;
      continue;
    }
    const std::uint8_t second = static_cast<std::uint8_t>(bytes[at + 1]);
    if (second >= 0x30 && second <= 0x39) {
      // A four-byte sequence. **Its whole shape is checked before four bytes are consumed**, for the
      // reason EUC-JP's 0x8F path states: the third and fourth bytes of a *malformed* four-byte
      // sequence are ordinary text, and `81 30 3C 3C` consumed blindly deletes a `<`. Only a
      // sequence whose four bytes are all in range is taken as one unit; anything else costs one
      // byte, and the rest is decoded. End-of-queue with first/second/third still set is one error
      // for the remainder -- `81 30` and `81 30 FE` at flush are each one U+FFFD. A third byte out
      // of range is not that: restore second and third, so `A0 30 2B` is U+FFFD then `0+`.
      if (at + 2 >= bytes.size()) {
        if (HoldIncomplete(bytes, at, leftover)) {
          break;
        }
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        at = bytes.size();
        continue;
      }
      const std::uint8_t third = static_cast<std::uint8_t>(bytes[at + 2]);
      if (third < 0x81 || third > 0xFE) {
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        ++at;
        continue;
      }
      if (at + 3 >= bytes.size()) {
        if (HoldIncomplete(bytes, at, leftover)) {
          break;
        }
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        at = bytes.size();
        continue;
      }
      const std::uint8_t fourth = static_cast<std::uint8_t>(bytes[at + 3]);
      if (fourth < 0x30 || fourth > 0x39) {
        if (!EmitError(out, fatal, failed)) {
          return out;
        }
        ++at;
        continue;
      }
      const std::size_t pointer =
          (static_cast<std::size_t>(byte) - 0x81u) * (10u * 126u * 10u) +
          (static_cast<std::size_t>(second) - 0x30u) * (10u * 126u) +
          (static_cast<std::size_t>(third) - 0x81u) * 10u +
          (static_cast<std::size_t>(fourth) - 0x30u);
      if (const std::optional<std::uint32_t> code = Gb18030RangesCodePoint(pointer)) {
        util::AppendUtf8(out, *code);
      } else if (!EmitError(out, fatal, failed)) {
        return out;
      }
      at += 4;
      continue;
    }
    if (second == 0x7F || second < 0x40 || second > 0xFE) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, second);
      continue;
    }
    const std::size_t offset = second < 0x7F ? 0x40u : 0x41u;
    const std::size_t pointer = (static_cast<std::size_t>(byte) - 0x81u) * 190u +
                                (static_cast<std::size_t>(second) - offset);
    std::uint32_t code = 0;
    if (!IndexLookup(kGb18030, std::size(kGb18030), pointer, kHole, code)) {
      if (!EmitError(out, fatal, failed)) {
        return out;
      }
      AdvanceAfterTwoByteError(at, second);
      continue;
    }
    util::AppendUtf8(out, code);
    at += 2;
  }
  return out;
}

// ISO-2022-JP, and the only member of the family where a byte's meaning depends on bytes some
// distance behind it. `41` is `A` after `ESC ( B` and half of a kanji after `ESC $ B`.
//
// **That is a security property before it is a rendering one.** A stateful encoding is one where a
// sanitiser that scanned for `<` in the bytes has scanned the wrong thing, because the escape that
// decides whether `3C` *is* a `<` may be anywhere earlier in the document. It is decoded here
// exactly as the standard writes it, pushback and all, so that this browser and the server that
// filtered the document agree about which bytes are markup.
//
// The two states that are not character sets -- escape start and escape -- are why this needs a
// pushback queue rather than an index: a partial escape sequence is *restored* to the input and
// decoded as text, so `ESC (` followed by `X` produces the replacement character and then `(` and
// `X`, and nothing is swallowed.
std::string DecodeIso2022Jp(std::string_view bytes, std::string* leftover = nullptr,
                            bool stream = false, bool fatal = false, bool* failed = nullptr) {
  enum class State : std::uint8_t {
    Ascii,
    Roman,
    Katakana,
    LeadByte,
    TrailingByte,
    EscapeStart,
    Escape
  };
  std::string out;
  State state = State::Ascii;
  // Where an unrecognised escape sequence falls back to, which is not the same as `state`: the
  // escape states are transient and have no character set of their own.
  State output_state = State::Ascii;
  std::uint8_t lead = 0;
  // The standard's "ISO-2022-JP output" flag. It exists so that an escape to the state the decoder
  // is *already* in is an error rather than a no-op -- `ESC ( B ESC ( B` is a document trying to
  // hide something in what looks like a redundant escape.
  bool output = false;
  if (leftover != nullptr && leftover->size() >= 4) {
    const auto loaded = static_cast<State>(static_cast<std::uint8_t>((*leftover)[0]));
    if (loaded <= State::Escape) {
      state = loaded;
      output_state = static_cast<State>(static_cast<std::uint8_t>((*leftover)[1]));
      lead = static_cast<std::uint8_t>((*leftover)[2]);
      output = (*leftover)[3] != 0;
    }
  }
  if (leftover != nullptr) {
    leftover->clear();
  }
  const auto save = [&] {
    if (leftover == nullptr || !stream) {
      return;
    }
    leftover->push_back(static_cast<char>(state));
    leftover->push_back(static_cast<char>(output_state));
    leftover->push_back(static_cast<char>(lead));
    leftover->push_back(output ? 1 : 0);
  };
  const auto fail = [&] {
    return !EmitError(out, fatal, failed);
  };

  std::size_t at = 0;
  std::uint8_t pushback[2] = {0, 0};
  int pushed = 0;
  const auto read = [&]() -> int {
    if (pushed > 0) {
      const std::uint8_t byte = pushback[0];
      pushback[0] = pushback[1];
      --pushed;
      return byte;
    }
    return at < bytes.size() ? static_cast<int>(static_cast<std::uint8_t>(bytes[at++])) : -1;
  };
  const auto restore = [&](std::uint8_t first) {
    pushback[1] = pushback[0];
    pushback[0] = first;
    ++pushed;
  };

  for (;;) {
    const int byte = read();
    switch (state) {
      case State::Ascii:
        if (byte == 0x1B) {
          state = State::EscapeStart;
        } else if (byte >= 0x00 && byte <= 0x7F && byte != 0x0E && byte != 0x0F) {
          output = false;
          out.push_back(static_cast<char>(byte));
        } else if (byte < 0) {
          save();
          return out;
        } else {
          output = false;
          if (fail()) {
            save();
            return out;
          }
        }
        break;

      case State::Roman:
        if (byte == 0x1B) {
          state = State::EscapeStart;
        } else if (byte == 0x5C) {
          output = false;
          util::AppendUtf8(out, 0x00A5);
        } else if (byte == 0x7E) {
          output = false;
          util::AppendUtf8(out, 0x203E);
        } else if (byte >= 0x00 && byte <= 0x7F && byte != 0x0E && byte != 0x0F) {
          output = false;
          out.push_back(static_cast<char>(byte));
        } else if (byte < 0) {
          save();
          return out;
        } else {
          output = false;
          if (fail()) {
            save();
            return out;
          }
        }
        break;

      case State::Katakana:
        if (byte == 0x1B) {
          state = State::EscapeStart;
        } else if (byte >= 0x21 && byte <= 0x5F) {
          output = false;
          util::AppendUtf8(out, 0xFF61u - 0x21u + static_cast<std::uint32_t>(byte));
        } else if (byte < 0) {
          save();
          return out;
        } else {
          output = false;
          if (fail()) {
            save();
            return out;
          }
        }
        break;

      case State::LeadByte:
        if (byte == 0x1B) {
          state = State::EscapeStart;
        } else if (byte >= 0x21 && byte <= 0x7E) {
          output = false;
          lead = static_cast<std::uint8_t>(byte);
          state = State::TrailingByte;
        } else if (byte < 0) {
          save();
          return out;
        } else {
          output = false;
          if (fail()) {
            save();
            return out;
          }
        }
        break;

      case State::TrailingByte:
        if (byte == 0x1B) {
          state = State::EscapeStart;
          if (fail()) {
            save();
            return out;
          }
        } else if (byte >= 0x21 && byte <= 0x7E) {
          state = State::LeadByte;
          const std::size_t pointer = (static_cast<std::size_t>(lead) - 0x21u) * 94u +
                                      static_cast<std::size_t>(byte) - 0x21u;
          std::uint32_t code = 0;
          if (IndexLookup(kJis0208, std::size(kJis0208), pointer, kHole, code)) {
            util::AppendUtf8(out, code);
          } else if (fail()) {
            save();
            return out;
          }
        } else if (byte < 0) {
          if (stream) {
            save();
            return out;
          }
          state = State::LeadByte;
          if (fail()) {
            save();
            return out;
          }
        } else {
          state = State::LeadByte;
          if (fail()) {
            save();
            return out;
          }
        }
        break;

      case State::EscapeStart:
        if (byte == 0x24 || byte == 0x28) {
          lead = static_cast<std::uint8_t>(byte);
          state = State::Escape;
        } else if (byte < 0 && stream) {
          save();
          return out;
        } else {
          if (byte >= 0) {
            restore(static_cast<std::uint8_t>(byte));
          }
          output = false;
          state = output_state;
          if (fail()) {
            save();
            return out;
          }
        }
        break;

      case State::Escape: {
        const std::uint8_t leading = lead;
        lead = 0;
        std::optional<State> next;
        if (leading == 0x28 && byte == 0x42) {
          next = State::Ascii;
        } else if (leading == 0x28 && byte == 0x4A) {
          next = State::Roman;
        } else if (leading == 0x28 && byte == 0x49) {
          next = State::Katakana;
        } else if (leading == 0x24 && (byte == 0x40 || byte == 0x42)) {
          next = State::LeadByte;
        }
        if (next.has_value()) {
          state = *next;
          output_state = *next;
          const bool had_output = output;
          output = true;
          if (had_output && fail()) {
            save();
            return out;
          }
          break;
        }
        // Not an escape sequence at all: both bytes go back and are decoded as whatever the
        // character set in force says they are.
        if (byte < 0 && stream) {
          lead = leading;
          state = State::Escape;
          save();
          return out;
        }
        if (byte < 0) {
          restore(leading);
        } else {
          pushback[0] = leading;
          pushback[1] = static_cast<std::uint8_t>(byte);
          pushed = 2;
        }
        output = false;
        state = output_state;
        if (fail()) {
          save();
          return out;
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The encoders.
//
// **These are not the decode tables read backwards, and that is the whole of the difficulty.** Each
// index maps two pointers to one code point in places, and the standard picks between them with a
// rule per encoding -- Shift_JIS drops a range of pointers before taking the first match, Big5 drops
// the Hong Kong supplement and takes the *last* match for six characters. Inverting a decode table
// without those rules produces bytes that decode back to the right character and are still not the
// bytes any other browser sends, which is a wrong answer that no round-trip test can see. The rules
// live in tools/unicode/generate_encodings.py, where the tables are built.
//
// The lookup is a binary search over a sorted array of code points with the pointer at the same
// subscript. The *pointer* is what is stored rather than the bytes, because every encoder divides it
// differently -- 94, 157, 188, 190 -- and storing bytes would mean five tables where the standard
// has one index.

// The standard's "index pointer for code point", already specialised per encoding by the generator.
// Nullopt is what the standard calls null and is the encoder's error case.
template <typename Codes, typename Pointers>
std::optional<std::size_t> IndexPointer(const Codes& codes, const Pointers& pointers,
                                        std::size_t count, std::uint32_t code_point) {
  const auto* begin = std::begin(codes);
  const auto* end = begin + count;
  const auto* found = std::lower_bound(begin, end, code_point);
  if (found == end || static_cast<std::uint32_t>(*found) != code_point) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(pointers[found - begin]);
}

// Takes a `size_t` because every caller is the result of the standard's pointer arithmetic, which is
// done in the widest type available for the reason every parser in this repository does: a division
// and a remainder over a value derived from input is where an overflow would be, and narrowing at
// the one point the byte is written is where it is checked.
void PushByte(std::string& out, std::size_t byte) {
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(byte & 0xFFu)));
}

bool IsAscii(std::uint32_t code_point) { return code_point <= 0x7F; }

bool EncodeShiftJis(std::uint32_t code_point, std::string& out) {
  if (IsAscii(code_point) || code_point == 0x80) {
    PushByte(out, code_point);
    return true;
  }
  // The yen sign and the overline are 0x5C and 0x7E, which are also `\` and `~`. That is the
  // standard, and it is the asymmetry that makes Shift_JIS round-tripping impossible: those two
  // bytes decode as backslash and tilde, so encoding then decoding U+00A5 gives U+005C.
  if (code_point == 0x00A5) {
    PushByte(out, 0x5C);
    return true;
  }
  if (code_point == 0x203E) {
    PushByte(out, 0x7E);
    return true;
  }
  if (code_point >= 0xFF61 && code_point <= 0xFF9F) {
    PushByte(out, code_point - 0xFF61 + 0xA1);
    return true;
  }
  if (code_point == 0x2212) {
    code_point = 0xFF0D;  // minus sign to fullwidth hyphen-minus, which is the only one it has
  }
  const std::optional<std::size_t> pointer = IndexPointer(
      kShiftJisEncodeCodes, kShiftJisEncodePointers, std::size(kShiftJisEncodeCodes), code_point);
  if (!pointer.has_value()) {
    return false;
  }
  const std::size_t lead = *pointer / 188;
  const std::size_t trail = *pointer % 188;
  PushByte(out, lead + (lead < 0x1F ? 0x81u : 0xC1u));
  PushByte(out, trail + (trail < 0x3F ? 0x40u : 0x41u));
  return true;
}

bool EncodeEucJp(std::uint32_t code_point, std::string& out) {
  if (IsAscii(code_point)) {
    PushByte(out, code_point);
    return true;
  }
  if (code_point == 0x00A5) {
    PushByte(out, 0x5C);
    return true;
  }
  if (code_point == 0x203E) {
    PushByte(out, 0x7E);
    return true;
  }
  if (code_point >= 0xFF61 && code_point <= 0xFF9F) {
    PushByte(out, 0x8E);
    PushByte(out, code_point - 0xFF61 + 0xA1);
    return true;
  }
  if (code_point == 0x2212) {
    code_point = 0xFF0D;
  }
  // jis0208 only. **JIS X 0212 is decoded and never encoded**, which is the standard's rule rather
  // than a gap here: the 0x8F sequences exist in old documents and no browser produces new ones.
  const std::optional<std::size_t> pointer = IndexPointer(
      kJis0208EncodeCodes, kJis0208EncodePointers, std::size(kJis0208EncodeCodes), code_point);
  if (!pointer.has_value()) {
    return false;
  }
  PushByte(out, *pointer / 94 + 0xA1);
  PushByte(out, *pointer % 94 + 0xA1);
  return true;
}

bool EncodeEucKr(std::uint32_t code_point, std::string& out) {
  if (IsAscii(code_point)) {
    PushByte(out, code_point);
    return true;
  }
  const std::optional<std::size_t> pointer = IndexPointer(
      kEucKrEncodeCodes, kEucKrEncodePointers, std::size(kEucKrEncodeCodes), code_point);
  if (!pointer.has_value()) {
    return false;
  }
  PushByte(out, *pointer / 190 + 0x81);
  PushByte(out, *pointer % 190 + 0x41);
  return true;
}

bool EncodeBig5(std::uint32_t code_point, std::string& out) {
  if (IsAscii(code_point)) {
    PushByte(out, code_point);
    return true;
  }
  const std::optional<std::size_t> pointer = IndexPointer(
      kBig5EncodeCodes, kBig5EncodePointers, std::size(kBig5EncodeCodes), code_point);
  if (!pointer.has_value()) {
    return false;
  }
  const std::size_t trail = *pointer % 157;
  PushByte(out, *pointer / 157 + 0x81);
  PushByte(out, trail + (trail < 0x3F ? 0x40u : 0x62u));
  return true;
}

// GB18030's eighteen private-use code points that do not encode where the index says.
//
// They are the GB18030-2022 revision's compatibility hole: the revision moved those characters to
// real code points, and the bytes that used to mean them are still in deployed documents. Encoding
// them through the index would produce four-byte sequences no GBK server understands, so the
// standard carries this side table instead -- and it is *encoder only*, so it does not round-trip.
struct Gb18030EncoderException {
  std::uint32_t code_point;
  std::uint8_t lead;
  std::uint8_t trail;
};
constexpr Gb18030EncoderException kGb18030EncoderExceptions[] = {
    {0xE78D, 0xA6, 0xD9}, {0xE78E, 0xA6, 0xDA}, {0xE78F, 0xA6, 0xDB}, {0xE790, 0xA6, 0xDC},
    {0xE791, 0xA6, 0xDD}, {0xE792, 0xA6, 0xDE}, {0xE793, 0xA6, 0xDF}, {0xE794, 0xA6, 0xEC},
    {0xE795, 0xA6, 0xED}, {0xE796, 0xA6, 0xF3}, {0xE81E, 0xFE, 0x59}, {0xE826, 0xFE, 0x61},
    {0xE82B, 0xFE, 0x66}, {0xE82C, 0xFE, 0x67}, {0xE832, 0xFE, 0x6D}, {0xE843, 0xFE, 0x7E},
    {0xE854, 0xFE, 0x90}, {0xE864, 0xFE, 0xA0},
};

// The standard's "index gb18030 ranges pointer": the last range whose code point is at or below
// this one, plus the distance. A linear scan would be 207 comparisons per character; the table is
// ascending in both columns, so it is a binary search over one of them.
std::optional<std::size_t> Gb18030RangesPointer(std::uint32_t code_point) {
  if (code_point == 0xE7C7) {
    return 7457;  // the standard's one exception, from the GB18030-2005 revision
  }
  const auto* begin = std::begin(kGb18030RangeCodePoints);
  const auto* end = begin + std::size(kGb18030RangeCodePoints);
  const auto* above = std::upper_bound(begin, end, code_point);
  if (above == begin) {
    return std::nullopt;
  }
  const std::size_t at = static_cast<std::size_t>(above - begin) - 1;
  return kGb18030RangePointers[at] + (code_point - kGb18030RangeCodePoints[at]);
}

bool EncodeGb18030(std::uint32_t code_point, std::string& out, bool is_gbk) {
  if (IsAscii(code_point)) {
    PushByte(out, code_point);
    return true;
  }
  // The one code point GB18030 cannot encode at all: the index maps 0xA3 0xA0 to U+3000 for
  // compatibility with deployed content, so U+E5E5 has no bytes left and the index does not
  // round-trip. The standard says this explicitly, which is why it is an error rather than an
  // oversight.
  if (code_point == 0xE5E5) {
    return false;
  }
  if (is_gbk && code_point == 0x20AC) {
    PushByte(out, 0x80);
    return true;
  }
  for (const Gb18030EncoderException& exception : kGb18030EncoderExceptions) {
    if (exception.code_point == code_point) {
      PushByte(out, exception.lead);
      PushByte(out, exception.trail);
      return true;
    }
  }
  if (const std::optional<std::size_t> pointer = IndexPointer(
          kGb18030EncodeCodes, kGb18030EncodePointers, std::size(kGb18030EncodeCodes), code_point)) {
    const std::size_t trail = *pointer % 190;
    PushByte(out, *pointer / 190 + 0x81);
    PushByte(out, trail + (trail < 0x3F ? 0x40u : 0x41u));
    return true;
  }
  // **GBK stops here and GB18030 does not**, which is the only difference between the two encoders
  // and the reason they are two encodings rather than one with two labels.
  if (is_gbk) {
    return false;
  }
  const std::optional<std::size_t> pointer = Gb18030RangesPointer(code_point);
  if (!pointer.has_value()) {
    return false;
  }
  std::size_t rest = *pointer;
  const std::size_t byte1 = rest / (10 * 126 * 10);
  rest %= (10 * 126 * 10);
  const std::size_t byte2 = rest / (10 * 126);
  rest %= (10 * 126);
  const std::size_t byte3 = rest / 10;
  const std::size_t byte4 = rest % 10;
  PushByte(out, byte1 + 0x81);
  PushByte(out, byte2 + 0x30);
  PushByte(out, byte3 + 0x81);
  PushByte(out, byte4 + 0x30);
  return true;
}

// ISO-2022-JP, the stateful one. The state is which character set the bytes are currently being read
// as, and an escape sequence changes it: `ESC ( B` for ASCII, `ESC ( J` for Roman, `ESC $ B` for
// jis0208. The standard's encoder is written as a loop that *pushes the code point back* and emits
// an escape whenever the state has to change, which is exactly what the two `continue`s below do.
//
// U+000E, U+000F and U+001B return U+FFFD rather than themselves, and the standard says why in one
// word: attacks. Those three are shift-out, shift-in and escape -- a document that could put a raw
// 0x1B into an ISO-2022-JP stream could change what every byte after it decodes as, which turns
// text a filter approved into markup.
enum class Iso2022JpState : int { Ascii = 0, Roman = 1, Jis0208 = 2 };

bool EncodeIso2022Jp(std::uint32_t code_point, int& raw_state, std::string& out) {
  auto state = static_cast<Iso2022JpState>(raw_state);
  const auto set_state = [&](Iso2022JpState next) {
    state = next;
    raw_state = static_cast<int>(next);
  };
  for (;;) {
    if ((state == Iso2022JpState::Ascii || state == Iso2022JpState::Roman) &&
        (code_point == 0x000E || code_point == 0x000F || code_point == 0x001B)) {
      return false;
    }
    if (state == Iso2022JpState::Ascii && IsAscii(code_point)) {
      PushByte(out, code_point);
      return true;
    }
    if (state == Iso2022JpState::Roman &&
        ((IsAscii(code_point) && code_point != 0x5C && code_point != 0x7E) ||
         code_point == 0x00A5 || code_point == 0x203E)) {
      if (IsAscii(code_point)) {
        PushByte(out, code_point);
      } else if (code_point == 0x00A5) {
        PushByte(out, 0x5C);
      } else {
        PushByte(out, 0x7E);
      }
      return true;
    }
    if (IsAscii(code_point) && state != Iso2022JpState::Ascii) {
      out += "\x1B(B";
      set_state(Iso2022JpState::Ascii);
      continue;
    }
    if ((code_point == 0x00A5 || code_point == 0x203E) && state != Iso2022JpState::Roman) {
      out += "\x1B(J";
      set_state(Iso2022JpState::Roman);
      continue;
    }
    if (code_point == 0x2212) {
      code_point = 0xFF0D;
    }
    if (code_point >= 0xFF61 && code_point <= 0xFF9F) {
      // Halfwidth katakana have no jis0208 pointer, so the standard folds them to their fullwidth
      // forms first. This is the one encoder that changes the character rather than failing, and
      // the index exists because the fold is *not* Unicode's NFKC for two of them.
      code_point = kIso2022JpKatakana[code_point - 0xFF61];
    }
    const std::optional<std::size_t> pointer = IndexPointer(
        kJis0208EncodeCodes, kJis0208EncodePointers, std::size(kJis0208EncodeCodes), code_point);
    if (!pointer.has_value()) {
      if (state == Iso2022JpState::Jis0208) {
        // **The state is reset before the failure is reported**, and it must be: the caller writes
        // the unencodable character as ASCII (`&#1234;`), and ASCII inside a jis0208 run would be
        // read back as kanji.
        out += "\x1B(B";
        set_state(Iso2022JpState::Ascii);
      }
      return false;
    }
    if (state != Iso2022JpState::Jis0208) {
      out += "\x1B$B";
      set_state(Iso2022JpState::Jis0208);
      continue;
    }
    PushByte(out, *pointer / 94 + 0x21);
    PushByte(out, *pointer % 94 + 0x21);
    return true;
  }
}

}  // namespace

std::string DecodeMultiByte(std::string_view bytes, Encoding encoding) {
  switch (encoding) {
    case Encoding::ShiftJis:
      return DecodeShiftJis(bytes, nullptr);
    case Encoding::EucJp:
      return DecodeEucJp(bytes, nullptr);
    case Encoding::EucKr:
      return DecodeEucKr(bytes, nullptr);
    case Encoding::Big5:
      return DecodeBig5(bytes, nullptr);
    case Encoding::Gb18030:
    case Encoding::Gbk:
      return DecodeGb18030(bytes, nullptr);
    case Encoding::Iso2022Jp:
      return DecodeIso2022Jp(bytes);
    default:
      return std::string();
  }
}

bool DecodeMultiByteStreaming(std::string_view bytes, Encoding encoding, std::string& out,
                              std::string& leftover, bool stream, bool fatal) {
  bool failed = false;
  if (encoding == Encoding::Iso2022Jp) {
    // Leftover is decoder state; clearing it first would drop Roman after `ESC ( J`.
    out = DecodeIso2022Jp(bytes, &leftover, stream, fatal, &failed);
    return !failed;
  }
  leftover.clear();
  std::string* hold = stream ? &leftover : nullptr;
  switch (encoding) {
    case Encoding::ShiftJis:
      out = DecodeShiftJis(bytes, hold, fatal, &failed);
      return !failed;
    case Encoding::EucJp:
      out = DecodeEucJp(bytes, hold, fatal, &failed);
      return !failed;
    case Encoding::EucKr:
      out = DecodeEucKr(bytes, hold, fatal, &failed);
      return !failed;
    case Encoding::Big5:
      out = DecodeBig5(bytes, hold, fatal, &failed);
      return !failed;
    case Encoding::Gb18030:
    case Encoding::Gbk:
      out = DecodeGb18030(bytes, hold, fatal, &failed);
      return !failed;
    default:
      out.clear();
      return true;
  }
}

bool EncodeMultiByte(std::uint32_t code_point, Encoding encoding, int& state, std::string& out) {
  switch (encoding) {
    case Encoding::ShiftJis:
      return EncodeShiftJis(code_point, out);
    case Encoding::EucJp:
      return EncodeEucJp(code_point, out);
    case Encoding::EucKr:
      return EncodeEucKr(code_point, out);
    case Encoding::Big5:
      return EncodeBig5(code_point, out);
    case Encoding::Gb18030:
      return EncodeGb18030(code_point, out, false);
    case Encoding::Gbk:
      return EncodeGb18030(code_point, out, true);
    case Encoding::Iso2022Jp:
      return EncodeIso2022Jp(code_point, state, out);
    default:
      return false;
  }
}

void FinishMultiByte(Encoding encoding, int& state, std::string& out) {
  // Only ISO-2022-JP has anything to say at the end of a stream, and what it says is load-bearing:
  // a stream that ends in the jis0208 state decodes wrong when anything is concatenated after it.
  if (encoding != Encoding::Iso2022Jp || state == static_cast<int>(Iso2022JpState::Ascii)) {
    return;
  }
  out += "\x1B(B";
  state = static_cast<int>(Iso2022JpState::Ascii);
}

}  // namespace microbrowser::html

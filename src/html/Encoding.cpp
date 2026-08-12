#include "html/Encoding.h"

#include <algorithm>
#include <array>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The specification's prescan bound. A `<meta charset>` after it does not count -- in every browser --
// so matching the number is what makes this browser agree with the one a page was tested in.
constexpr std::size_t kPrescanBytes = 1024;

// windows-1252's difference from ISO-8859-1: the 0x80-0x9F range, which ISO-8859-1 leaves as control
// characters and which windows-1252 fills with punctuation. This table is *why* the two labels are the
// same decoder in the specification -- a page labelled `iso-8859-1` containing a 0x93 means a curly
// quote, because that is what the authoring tool that produced it meant, and rendering a control
// character there is rendering something no reader ever saw.
constexpr std::array<std::uint32_t, 32> kWindows1252Upper = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

// The high halves of the ISO-8859 parts this browser decodes. Only the 0xA0-0xFF range differs between
// them -- 0x00-0x9F is ASCII plus C1 controls in every part -- so each table is 96 entries rather than
// 256, which is also how the specification's index files are written.
constexpr std::array<std::uint32_t, 96> kIso8859_2 = {
    0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7, 0x00A8, 0x0160, 0x015E, 0x0164,
    0x0179, 0x00AD, 0x017D, 0x017B, 0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
    0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C, 0x0154, 0x00C1, 0x00C2, 0x0102,
    0x00C4, 0x0139, 0x0106, 0x00C7, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7, 0x0158, 0x016E, 0x00DA, 0x0170,
    0x00DC, 0x00DD, 0x0162, 0x00DF, 0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F, 0x0111, 0x0144, 0x0148, 0x00F3,
    0x00F4, 0x0151, 0x00F6, 0x00F7, 0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};
constexpr std::array<std::uint32_t, 96> kIso8859_5 = {
    0x00A0, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x0409, 0x040A, 0x040B,
    0x040C, 0x00AD, 0x040E, 0x040F, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423,
    0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B,
    0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F, 0x2116, 0x0451, 0x0452, 0x0453,
    0x0454, 0x0455, 0x0456, 0x0457, 0x0458, 0x0459, 0x045A, 0x045B, 0x045C, 0x00A7, 0x045E, 0x045F,
};
constexpr std::array<std::uint32_t, 96> kIso8859_7 = {
    0x00A0, 0x2018, 0x2019, 0x00A3, 0x20AC, 0x20AF, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x037A, 0x00AB,
    0x00AC, 0x00AD, 0xFFFD, 0x2015, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x0384, 0x0385, 0x0386, 0x00B7,
    0x0388, 0x0389, 0x038A, 0x00BB, 0x038C, 0x00BD, 0x038E, 0x038F, 0x0390, 0x0391, 0x0392, 0x0393,
    0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F,
    0x03A0, 0x03A1, 0xFFFD, 0x03A3, 0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9, 0x03AA, 0x03AB,
    0x03AC, 0x03AD, 0x03AE, 0x03AF, 0x03B0, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7,
    0x03B8, 0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0, 0x03C1, 0x03C2, 0x03C3,
    0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x03CA, 0x03CB, 0x03CC, 0x03CD, 0x03CE, 0xFFFD,
};
constexpr std::array<std::uint32_t, 96> kIso8859_9 = {
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB,
    0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3,
    0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x011E, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB,
    0x00DC, 0x0130, 0x015E, 0x00DF, 0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x011F, 0x00F1, 0x00F2, 0x00F3,
    0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x0131, 0x015F, 0x00FF,
};
constexpr std::array<std::uint32_t, 96> kIso8859_15 = {
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x20AC, 0x00A5, 0x0160, 0x00A7, 0x0161, 0x00A9, 0x00AA, 0x00AB,
    0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x017D, 0x00B5, 0x00B6, 0x00B7,
    0x017E, 0x00B9, 0x00BA, 0x00BB, 0x0152, 0x0153, 0x0178, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3,
    0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB,
    0x00DC, 0x00DD, 0x00DE, 0x00DF, 0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0, 0x00F1, 0x00F2, 0x00F3,
    0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

const std::array<std::uint32_t, 96>* SingleByteTable(Encoding encoding) {
  switch (encoding) {
    case Encoding::Iso8859_2:
      return &kIso8859_2;
    case Encoding::Iso8859_5:
      return &kIso8859_5;
    case Encoding::Iso8859_7:
      return &kIso8859_7;
    case Encoding::Iso8859_9:
      return &kIso8859_9;
    case Encoding::Iso8859_15:
      return &kIso8859_15;
    default:
      return nullptr;
  }
}

// The single-byte encoders, which are the tables above searched backwards.
//
// A linear scan over 96 entries rather than a generated reverse table, and that is a measurement
// rather than laziness: a single-byte encoding is only ever asked to encode a *URL query* or a form
// field, both of which are tens of characters, and 96 comparisons of a `uint32_t` is under the cost
// of the branch that would pick a table. The multi-byte encoders are the ones with 24,000 entries
// and they are binary searches over generated tables for exactly that reason.
bool EncodeSingleByte(std::uint32_t code_point, Encoding encoding, std::string& out) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
    return true;
  }
  if (const std::array<std::uint32_t, 96>* table = SingleByteTable(encoding)) {
    // 0x80-0x9F is the C1 control range, identical in every ISO-8859 part, and the table starts at
    // 0xA0. A code point in that range encodes to itself; anything else has to be in the table.
    if (code_point >= 0x80 && code_point <= 0x9F) {
      out.push_back(static_cast<char>(code_point));
      return true;
    }
    for (std::size_t i = 0; i < table->size(); ++i) {
      if ((*table)[i] == code_point && code_point != 0xFFFD) {
        out.push_back(static_cast<char>(0xA0u + i));
        return true;
      }
    }
    return false;
  }
  // windows-1252, and ISO-8859-1 with it -- the specification maps the second label to the first
  // decoder, and an encoder that disagreed would be the same confusion in the other direction.
  for (std::size_t i = 0; i < kWindows1252Upper.size(); ++i) {
    if (kWindows1252Upper[i] == code_point && code_point != 0xFFFD) {
      out.push_back(static_cast<char>(0x80u + i));
      return true;
    }
  }
  if (code_point >= 0xA0 && code_point <= 0xFF) {
    out.push_back(static_cast<char>(code_point));
    return true;
  }
  return false;
}

// `charset=` out of a `Content-Type`, honouring quotes. `text/html; charset="utf-8"` is in the wild
// and a parser that kept the quote would look up a label that does not exist -- and fall through to
// windows-1252 on a page that said UTF-8, which is the confusion in miniature.
std::optional<std::string_view> CharsetFromContentType(std::string_view header) {
  const std::string lowered = util::AsciiLowerCase(std::string(header));
  const std::size_t at = lowered.find("charset");
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::size_t equals = lowered.find('=', at);
  if (equals == std::string::npos) {
    return std::nullopt;
  }
  ++equals;
  while (equals < header.size() && (header[equals] == ' ' || header[equals] == '\t')) {
    ++equals;
  }
  if (equals >= header.size()) {
    return std::nullopt;
  }
  if (header[equals] == '"' || header[equals] == '\'') {
    const char quote = header[equals];
    const std::size_t end = header.find(quote, equals + 1);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return header.substr(equals + 1, end - equals - 1);
  }
  std::size_t end = equals;
  // `>` is in this list because the same function reads a `<meta>` tag during the prescan, where the
  // value is terminated by the tag rather than by a semicolon. Without it the label from
  // `<meta charset=utf-8>` is `utf-8>`, which resolves to nothing -- and a page that declared UTF-8
  // gets decoded as windows-1252, which is the confusion this file exists to prevent.
  while (end < header.size() && header[end] != ';' && header[end] != ' ' && header[end] != '\t' &&
         header[end] != '>' && header[end] != '"' && header[end] != '\'') {
    ++end;
  }
  return header.substr(equals, end - equals);
}

// The prescan: `<meta charset=...>` or `<meta http-equiv="content-type" content="...">` in the first
// 1024 bytes.
//
// Deliberately a *scan for the attribute* rather than a parse, and the specification agrees: this runs
// before there is a tokenizer, because the tokenizer's input is what it decides. A page's `<meta>`
// inside a comment is therefore honoured, which is what other browsers do -- matching them matters
// more than being clever, because a page is tested against them.
std::optional<Encoding> PrescanForMeta(std::string_view bytes) {
  const std::string lowered =
      util::AsciiLowerCase(std::string(bytes.substr(0, std::min(bytes.size(), kPrescanBytes))));
  std::size_t at = 0;
  while ((at = lowered.find("<meta", at)) != std::string::npos) {
    // A `<meta` with no `>` after it, which is what a document truncated mid-tag looks like -- and
    // the prescan runs on the *first 1024 bytes*, so a tag straddling that boundary is truncated by
    // construction rather than by accident. `find` answers npos here, and `npos + 1` is 0: the first
    // version of this line computed `end - at` from that and read gigabytes off the end of the
    // buffer. Found by the fuzzer on its first run, from `<html><meta ch`.
    const std::size_t close = lowered.find('>', at);
    const std::size_t end = close == std::string::npos ? lowered.size() : close + 1;
    const std::string_view tag(lowered.data() + at, end - at);
    // `charset=` directly on the meta, which is the modern spelling.
    if (const std::size_t charset = tag.find("charset"); charset != std::string_view::npos) {
      if (const std::optional<std::string_view> label = CharsetFromContentType(tag)) {
        if (const std::optional<Encoding> found = EncodingFromLabel(*label)) {
          return found;
        }
      }
    }
    // Past this tag, and never backwards: `end` is at least `at + 1` because `at` points at a `<`,
    // so this cannot loop.
    at = end > at ? end : at + 1;
  }
  return std::nullopt;
}

void AppendUtf8(std::string& out, std::uint32_t code) {
  util::AppendUtf8(out, code);
}

// The replacement character, appended once per *maximal subpart* of an ill-formed sequence. That
// phrase is the whole of the substitution rule and it is why this is not "skip a byte and continue":
// `\xE0\x80\x41` is one replacement followed by `A`, not two replacements and not a swallowed `A`.
void AppendReplacement(std::string& out) {
  out += "\xEF\xBF\xBD";
  AddPerformanceCounter(PerfCounterId::EncodingReplacements);
}

std::string DecodeUtf8Strictly(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  std::size_t at = 0;
  while (at < bytes.size()) {
    const std::uint8_t lead = static_cast<std::uint8_t>(bytes[at]);
    if (lead < 0x80) {
      out.push_back(static_cast<char>(lead));
      ++at;
      continue;
    }
    // How many continuation bytes this lead promises, and what range the result must land in. The
    // *lower* bound is what rejects an overlong encoding -- `\xC0\x80` is a two-byte spelling of NUL,
    // and accepting it is how a filter that looked for a literal `\0` is bypassed.
    // The **second byte's valid range depends on the lead**, and that is not a refinement -- it is
    // what decides how many U+FFFDs an ill-formed run produces. `ED A0 80` is three replacements
    // rather than one, because `A0` is outside `ED`'s range (80-9F), so the maximal subpart ends
    // after `ED` and the two bytes after it are strays. A decoder that only checked the *resulting*
    // code point would consume all three and emit one replacement -- a different document.
    int continuations = 0;
    std::uint32_t code = 0;
    std::uint8_t second_low = 0x80;
    std::uint8_t second_high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      continuations = 1;
      code = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      continuations = 2;
      code = lead & 0x0Fu;
      if (lead == 0xE0) {
        second_low = 0xA0;  // below this is an overlong three-byte form
      } else if (lead == 0xED) {
        second_high = 0x9F;  // above this is a surrogate
      }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      continuations = 3;
      code = lead & 0x07u;
      if (lead == 0xF0) {
        second_low = 0x90;  // below this is an overlong four-byte form
      } else if (lead == 0xF4) {
        second_high = 0x8F;  // above this is beyond U+10FFFF
      }
    } else {
      // 0x80-0xC1 and 0xF5-0xFF are never a lead: a stray continuation, an overlong two-byte form, or
      // a lead for a code point above U+10FFFF.
      AppendReplacement(out);
      ++at;
      continue;
    }
    std::size_t taken = 1;
    bool ok = true;
    for (int i = 0; i < continuations; ++i) {
      if (at + taken >= bytes.size()) {
        ok = false;
        break;
      }
      const std::uint8_t next = static_cast<std::uint8_t>(bytes[at + taken]);
      const std::uint8_t low = i == 0 ? second_low : 0x80;
      const std::uint8_t high = i == 0 ? second_high : 0xBF;
      if (next < low || next > high) {
        // Outside the range this lead allows. **The sequence ends here and this byte is not
        // consumed** -- it is the start of whatever comes next, and consuming it is how an `A` after
        // a bad sequence disappears.
        ok = false;
        break;
      }
      code = (code << 6) | (next & 0x3Fu);
      ++taken;
    }
    if (!ok) {
      // One replacement for the whole maximal subpart, and the bytes it *did* cover are consumed --
      // no more. Advancing by one instead would emit a second replacement for a byte that was
      // already part of this one, and advancing past the byte that ended it would delete a
      // character.
      AppendReplacement(out);
      at += taken;
      continue;
    }
    AppendUtf8(out, code);
    at += taken;
  }
  return out;
}

std::string DecodeSingleByte(std::string_view bytes, Encoding encoding) {
  const std::array<std::uint32_t, 96>* table = SingleByteTable(encoding);
  std::string out;
  out.reserve(bytes.size());
  for (const char byte : bytes) {
    const std::uint8_t value = static_cast<std::uint8_t>(byte);
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
      continue;
    }
    if (table != nullptr) {
      if (value < 0xA0) {
        // C1 controls in every ISO-8859 part. Passed through as the code point they are rather than
        // replaced: they are legal characters, and a decoder that replaced them would corrupt text.
        AppendUtf8(out, value);
      } else {
        AppendUtf8(out, (*table)[value - 0xA0u]);
      }
      continue;
    }
    // windows-1252, which is also what `iso-8859-1` and `latin1` decode as -- the specification maps
    // those labels to this decoder, because a page labelled ISO-8859-1 with a 0x93 in it means a curly
    // quote and that is what its reader saw.
    if (value < 0xA0) {
      AppendUtf8(out, kWindows1252Upper[value - 0x80u]);
    } else {
      AppendUtf8(out, value);
    }
  }
  return out;
}

std::string DecodeUtf16(std::string_view bytes, bool little_endian) {
  std::string out;
  std::size_t at = 0;
  while (at + 1 < bytes.size()) {
    const std::uint8_t first = static_cast<std::uint8_t>(bytes[at]);
    const std::uint8_t second = static_cast<std::uint8_t>(bytes[at + 1]);
    const std::uint32_t unit = little_endian ? static_cast<std::uint32_t>(first | (second << 8))
                                            : static_cast<std::uint32_t>((first << 8) | second);
    at += 2;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      // A high surrogate needs its pair. An unpaired one is U+FFFD -- not passed through -- because a
      // lone surrogate is not a character and encoding one as UTF-8 produces bytes no decoder accepts.
      if (at + 1 >= bytes.size()) {
        AppendReplacement(out);
        break;
      }
      const std::uint8_t third = static_cast<std::uint8_t>(bytes[at]);
      const std::uint8_t fourth = static_cast<std::uint8_t>(bytes[at + 1]);
      const std::uint32_t low = little_endian ? static_cast<std::uint32_t>(third | (fourth << 8))
                                             : static_cast<std::uint32_t>((third << 8) | fourth);
      if (low < 0xDC00 || low > 0xDFFF) {
        AppendReplacement(out);
        continue;  // the second unit is not consumed: it may be a valid character of its own
      }
      at += 2;
      AppendUtf8(out, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
      continue;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      AppendReplacement(out);  // a low surrogate with nothing before it
      continue;
    }
    AppendUtf8(out, unit);
  }
  if (at < bytes.size()) {
    AppendReplacement(out);  // an odd trailing byte
  }
  return out;
}

}  // namespace

std::optional<Encoding> EncodingFromLabel(std::string_view label) {
  const std::string lowered = util::AsciiLowerCase(std::string(util::TrimAscii(label)));
  // The labels the Encoding Standard lists for each of these, which is more than the canonical name:
  // a page writes `utf8`, `UTF_8`, `cp1252` or `latin1`, and every one of those is in the index.
  if (lowered == "utf-8" || lowered == "utf8" || lowered == "unicode-1-1-utf-8" ||
      lowered == "utf_8") {
    return Encoding::Utf8;
  }
  if (lowered == "windows-1252" || lowered == "cp1252" || lowered == "x-cp1252" ||
      lowered == "ansi_x3.4-1968" || lowered == "ascii" || lowered == "us-ascii" ||
      lowered == "iso-8859-1" || lowered == "iso8859-1" || lowered == "latin1" ||
      lowered == "l1" || lowered == "cp819") {
    // **All of these are one decoder**, including `ascii`: the specification maps ASCII's label to
    // windows-1252 because a document that claims ASCII and contains a high byte is a document whose
    // author meant windows-1252, and replacing those bytes would corrupt text that renders elsewhere.
    return Encoding::Windows1252;
  }
  if (lowered == "iso-8859-2" || lowered == "iso8859-2" || lowered == "latin2" ||
      lowered == "l2") {
    return Encoding::Iso8859_2;
  }
  if (lowered == "iso-8859-5" || lowered == "iso8859-5" || lowered == "cyrillic") {
    return Encoding::Iso8859_5;
  }
  if (lowered == "iso-8859-7" || lowered == "iso8859-7" || lowered == "greek" ||
      lowered == "greek8") {
    return Encoding::Iso8859_7;
  }
  if (lowered == "iso-8859-9" || lowered == "iso8859-9" || lowered == "latin5" ||
      lowered == "windows-1254") {
    return Encoding::Iso8859_9;
  }
  if (lowered == "iso-8859-15" || lowered == "iso8859-15" || lowered == "latin9" ||
      lowered == "csisolatin9") {
    return Encoding::Iso8859_15;
  }
  if (lowered == "utf-16" || lowered == "utf-16le" || lowered == "utf16" || lowered == "utf16le") {
    // A bare `utf-16` label means *little endian* in the Encoding Standard. That looks arbitrary and
    // is not: it is what the installed base emits, and guessing big-endian instead produces text with
    // a NUL between every character.
    return Encoding::Utf16Le;
  }
  if (lowered == "utf-16be" || lowered == "utf16be") {
    return Encoding::Utf16Be;
  }
  // The legacy multi-byte labels, every spelling the Encoding Standard's index lists. `shift-jis` with
  // a hyphen, `sjis`, `ms_kanji` and `csshiftjis` are all in real documents, and a page whose label is
  // unrecognised falls through to windows-1252 -- which for Japanese is mojibake rather than text.
  if (lowered == "shift_jis" || lowered == "shift-jis" || lowered == "sjis" ||
      lowered == "ms_kanji" || lowered == "ms932" || lowered == "csshiftjis" ||
      lowered == "windows-31j" || lowered == "x-sjis") {
    return Encoding::ShiftJis;
  }
  if (lowered == "euc-jp" || lowered == "eucjp" || lowered == "x-euc-jp" ||
      lowered == "cseucpkdfmtjapanese") {
    return Encoding::EucJp;
  }
  if (lowered == "iso-2022-jp" || lowered == "csiso2022jp") {
    return Encoding::Iso2022Jp;
  }
  if (lowered == "euc-kr" || lowered == "euckr" || lowered == "windows-949" ||
      lowered == "ks_c_5601-1987" || lowered == "ks_c_5601-1989" || lowered == "ksc5601" ||
      lowered == "ksc_5601" || lowered == "iso-ir-149" || lowered == "csksc56011987" ||
      lowered == "korean" || lowered == "cseuckr") {
    return Encoding::EucKr;
  }
  if (lowered == "big5" || lowered == "big5-hkscs" || lowered == "cn-big5" ||
      lowered == "csbig5" || lowered == "x-x-big5") {
    return Encoding::Big5;
  }
  if (lowered == "gb18030") {
    return Encoding::Gb18030;
  }
  if (lowered == "gbk" || lowered == "gb2312" || lowered == "gb_2312" ||
      lowered == "gb_2312-80" || lowered == "chinese" || lowered == "csgb2312" ||
      lowered == "csiso58gb231280" || lowered == "iso-ir-58" || lowered == "x-gbk") {
    // **GBK is not GB18030 and these labels are not that one.** They share a decoder -- GB18030 is a
    // superset, so decoding a GBK document with it produces the same characters -- and they do not
    // share an encoder: GBK refuses everything the two-byte form cannot reach, where GB18030 emits
    // four bytes. A page labelled `gbk` whose form sent four-byte sequences would be sending bytes
    // its own server has no decoder for.
    return Encoding::Gbk;
  }
  // Everything else is *nothing*, so the caller falls through to the next step of the algorithm rather
  // than to UTF-8. What is left in that category is small now -- the EBCDIC labels, and the
  // `replacement` encoding the standard defines for labels that are dangerous to honour.
  return std::nullopt;
}

std::string_view EncodingName(Encoding encoding) {
  switch (encoding) {
    case Encoding::Utf8:
      return "UTF-8";
    case Encoding::Windows1252:
    case Encoding::Latin1:
      return "windows-1252";
    case Encoding::Iso8859_2:
      return "ISO-8859-2";
    case Encoding::Iso8859_5:
      return "ISO-8859-5";
    case Encoding::Iso8859_7:
      return "ISO-8859-7";
    case Encoding::Iso8859_9:
      return "windows-1254";
    case Encoding::Iso8859_15:
      return "ISO-8859-15";
    case Encoding::Utf16Le:
      return "UTF-16LE";
    case Encoding::Utf16Be:
      return "UTF-16BE";
    case Encoding::ShiftJis:
      return "Shift_JIS";
    case Encoding::EucJp:
      return "EUC-JP";
    case Encoding::EucKr:
      return "EUC-KR";
    case Encoding::Big5:
      return "Big5";
    case Encoding::Gb18030:
      return "gb18030";
    case Encoding::Gbk:
      return "GBK";
    case Encoding::Iso2022Jp:
      return "ISO-2022-JP";
  }
  return "UTF-8";
}

std::size_t BomLength(std::string_view bytes) {
  if (bytes.size() >= 3 && static_cast<std::uint8_t>(bytes[0]) == 0xEF &&
      static_cast<std::uint8_t>(bytes[1]) == 0xBB && static_cast<std::uint8_t>(bytes[2]) == 0xBF) {
    return 3;
  }
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFF &&
      static_cast<std::uint8_t>(bytes[1]) == 0xFE) {
    return 2;
  }
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFE &&
      static_cast<std::uint8_t>(bytes[1]) == 0xFF) {
    return 2;
  }
  return 0;
}

Encoding SniffEncoding(std::string_view bytes, std::string_view content_type) {
  // 1. The BOM, which wins over everything -- including a contradictory `charset`. The BOM is *in the
  // bytes*; a header is a claim about them, and when the two disagree the bytes are the evidence.
  if (bytes.size() >= 3 && static_cast<std::uint8_t>(bytes[0]) == 0xEF &&
      static_cast<std::uint8_t>(bytes[1]) == 0xBB && static_cast<std::uint8_t>(bytes[2]) == 0xBF) {
    return Encoding::Utf8;
  }
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFF &&
      static_cast<std::uint8_t>(bytes[1]) == 0xFE) {
    return Encoding::Utf16Le;
  }
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFE &&
      static_cast<std::uint8_t>(bytes[1]) == 0xFF) {
    return Encoding::Utf16Be;
  }
  // 2. `Content-Type`.
  if (!content_type.empty()) {
    if (const std::optional<std::string_view> label = CharsetFromContentType(content_type)) {
      if (const std::optional<Encoding> found = EncodingFromLabel(*label)) {
        return *found;
      }
    }
  }
  // 3. The prescan.
  if (const std::optional<Encoding> found = PrescanForMeta(bytes)) {
    AddPerformanceCounter(PerfCounterId::EncodingFromPrescan);
    return *found;
  }
  // 4. windows-1252, and not UTF-8. A page with no declaration is overwhelmingly old, and decoding it
  // as UTF-8 turns every high byte into U+FFFD where windows-1252 renders what its author saw.
  AddPerformanceCounter(PerfCounterId::EncodingFellBackToWindows1252);
  return Encoding::Windows1252;
}

std::string DecodeToUtf8(std::string_view bytes, Encoding encoding) {
  // The BOM is not text. One that reached the tokenizer would be a zero-width character at the start
  // of the document -- invisible, and it shifts every offset a parse error reports.
  const std::size_t bom = BomLength(bytes);
  const std::string_view body = bytes.substr(std::min(bom, bytes.size()));
  switch (encoding) {
    case Encoding::Utf8:
      return DecodeUtf8Strictly(body);
    case Encoding::Utf16Le:
      return DecodeUtf16(body, true);
    case Encoding::Utf16Be:
      return DecodeUtf16(body, false);
    case Encoding::ShiftJis:
    case Encoding::EucJp:
    case Encoding::EucKr:
    case Encoding::Big5:
    case Encoding::Gb18030:
    case Encoding::Gbk:
    case Encoding::Iso2022Jp:
      return DecodeMultiByte(body, encoding);
    default:
      return DecodeSingleByte(body, encoding);
  }
}

bool Encoder::Encode(std::uint32_t code_point, std::string& out) {
  // A surrogate is not a scalar value. It arrives here because a page's string is UTF-16 code units
  // and may hold a lone one, and it is a failure rather than an assertion for that reason -- the
  // caller writes it as `&#55296;`, which is what every browser sends.
  if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    return false;
  }
  switch (encoding_) {
    case Encoding::Utf8:
      // The one encoding that cannot fail: every scalar value has a UTF-8 form.
      util::AppendUtf8(out, code_point);
      return true;
    case Encoding::Utf16Le:
    case Encoding::Utf16Be:
      // **Deliberately not implemented, and not "not supported".** The Encoding Standard has no
      // UTF-16 encoder at all: every place a document's encoding is used to *produce* bytes -- a
      // form body, a URL query -- replaces UTF-16 with UTF-8 first, because a UTF-16 form body would
      // contain NUL bytes that nothing downstream survives. Reaching here means a caller skipped
      // that replacement, so failing is the honest answer.
      return false;
    case Encoding::ShiftJis:
    case Encoding::EucJp:
    case Encoding::EucKr:
    case Encoding::Big5:
    case Encoding::Gb18030:
    case Encoding::Gbk:
    case Encoding::Iso2022Jp:
      return EncodeMultiByte(code_point, encoding_, state_, out);
    default:
      return EncodeSingleByte(code_point, encoding_, out);
  }
}

void Encoder::Finish(std::string& out) { FinishMultiByte(encoding_, state_, out); }

std::uint32_t NextScalarValue(std::string_view input, std::size_t& at) {
  std::uint32_t code_point = 0;
  if (!util::DecodeUtf8(input, at, code_point)) {
    // Ill-formed bytes are U+FFFD and cost exactly one byte -- never passed through, because a raw
    // byte reaching an encoder's output is the shortcut this whole file exists to refuse.
    ++at;
    return 0xFFFD;
  }
  if (code_point < 0xD800 || code_point > 0xDFFF) {
    return code_point;
  }
  if (code_point <= 0xDBFF) {
    std::size_t after = at;
    std::uint32_t low = 0;
    if (util::DecodeUtf8(input, after, low) && low >= 0xDC00 && low <= 0xDFFF) {
      at = after;
      return 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
    }
  }
  // A surrogate with no partner is not a character. U+FFFD is what the IDL conversion to a scalar
  // value string produces, and it matters that it is *not* the surrogate: an encoder handed one
  // would report it unencodable and the caller would write `&#55357;` into a URL, which is a
  // character reference for something that cannot exist.
  return 0xFFFD;
}

std::string EncodeWithNumericEscapes(std::string_view input, Encoding encoding) {
  Encoder encoder(encoding);
  std::string out;
  out.reserve(input.size());
  std::size_t at = 0;
  while (at < input.size()) {
    const std::uint32_t code_point = NextScalarValue(input, at);
    if (!encoder.Encode(code_point, out)) {
      out += "&#";
      out += std::to_string(code_point);
      out += ';';
    }
  }
  encoder.Finish(out);
  return out;
}

}  // namespace microbrowser::html

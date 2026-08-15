#include "html/Encoding.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The specification's prescan bound. A `<meta charset>` after it does not count -- in every browser --
// so matching the number is what makes this browser agree with the one a page was tested in.
constexpr std::size_t kPrescanBytes = 1024;

#include "html/SingleByteIndexes.inc"

constexpr std::uint16_t kSingleByteHole = 0xFFFF;

void AppendUtf8(std::string& out, std::uint32_t code);
void AppendReplacement(std::string& out);

// The single-byte decoder: ASCII below 0x80, then pointer = byte − 0x80 into a 128-entry index.
// A hole is U+FFFD, or a failure when `fatal` is set -- TextDecoder's flag, and the reason this
// returns bool rather than a string.
bool DecodeSingleByte(std::string_view bytes, Encoding encoding, std::string& out, bool fatal) {
  if (encoding == Encoding::Replacement) {
    // Encoding Standard: empty in, empty out; any non-empty input is *one*
    // U+FFFD, not one per byte. TextDecoder refuses this encoding; documents
    // and XHR still reach it through a label the sniffer accepted.
    if (bytes.empty()) {
      return true;
    }
    if (fatal) {
      return false;
    }
    AppendReplacement(out);
    return true;
  }
  if (encoding == Encoding::XUserDefined) {
    out.reserve(out.size() + bytes.size());
    for (const char byte : bytes) {
      const std::uint8_t value = static_cast<std::uint8_t>(byte);
      if (value < 0x80) {
        out.push_back(static_cast<char>(value));
      } else {
        AppendUtf8(out, 0xF780u + (value - 0x80u));
      }
    }
    return true;
  }
  const std::uint16_t* table = SingleByteIndex(encoding);
  if (table == nullptr) {
    return false;
  }
  out.reserve(out.size() + bytes.size());
  for (const char byte : bytes) {
    const std::uint8_t value = static_cast<std::uint8_t>(byte);
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
      continue;
    }
    const std::uint16_t code = table[value - 0x80u];
    if (code == kSingleByteHole) {
      if (fatal) {
        return false;
      }
      AppendReplacement(out);
      continue;
    }
    AppendUtf8(out, code);
  }
  return true;
}

// The single-byte encoders, which are the tables above searched backwards.
//
// A linear scan over 128 entries rather than a generated reverse table, and that is a measurement
// rather than laziness: a single-byte encoding is only ever asked to encode a *URL query* or a form
// field, both of which are tens of characters, and 128 comparisons of a `uint16_t` is under the cost
// of the branch that would pick a table. The multi-byte encoders are the ones with 24,000 entries
// and they are binary searches over generated tables for exactly that reason.
bool EncodeSingleByte(std::uint32_t code_point, Encoding encoding, std::string& out) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
    return true;
  }
  if (encoding == Encoding::XUserDefined) {
    if (code_point >= 0xF780u && code_point <= 0xF7FFu) {
      out.push_back(static_cast<char>(0x80u + (code_point - 0xF780u)));
      return true;
    }
    return false;
  }
  if (encoding == Encoding::Replacement) {
    return false;
  }
  const std::uint16_t* table = SingleByteIndex(encoding);
  if (table == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < 128; ++i) {
    if (table[i] == code_point && code_point != kSingleByteHole) {
      out.push_back(static_cast<char>(0x80u + i));
      return true;
    }
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
          // HTML's prescan, not the Encoding Standard's get-an-encoding: a `<meta charset=utf-16>`
          // cannot describe a UTF-16 document (the tag itself is ASCII), so the spec returns UTF-8.
          // `x-user-defined` is similarly rewritten to windows-1252. TextDecoder still honours both
          // labels; only a document's meta goes through this.
          if (*found == Encoding::Utf16Le || *found == Encoding::Utf16Be) {
            return Encoding::Utf8;
          }
          if (*found == Encoding::XUserDefined) {
            return Encoding::Windows1252;
          }
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

// UTF-8 decode with TextDecoder's leftover. `stream` holds a valid incomplete prefix instead of
// emitting U+FFFD for it; `fatal` fails instead of substituting. The document decoder is this
// with both flags false.
bool DecodeUtf8(std::string_view bytes, std::string& out, std::string& leftover, bool stream,
                bool fatal) {
  leftover.clear();
  out.reserve(out.size() + bytes.size());
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
      // a lead for a code point above U+10FFFF. Not a prefix to hold -- emit now, even when streaming.
      if (fatal) {
        return false;
      }
      AppendReplacement(out);
      ++at;
      continue;
    }
    std::size_t taken = 1;
    bool ok = true;
    bool incomplete = false;
    for (int i = 0; i < continuations; ++i) {
      if (at + taken >= bytes.size()) {
        ok = false;
        incomplete = true;
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
      if (incomplete && stream) {
        leftover = std::string(bytes.substr(at));
        return true;
      }
      // One replacement for the whole maximal subpart, and the bytes it *did* cover are consumed --
      // no more. Advancing by one instead would emit a second replacement for a byte that was
      // already part of this one, and advancing past the byte that ended it would delete a
      // character.
      if (fatal) {
        return false;
      }
      AppendReplacement(out);
      at += taken;
      continue;
    }
    AppendUtf8(out, code);
    at += taken;
  }
  return true;
}

bool DecodeUtf16(std::string_view bytes, bool little_endian, std::string& out, std::string& leftover,
                 bool stream, bool fatal) {
  leftover.clear();
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
        if (stream) {
          leftover = std::string(bytes.substr(at - 2));
          return true;
        }
        if (fatal) {
          return false;
        }
        // Encoding Standard: end-of-queue with a leading surrogate, a leading
        // byte, or both is *one* error. A high plus an odd trailing byte is
        // U+FFFD, not two. Consume the leftover byte so the odd-length check
        // below does not emit a second replacement.
        AppendReplacement(out);
        at = bytes.size();
        break;
      }
      const std::uint8_t third = static_cast<std::uint8_t>(bytes[at]);
      const std::uint8_t fourth = static_cast<std::uint8_t>(bytes[at + 1]);
      const std::uint32_t low = little_endian ? static_cast<std::uint32_t>(third | (fourth << 8))
                                             : static_cast<std::uint32_t>((third << 8) | fourth);
      if (low < 0xDC00 || low > 0xDFFF) {
        if (fatal) {
          return false;
        }
        AppendReplacement(out);
        continue;  // the second unit is not consumed: it may be a valid character of its own
      }
      at += 2;
      AppendUtf8(out, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
      continue;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      if (fatal) {
        return false;
      }
      AppendReplacement(out);  // a low surrogate with nothing before it
      continue;
    }
    AppendUtf8(out, unit);
  }
  if (at < bytes.size()) {
    if (stream) {
      leftover = std::string(bytes.substr(at));
      return true;
    }
    if (fatal) {
      return false;
    }
    AppendReplacement(out);  // an odd trailing byte
  }
  return true;
}

}  // namespace

std::optional<Encoding> EncodingFromLabel(std::string_view label) {
  // HTML's ASCII whitespace, not C's: `\vwindows-1252` is an invalid label, and TrimAscii would
  // accept it because `\v` is isspace. The Encoding Standard names the five characters.
  const std::string lowered = util::AsciiLowerCase(std::string(util::TrimHtmlWhitespace(label)));
  const auto* begin = std::begin(kEncodingLabels);
  const auto* end = std::end(kEncodingLabels);
  const auto* found =
      std::lower_bound(begin, end, lowered, [](const EncodingLabel& entry, const std::string& key) {
        return std::string_view(entry.label) < key;
      });
  if (found != end && found->label == lowered) {
    return found->encoding;
  }
  return std::nullopt;
}

std::optional<Encoding> EncodingFromMimeType(std::string_view content_type) {
  if (const std::optional<std::string_view> label = CharsetFromContentType(content_type)) {
    return EncodingFromLabel(*label);
  }
  return std::nullopt;
}

std::string_view EncodingName(Encoding encoding) {
  switch (encoding) {
    case Encoding::Utf8:
      return "UTF-8";
    case Encoding::Windows1252:
    case Encoding::Latin1:
      return "windows-1252";
    case Encoding::Ibm866:
      return "IBM866";
    case Encoding::Iso8859_2:
      return "ISO-8859-2";
    case Encoding::Iso8859_3:
      return "ISO-8859-3";
    case Encoding::Iso8859_4:
      return "ISO-8859-4";
    case Encoding::Iso8859_5:
      return "ISO-8859-5";
    case Encoding::Iso8859_6:
      return "ISO-8859-6";
    case Encoding::Iso8859_7:
      return "ISO-8859-7";
    case Encoding::Iso8859_8:
      return "ISO-8859-8";
    case Encoding::Iso8859_8I:
      return "ISO-8859-8-I";
    case Encoding::Iso8859_9:
      return "windows-1254";
    case Encoding::Iso8859_10:
      return "ISO-8859-10";
    case Encoding::Iso8859_13:
      return "ISO-8859-13";
    case Encoding::Iso8859_14:
      return "ISO-8859-14";
    case Encoding::Iso8859_15:
      return "ISO-8859-15";
    case Encoding::Iso8859_16:
      return "ISO-8859-16";
    case Encoding::Koi8R:
      return "KOI8-R";
    case Encoding::Koi8U:
      return "KOI8-U";
    case Encoding::Macintosh:
      return "macintosh";
    case Encoding::Windows874:
      return "windows-874";
    case Encoding::Windows1250:
      return "windows-1250";
    case Encoding::Windows1251:
      return "windows-1251";
    case Encoding::Windows1253:
      return "windows-1253";
    case Encoding::Windows1255:
      return "windows-1255";
    case Encoding::Windows1256:
      return "windows-1256";
    case Encoding::Windows1257:
      return "windows-1257";
    case Encoding::Windows1258:
      return "windows-1258";
    case Encoding::XMacCyrillic:
      return "x-mac-cyrillic";
    case Encoding::XUserDefined:
      return "x-user-defined";
    case Encoding::Replacement:
      return "replacement";
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

namespace {

// Whether the response says this is XML. The `+xml` suffix is the general rule and the three
// explicit types are the ones that predate it; between them they are what the MIME Sniffing
// Standard calls an XML MIME type.
bool IsXmlContentType(std::string_view content_type) {
  const std::string essence =
      util::AsciiLowerCase(util::TrimAscii(content_type.substr(0, content_type.find(';'))));
  return essence == "text/xml" || essence == "application/xml" ||
         (essence.size() > 4 && essence.compare(essence.size() - 4, 4, "+xml") == 0);
}

// `<?xml version="1.0" encoding="..."?>`, read from the first declaration only.
//
// Deliberately narrow: the declaration must be the very first thing in the document, which is what
// XML requires, so there is no scanning and nothing to bound. Anything else -- a label this build
// does not know, a missing `encoding` -- falls through to the XML default rather than to HTML's.
std::optional<Encoding> EncodingFromXmlDeclaration(std::string_view bytes) {
  constexpr std::string_view kPrefix = "<?xml";
  if (bytes.size() < kPrefix.size() || bytes.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  const std::size_t end = bytes.find("?>");
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view declaration = bytes.substr(0, end);
  const std::size_t at = declaration.find("encoding");
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t open = declaration.find_first_of("\"'", at);
  if (open == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t close = declaration.find(declaration[open], open + 1);
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  return EncodingFromLabel(declaration.substr(open + 1, close - open - 1));
}

}  // namespace

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
  // 3. The XML declaration, and the XML *default*, both of which apply only to an XML content type.
  //
  // **An XML document with no declaration is UTF-8, not windows-1252.** The fallback below is
  // HTML's, and it is right for HTML for the reason written there; XML has never had it. The
  // difference is observable well beyond decoding: a document's character set is what HTML's
  // "encoding-parse a URL" encodes a query with, so `url/a-element-xhtml.xhtml` -- whose only
  // declaration is `<?xml version="1.0" encoding="UTF-8"?>` -- reported `?q=%26%238995%3B` for a
  // link a browser reports as `?q=%E2%8C%A3`. Five subtests, and they only became visible once
  // `<a href>` started honouring the document's charset at all (TD-0058).
  if (IsXmlContentType(content_type)) {
    if (const std::optional<Encoding> declared = EncodingFromXmlDeclaration(bytes)) {
      return *declared;
    }
    return Encoding::Utf8;
  }
  // 4. The prescan.
  if (const std::optional<Encoding> found = PrescanForMeta(bytes)) {
    AddPerformanceCounter(PerfCounterId::EncodingFromPrescan);
    return *found;
  }
  // 5. windows-1252, and not UTF-8. A page with no declaration is overwhelmingly old, and decoding it
  // as UTF-8 turns every high byte into U+FFFD where windows-1252 renders what its author saw.
  AddPerformanceCounter(PerfCounterId::EncodingFellBackToWindows1252);
  return Encoding::Windows1252;
}

bool DecodeBytesStreaming(std::string& leftover, std::string_view bytes, Encoding encoding,
                          std::string& out, bool stream, bool fatal) {
  out.clear();
  // ISO-2022-JP's leftover is decoder *state*, not unconsumed input. Prepending it as bytes
  // would inject NULs. The other encodings hold a prefix of the byte stream.
  if (encoding == Encoding::Iso2022Jp) {
    return DecodeMultiByteStreaming(bytes, encoding, out, leftover, stream, fatal);
  }
  std::string input;
  input.reserve(leftover.size() + bytes.size());
  input.append(leftover);
  input.append(bytes.data(), bytes.size());
  leftover.clear();
  switch (encoding) {
    case Encoding::Utf8:
      return DecodeUtf8(input, out, leftover, stream, fatal);
    case Encoding::Utf16Le:
      return DecodeUtf16(input, true, out, leftover, stream, fatal);
    case Encoding::Utf16Be:
      return DecodeUtf16(input, false, out, leftover, stream, fatal);
    case Encoding::ShiftJis:
    case Encoding::EucJp:
    case Encoding::EucKr:
    case Encoding::Big5:
    case Encoding::Gb18030:
    case Encoding::Gbk:
      return DecodeMultiByteStreaming(input, encoding, out, leftover, stream, fatal);
    default:
      return DecodeSingleByte(input, encoding, out, fatal);
  }
}

bool DecodeBytes(std::string_view bytes, Encoding encoding, std::string& out, bool fatal) {
  std::string leftover;
  return DecodeBytesStreaming(leftover, bytes, encoding, out, false, fatal);
}

std::string DecodeBytes(std::string_view bytes, Encoding encoding) {
  std::string out;
  (void)DecodeBytes(bytes, encoding, out, false);
  return out;
}

std::string DecodeToUtf8(std::string_view bytes, Encoding encoding) {
  // The BOM is not text. One that reached the tokenizer would be a zero-width character at the start
  // of the document -- invisible, and it shifts every offset a parse error reports.
  const std::size_t bom = BomLength(bytes);
  return DecodeBytes(bytes.substr(std::min(bom, bytes.size())), encoding);
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

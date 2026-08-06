#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::html {

// Which encoding a document is in, and how to turn its bytes into text.
//
// ADR 0025 §2. **Encoding correctness is a security property, not only a rendering one**, and that is
// the reason this is implemented literally rather than approximately: a decoder that emits a `<`
// where the specification says U+FFFD turns a sanitised document into a script-executing one.
// Encoding confusion leading to XSS is a real, repeatedly exploited bug family, and every rule below
// that looks pedantic is load-bearing for it.
//
// The set is the one ADR 0025 §2 chose by usage, and it is now complete: the single-byte tables here,
// and the multi-byte family in MultiByteEncodings.cpp behind generated indexes.
enum class Encoding : std::uint8_t {
  Utf8,
  // The fallback, and it is **not** UTF-8. That is what the specification says, and the reason is
  // that a page with no declaration is overwhelmingly old: decoding such a page as UTF-8 turns every
  // high byte into U+FFFD, where windows-1252 renders the text its author saw.
  Windows1252,
  Latin1,       // ISO-8859-1, which the specification maps *to* windows-1252 -- see DecodeToUtf8
  Iso8859_2,
  Iso8859_5,
  Iso8859_7,
  Iso8859_9,
  Iso8859_15,
  Utf16Le,
  Utf16Be,
  // The legacy multi-byte family, ADR 0025 §2's fourth group and session 32. Each is a lead byte, a
  // trail byte and its own pointer arithmetic over one of four generated indexes -- see
  // MultiByteEncodings.cpp, where the *ranges* are the difficulty: a wrong one produces plausible
  // wrong characters rather than a failure.
  ShiftJis,
  EucJp,
  EucKr,
  Big5,
  Gb18030,
};

// A label as a document wrote it -- `utf-8`, `UTF8`, `iso-8859-1`, `latin1`, `windows-1252`, `cp1252`
// -- resolved to an encoding, or nothing when it names one this browser does not have.
//
// Nothing is the important answer: an unrecognised label must fall through to the *next* step of the
// sniffing algorithm rather than to UTF-8, because a page that declares `shift_jis` and gets UTF-8 is
// a page whose bytes are reinterpreted, which is the confusion this file exists to prevent.
std::optional<Encoding> EncodingFromLabel(std::string_view label);

// The encoding a document is in, by the WHATWG Encoding Standard's order:
//
//   1. a byte-order mark, which wins over everything -- including a contradictory `charset`, because
//      the BOM is *in the bytes* and a header is a claim about them;
//   2. an explicit `charset` in `Content-Type`;
//   3. a prescan of the first 1024 bytes for `<meta charset>` or `<meta http-equiv>`;
//   4. windows-1252.
//
// `content_type` is the header exactly as it arrived, or empty. The prescan bound is the
// specification's 1024 bytes and it is a bound rather than a heuristic: a `<meta charset>` after it
// does not count, in every browser, so a page that puts one at byte 2000 is decoded as though it had
// none -- and matching that is what makes this browser agree with the one the page was tested in.
Encoding SniffEncoding(std::string_view bytes, std::string_view content_type = {});

// The multi-byte decoders, in their own translation unit because their tables are 648KB of generated
// data. Called by DecodeToUtf8; separate so that this header does not imply the tables.
std::string DecodeMultiByte(std::string_view bytes, Encoding encoding);

// The document's bytes as UTF-8, with every ill-formed sequence replaced by U+FFFD.
//
// **Never a skipped byte and never a raw byte passed through**, which are the two shortcuts that turn
// a decoder into an XSS vector. The substitution follows the specification's rules exactly: a maximal
// subpart of an ill-formed sequence becomes *one* U+FFFD, so `\xE0\x80\x41` is one replacement
// followed by `A` rather than two replacements or a swallowed `A`.
std::string DecodeToUtf8(std::string_view bytes, Encoding encoding);

// How many bytes of BOM to skip, so a caller that has already sniffed does not decode it as text. A
// BOM that reached the tokenizer would be a zero-width character at the start of the document, which
// is invisible and shifts every offset a parse error reports.
std::size_t BomLength(std::string_view bytes);

}  // namespace microbrowser::html

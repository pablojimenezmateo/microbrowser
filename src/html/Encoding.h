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
  // GBK decodes as GB18030 and **encodes differently**, which is why it is an encoding here rather
  // than a label for one. Its encoder has the standard's "is GBK" flag set: the euro sign is one
  // byte, and every code point the two-byte form cannot reach is an *error* instead of a four-byte
  // sequence. A page that declares `gbk` and gets GB18030's encoder sends four bytes where every
  // other browser sends `&#…;`, and a form handler that split on those bytes would see a field the
  // user never typed.
  Gbk,
  // ISO-2022-JP, and the only stateful member of the family: the byte 0x41 means `A` or a kanji
  // depending on an escape sequence some distance earlier. That is why it is here at all rather
  // than filed with the others -- a stateful encoding is the one where "decode the tail of a
  // document" is not a well-defined question, and where a smuggled escape changes what every byte
  // after it means.
  Iso2022Jp,
};

// The encoding's name, exactly as the Encoding Standard spells it -- `UTF-8`, `Shift_JIS`,
// `ISO-2022-JP`. This is what `document.characterSet` answers and what a form's `accept-charset`
// resolves to, and it is the *canonical* name rather than the label the document happened to write:
// two documents that said `sjis` and `x-sjis` are in one encoding and must say so identically.
std::string_view EncodingName(Encoding encoding);

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

// The multi-byte encoders, in the same translation unit and for the same reason -- and `state` is
// ISO-2022-JP's, threaded through rather than kept in a static, because two encodings running at
// once (a form's fields and a URL's query) must not share one. `Encoder` is what callers use; these
// two are the seam that keeps the tables out of this header.
bool EncodeMultiByte(std::uint32_t code_point, Encoding encoding, int& state, std::string& out);
void FinishMultiByte(Encoding encoding, int& state, std::string& out);

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

// The other direction, and it is not the decoder read backwards.
//
// **An encoder can fail and a decoder cannot.** A decoder always has an answer -- U+FFFD -- but
// windows-1252 has no byte for `𝄞` and Shift_JIS has none for `한`, and what happens then is the
// *caller's* decision rather than this file's: a URL query spells the failure `%26%23119070%3B`, a
// form body spells it `&#119070;`, and `TextEncoder` cannot fail at all because it only encodes
// UTF-8. So this reports the failure and substitutes nothing. A substitution chosen here would be
// one every caller inherits and none of them asked for, and two of those three spellings would then
// have to un-do it.
//
// Stateful because one of these encodings is: ISO-2022-JP writes an escape sequence when it changes
// character set and *another* when it changes back, so a code point's bytes depend on the ones
// before it and the end of the input has bytes of its own. `Finish` is where those go, and the
// class exists so that a caller cannot forget the state by encoding a string a character at a time.
class Encoder {
 public:
  explicit Encoder(Encoding encoding) : encoding_(encoding) {}

  // Appends `code_point`'s bytes to `out` and returns true. False means the encoding has no
  // representation for it -- and **`out` may still have grown**, which is not sloppiness: an
  // ISO-2022-JP stream in the jis0208 state has to be escaped back to ASCII *before* the caller can
  // write the failure as `&#1234;`, because those nine ASCII bytes inside a kanji run decode as
  // kanji. So a caller appends its own spelling of the failure after whatever this left, never
  // instead of it.
  //
  // A surrogate is not a scalar value and is a failure here rather than an assertion: the input is
  // a page's string, which may be any sequence of UTF-16 code units at all.
  bool Encode(std::uint32_t code_point, std::string& out);

  // The bytes that belong at the end of the output, which for ISO-2022-JP is the escape back to
  // ASCII and for everything else is nothing. Not folded into `Encode`, because the caller decides
  // where the *stream* ends: a form body's fields are one stream and a URL's query is another.
  void Finish(std::string& out);

 private:
  Encoding encoding_;
  // ISO-2022-JP's encoder state, and nothing else's. 0 is ASCII, 1 is Roman, 2 is jis0208 -- the
  // standard's three, in its order.
  int state_ = 0;
};

// The next Unicode *scalar value* in `input`, advancing `at` past it.
//
// Not simply "the next UTF-8 sequence", and the difference is where the surrogates go. A string that
// came from JavaScript is a sequence of UTF-16 code units and may hold a lone surrogate or a pair
// spelled as two escapes, so this browser's internal form of one is WTF-8 rather than UTF-8. Every
// place such a string is *encoded* -- a URL's query, a form's body -- the specification says it is
// first converted to a scalar value string, which pairs the halves that pair and replaces the ones
// that do not with U+FFFD. Doing that in one function is what stops the two callers from disagreeing
// about what `"💩"` weighs.
std::uint32_t NextScalarValue(std::string_view input, std::size_t& at);

// `input`, which is UTF-8, encoded in `encoding` with every unencodable code point replaced by the
// HTML numeric character reference the form-submission and URL-query algorithms both use. That
// substitution is *their* rule rather than the encoder's, and it is here rather than at each caller
// because it is the same rule and because getting it wrong is a field a server reads as markup.
std::string EncodeWithNumericEscapes(std::string_view input, Encoding encoding);

}  // namespace microbrowser::html

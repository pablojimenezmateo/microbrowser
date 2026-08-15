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
// The Encoding Standard's set. ADR 0025 §2 named the ones a page is actually in; the rest of the
// single-byte family is the same 128-pointer lookup (byte minus 0x80) generated into
// SingleByteIndexes.inc. Refusing a label the standard lists is a RangeError a page cannot recover
// from, which is why the unused-on-HN encodings are here rather than absent.
enum class Encoding : std::uint8_t {
  Utf8,
  Windows1252,  // the fallback; ISO-8859-1's labels map here
  Latin1,       // unused by EncodingFromLabel; kept so a switch cannot forget the alias
  Ibm866,
  Iso8859_2,
  Iso8859_3,
  Iso8859_4,
  Iso8859_5,
  Iso8859_6,
  Iso8859_7,
  Iso8859_8,
  Iso8859_8I,  // same index as Iso8859_8; different canonical name
  Iso8859_9,   // canonical name windows-1254
  Iso8859_10,
  Iso8859_13,
  Iso8859_14,
  Iso8859_15,
  Iso8859_16,
  Koi8R,
  Koi8U,
  Macintosh,
  Windows874,
  Windows1250,
  Windows1251,
  Windows1253,
  Windows1255,
  Windows1256,
  Windows1257,
  Windows1258,
  XMacCyrillic,
  XUserDefined,
  Replacement,  // iso-2022-kr and the other labels the standard refuses to honour
  Utf16Le,
  Utf16Be,
  ShiftJis,
  EucJp,
  EucKr,
  Big5,
  Gb18030,
  Gbk,  // shares GB18030's decoder, not its encoder
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

// The charset parameter of a MIME type, or nothing when there is none or it
// names no encoding. XHR uses this and then falls back to UTF-8, which is not
// the document sniffer's windows-1252 -- two questions, two functions.
std::optional<Encoding> EncodingFromMimeType(std::string_view content_type);

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
// The same decoders with TextDecoder's leftover: an incomplete lead at the end of `bytes` is
// written to `leftover` when `stream` is true rather than becoming U+FFFD.
bool DecodeMultiByteStreaming(std::string_view bytes, Encoding encoding, std::string& out,
                              std::string& leftover, bool stream);

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
//
// A leading BOM is skipped: it is not text, and one that reached the tokenizer would be a
// zero-width character at the start of the document. `DecodeBytes` is the same conversion without
// that skip, which is what `TextDecoder` needs when `ignoreBOM` is true -- a BOM it was told to
// keep is three (or two) bytes of the encoding, not a signature to discard.
std::string DecodeToUtf8(std::string_view bytes, Encoding encoding);

// Decode `bytes` in `encoding` without stripping a leading BOM. `fatal` is TextDecoder's flag:
// a byte with no mapping is U+FFFD when false and a failure when true. The string-returning
// overload never fails -- that is the document decoder, which always has an answer.
bool DecodeBytes(std::string_view bytes, Encoding encoding, std::string& out, bool fatal);
std::string DecodeBytes(std::string_view bytes, Encoding encoding);

// TextDecoder's streaming decode. `leftover` is the decoder instance's I/O queue: prepended to
// `bytes`, then replaced with any incomplete suffix when `stream` is true. A flush (`stream`
// false) turns that suffix into U+FFFD -- or a failure when `fatal` is true. BOM handling is
// not here; TextDecoder strips U+FEFF from the *output* after this returns.
bool DecodeBytesStreaming(std::string& leftover, std::string_view bytes, Encoding encoding,
                          std::string& out, bool stream, bool fatal);

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

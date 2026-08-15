// Character encodings, and the substitution rules that make them a security property.
//
// ADR 0025 §2. **Encoding confusion leading to XSS is a real, repeatedly exploited bug family**, so
// the assertions here are mostly about ill-formed input: a decoder that emits a `<` where the
// specification says U+FFFD turns a sanitised document into a script-executing one, and one that
// *swallows* a byte hides a character a filter was looking for.

#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "html/Encoding.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

using html::DecodeToUtf8;
using html::Encoding;
using html::EncodingFromLabel;
using html::SniffEncoding;

// U+FFFD as UTF-8, which is what every substitution below produces.
const std::string kReplacement = "\xEF\xBF\xBD";

std::string Decode(const std::string& bytes, Encoding encoding) {
  return DecodeToUtf8(bytes, encoding);
}

}  // namespace

void RegisterEncodingTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Encoding/ADocumentsBytesBecomeTextBeforeTheTokenizerSeesThem", [] {
    // The wiring, which is the difference between a decoder that exists and one that works: the
    // bytes are decoded in `Page::Load`, so the tokenizer's input is code points. Without it every
    // assertion above is about a function nothing calls.
    gfx::FontLibrary library;
    gfx::FontCatalog fonts{library};
    fonts.Register("Test", 400, false, BuildSyntheticFont());
    fonts.SetDefaultFamily("Test");

    // A windows-1252 page with no declaration -- the fallback case, and the common one for old
    // content. `caf\xE9` has to become `café` in the DOM rather than a replacement character.
    engine::Page fallback(fonts);
    fallback.Load("<html><body><p id=t>caf\xE9</p></body></html>", "https://example.org/");
    Expect(fallback.CurrentDocument() != nullptr, "it parsed");
    Expect(fallback.CurrentDocument()->Body() != nullptr, "with a body");
    ExpectEqString(fallback.CurrentDocument()->Body()->TextContent(), "café",
                   "windows-1252 without a declaration renders the word");

    // And a `Content-Type` that says otherwise is obeyed: the same bytes are ill-formed UTF-8, so
    // this is where a replacement character is the *right* answer.
    engine::Page declared(fonts);
    declared.Load("<html><body><p>caf\xE9</p></body></html>", "https://example.org/",
                  csp::PolicyList{}, "text/html; charset=utf-8");
    ExpectEqString(declared.CurrentDocument()->Body()->TextContent(), "caf\xEF\xBF\xBD",
                   "a page that declares UTF-8 and is not gets U+FFFD, which is the honest answer");

    // A `<meta charset>` in the document, which is how most pages say it.
    engine::Page meta(fonts);
    meta.Load("<html><head><meta charset=\"utf-8\"></head><body><p>caf\xC3\xA9</p></body></html>",
              "https://example.org/");
    ExpectEqString(meta.CurrentDocument()->Body()->TextContent(), "café",
                   "and a meta-declared UTF-8 document decodes as UTF-8");

    // A UTF-16 document, which is only reachable through its BOM -- and whose bytes are nothing like
    // HTML until they are decoded.
    std::string utf16;
    const std::string source = "<p>hi</p>";
    utf16 += "\xFF\xFE";
    for (const char c : source) {
      utf16.push_back(c);
      utf16.push_back('\0');
    }
    engine::Page wide(fonts);
    wide.Load(utf16, "https://example.org/");
    Expect(wide.CurrentDocument()->Body() != nullptr, "a UTF-16 document parses at all");
    ExpectEqString(wide.CurrentDocument()->Body()->TextContent(), "hi",
                   "because the BOM was found before the tokenizer ran");
  });

  AddTest(tests, "Encoding/AnIllFormedSequenceBecomesOneReplacementAndEatsNothingAfterIt", [] {
    // **The rule the whole file turns on.** The specification replaces a *maximal subpart* with one
    // U+FFFD and does not consume the byte that ended it -- so a character after a bad sequence
    // survives. A decoder that consumed it would hide the very byte a sanitiser was looking for.
    // `E0 80 41` is **two** replacements and then the A, not one: `E0`'s valid second byte is
    // A0-BF, so the maximal subpart ends after `E0`, the `80` is a stray of its own, and the `A`
    // survives. I first asserted one replacement here and the decoder was right -- the count comes
    // from the per-lead ranges, not from the resulting code point.
    ExpectEqString(Decode("\xE0\x80\x41", Encoding::Utf8), kReplacement + kReplacement + "A",
                   "two replacements, then the A -- and never a swallowed A");
    ExpectEqString(Decode("\xE2\x82\x41", Encoding::Utf8), kReplacement + "A",
                   "while a lead whose second byte *is* in range takes it, so this is one");
    ExpectEqString(Decode("\xE0\x80<script>", Encoding::Utf8),
                   kReplacement + kReplacement + "<script>",
                   "and a `<` after a bad sequence reaches the tokenizer as a `<`, which is what "
                   "makes the tokenizer's own rules apply to it -- the count of replacements before "
                   "it is not the point, the surviving `<` is");
    // A truncated sequence at the end of input is one replacement, not silence.
    ExpectEqString(Decode("A\xE2\x82", Encoding::Utf8), "A" + kReplacement, "truncated at the end");
    // A stray continuation byte is one replacement each, because each is its own maximal subpart.
    ExpectEqString(Decode("\x80\x80", Encoding::Utf8), kReplacement + kReplacement, "two strays");
  });

  AddTest(tests, "Encoding/AnOverlongEncodingIsRefused", [] {
    // `\xC0\x80` is a two-byte spelling of NUL, and accepting it is how a filter looking for a literal
    // `\0` -- or for `/`, or for `<` -- is bypassed. The lower bound per sequence length is what
    // rejects it, which is why the decoder carries one.
    ExpectEqString(Decode("\xC0\x80", Encoding::Utf8), kReplacement + kReplacement,
                   "an overlong NUL is not a NUL");
    ExpectEqString(Decode("\xC1\xBF", Encoding::Utf8), kReplacement + kReplacement,
                   "and 0xC1 is never a lead, which is the other overlong two-byte form");
    ExpectEqString(Decode("\xC0\xAF", Encoding::Utf8), kReplacement + kReplacement,
                   "and an overlong `/` is not a `/`, which is the directory-traversal form");
    // `\xE0\x80\xAF` is a three-byte spelling of `/`: both trailing bytes *are* continuations, so
    // the sequence is well-formed in shape and out of range in value -- which is exactly the case a
    // decoder that only checked shape would let through. One replacement, all three bytes consumed.
    // Three replacements, not one: `E0`'s second byte must be A0-BF, so each of these bytes is its
    // own maximal subpart. What matters for the security property is that **no `/` comes out** --
    // the number of replacements is the specification's bookkeeping, and the absent slash is the bug
    // class.
    ExpectEqString(Decode("\xE0\x80\xAF", Encoding::Utf8),
                   kReplacement + kReplacement + kReplacement,
                   "the three-byte overlong slash produces no slash");
    // The shortest form of the same character is of course fine.
    ExpectEqString(Decode("/", Encoding::Utf8), "/", "a real slash is a slash");
  });

  AddTest(tests, "Encoding/ASurrogateIsNotACharacterInUtf8", [] {
    // CESU-8 and the `\xED\xA0\x80` family are how a code point is smuggled past a filter that
    // decoded properly. A surrogate is ill-formed in UTF-8 whatever it encodes.
    ExpectEqString(Decode("\xED\xA0\x80", Encoding::Utf8), kReplacement + kReplacement + kReplacement,
                   "a high surrogate is three ill-formed bytes");
    // And a code point above U+10FFFF, which no lead byte may introduce.
    ExpectEqString(Decode("\xF5\x80\x80\x80", Encoding::Utf8),
                   kReplacement + kReplacement + kReplacement + kReplacement, "beyond U+10FFFF");
  });

  AddTest(tests, "Encoding/WellFormedUtf8SurvivesUntouched", [] {
    // The other direction, which is the one a bug in the paragraph above would break: every valid
    // length has to pass through byte for byte.
    const std::string text = "ASCII, é, €, 𝄞";  // 1, 2, 3 and 4-byte sequences
    ExpectEqString(Decode(text, Encoding::Utf8), text, "valid UTF-8 is unchanged");
  });

  AddTest(tests, "Encoding/Iso8859OneIsWindows1252BecauseThatIsWhatAuthorsMeant", [] {
    // The specification maps `iso-8859-1`, `latin1` and even `ascii` to the windows-1252 decoder. It
    // looks like a violation and is the opposite: a page labelled ISO-8859-1 containing a 0x93 means a
    // curly quote, because that is what the authoring tool emitted, and rendering a C1 control there
    // is rendering something no reader ever saw.
    Expect(EncodingFromLabel("iso-8859-1") == Encoding::Windows1252, "iso-8859-1");
    Expect(EncodingFromLabel("latin1") == Encoding::Windows1252, "latin1");
    Expect(EncodingFromLabel("us-ascii") == Encoding::Windows1252, "and ascii");
    ExpectEqString(Decode("\x93hello\x94", Encoding::Windows1252), "“hello”",
                   "0x93 and 0x94 are curly quotes rather than controls");
    // The Encoding Standard maps the five "undefined" windows-1252 bytes to the C1 controls they
    // occupy, not to U+FFFD. A replacement there would be a character no other browser emits.
    ExpectEqString(Decode("\x81", Encoding::Windows1252), "\xC2\x81", "0x81 is U+0081");
    // Above 0xA0 the two encodings agree, which is most of the range.
    ExpectEqString(Decode("\xE9", Encoding::Windows1252), "é", "é is é");
  });

  AddTest(tests, "Encoding/TheOtherIsoPartsDecodeTheirOwnAlphabets", [] {
    ExpectEqString(Decode("\xE0", Encoding::Iso8859_2), "ŕ", "Latin-2");
    // 0xB0 is А and 0xC0 is Р: the Cyrillic alphabet starts at 0xB0 in this part, which I had wrong
    // in the first version of this assertion.
    ExpectEqString(Decode("\xB0", Encoding::Iso8859_5), "А", "Cyrillic А");
    ExpectEqString(Decode("\xC0", Encoding::Iso8859_5), "Р", "and Р sixteen letters later");
    ExpectEqString(Decode("\xC1", Encoding::Iso8859_7), "Α", "Greek alpha, which is not an A");
    ExpectEqString(Decode("\xF0", Encoding::Iso8859_9), "ğ", "Turkish");
    ExpectEqString(Decode("\xA4", Encoding::Iso8859_15), "€", "and Latin-9's euro");
    // The same byte is a different character in each, which is the entire reason the label matters:
    // getting it wrong is not a rounding error, it is different text.
    Expect(Decode("\xC1", Encoding::Iso8859_7) != Decode("\xC1", Encoding::Windows1252),
           "one byte, two alphabets");
  });

  AddTest(tests, "Encoding/TheRestOfTheSingleByteFamilyIsTheEncodingStandardsIndex", [] {
    Expect(EncodingFromLabel("ibm866") == Encoding::Ibm866, "ibm866");
    Expect(EncodingFromLabel("iso-8859-3") == Encoding::Iso8859_3, "iso-8859-3");
    Expect(EncodingFromLabel("koi8-r") == Encoding::Koi8R, "koi8-r");
    Expect(EncodingFromLabel("macintosh") == Encoding::Macintosh, "macintosh");
    Expect(EncodingFromLabel("windows-1250") == Encoding::Windows1250, "windows-1250");
    Expect(EncodingFromLabel("iso-8859-8-i") == Encoding::Iso8859_8I, "iso-8859-8-i");
    ExpectEqString(std::string(html::EncodingName(Encoding::Iso8859_8I)), "ISO-8859-8-I",
                   "8-I is not 8");
    Expect(EncodingFromLabel("csisolatin1") == Encoding::Windows1252, "a previously missing alias");
    Expect(EncodingFromLabel("iso_8859-1:1987") == Encoding::Windows1252, "and a colon in a label");
    // Vertical tab is not ASCII whitespace. TrimAscii would strip it and honour the label.
    Expect(!EncodingFromLabel("\vwindows-1252").has_value(), "\\v does not strip");
    // ISO-8859-3 has holes; 0xA5 is one. Non-fatal is U+FFFD, fatal is a failure.
    ExpectEqString(Decode("\xA5", Encoding::Iso8859_3), kReplacement, "a hole is U+FFFD");
    std::string fatal_out;
    Expect(!html::DecodeBytes("\xA5", Encoding::Iso8859_3, fatal_out, true),
           "and fatal refuses it");
    Expect(html::DecodeBytes("A", Encoding::Iso8859_3, fatal_out, true), "ASCII still decodes");
  });

  AddTest(tests, "Encoding/TheBomWinsOverAContradictoryHeader", [] {
    // The BOM is *in the bytes*; a `charset` is a claim about them. When they disagree the bytes are
    // the evidence, and that ordering is the specification's.
    Expect(SniffEncoding("\xEF\xBB\xBF<html>", "text/html; charset=windows-1252") == Encoding::Utf8,
           "a UTF-8 BOM beats a windows-1252 header");
    Expect(SniffEncoding("\xFF\xFE<\0h\0", "text/html; charset=utf-8") == Encoding::Utf16Le,
           "and a UTF-16 BOM beats a UTF-8 one");
    // And the BOM is not text: it must not reach the tokenizer, where it would be an invisible
    // zero-width character that shifts every reported offset.
    ExpectEqString(Decode("\xEF\xBB\xBF" "hi", Encoding::Utf8), "hi", "the BOM is consumed");
  });

  AddTest(tests, "Encoding/TheHeaderBeatsTheMetaAndTheMetaBeatsTheFallback", [] {
    Expect(SniffEncoding("<meta charset=iso-8859-7>", "text/html; charset=utf-8") == Encoding::Utf8,
           "the header wins over a meta");
    Expect(SniffEncoding("<html><head><meta charset=\"iso-8859-5\"></head>") == Encoding::Iso8859_5,
           "the meta wins over the fallback");
    Expect(SniffEncoding("<meta charset=utf-16>") == Encoding::Utf8,
           "a UTF-16 meta is UTF-8: the tag is ASCII, so the document cannot be UTF-16");
    Expect(SniffEncoding("<meta charset=x-user-defined>") == Encoding::Windows1252,
           "and x-user-defined in a meta is windows-1252");
    Expect(SniffEncoding("<html>", "text/html; charset=utf-16le") == Encoding::Utf16Le,
           "a Content-Type charset of UTF-16 is honoured -- that one can actually be UTF-16");
    Expect(SniffEncoding("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">") ==
               Encoding::Utf8,
           "including the http-equiv spelling, which is most of the old web");
    // A quoted charset in a header is in the wild, and keeping the quote would look up a label that
    // does not exist -- falling back to windows-1252 on a page that said UTF-8.
    Expect(SniffEncoding("<html>", "text/html; charset=\"utf-8\"") == Encoding::Utf8,
           "a quoted charset is unquoted first");
  });

  AddTest(tests, "Encoding/TheFallbackIsWindows1252AndNotUtf8", [] {
    // What the specification says, and the reason is empirical: a page with no declaration is
    // overwhelmingly old, and decoding it as UTF-8 turns every high byte into U+FFFD where
    // windows-1252 renders the text its author saw.
    Expect(SniffEncoding("<html><body>caf\xE9</body></html>") == Encoding::Windows1252,
           "no declaration means windows-1252");
    ExpectEqString(Decode("caf\xE9", SniffEncoding("caf\xE9")), "café",
                   "which renders the word rather than a replacement character");
  });

  AddTest(tests, "Encoding/APrescanStopsAtTheSpecifiedBound", [] {
    // 1024 bytes, and it is a bound rather than a heuristic: a `<meta charset>` after it does not
    // count in any browser, so a page that puts one at byte 2000 is decoded as though it had none.
    // Matching that is what makes this browser agree with the one the page was tested in.
    const std::string early = "<meta charset=iso-8859-5>" + std::string(2000, ' ');
    Expect(SniffEncoding(early) == Encoding::Iso8859_5, "inside the bound it counts");
    const std::string late = std::string(1100, ' ') + "<meta charset=iso-8859-5>";
    Expect(SniffEncoding(late) == Encoding::Windows1252, "and past it, it does not");
  });

  AddTest(tests, "Encoding/AnUnknownLabelFallsThroughRatherThanToUtf8", [] {
    // The confusion in miniature: a page that declares an encoding and is decoded as UTF-8 is a page
    // whose bytes are reinterpreted. An unrecognised label must take the *next* step of the algorithm.
    //
    // `shift_jis` and `gb18030` were the examples here until session 32 built them, `iso-2022-jp`
    // until that family landed, and `ibm866` / `hz-gb-2312` until the rest of the single-byte index
    // did. **The changed assertion is each session's own change.** What is left is labels the
    // standard does not list -- EBCDIC, invented names -- which must still fall through.
    Expect(!EncodingFromLabel("cp037").has_value(), "an encoding this browser lacks is nothing");
    Expect(!EncodingFromLabel("ebcdic").has_value(), "and so is another");
    Expect(!EncodingFromLabel("").has_value(), "and an empty label");
    Expect(EncodingFromLabel("ibm866") == Encoding::Ibm866, "ibm866 is in the index now");
    Expect(EncodingFromLabel("hz-gb-2312") == Encoding::Replacement,
           "and hz-gb-2312 is replacement, not a decoder");
    ExpectEqString(Decode("ABC", Encoding::Replacement), kReplacement,
                   "the replacement decoder emits one U+FFFD for any non-empty input");
    ExpectEqString(Decode("", Encoding::Replacement), "",
                   "and empty input stays empty");
    Expect(html::EncodingFromMimeType("text/plain;charset=ibm866") == Encoding::Ibm866,
           "XHR reads charset off Content-Type without falling through to windows-1252");
    Expect(!html::EncodingFromMimeType("text/plain").has_value(),
           "and no charset is nothing, so the caller can default to UTF-8");
    // With an unknown header, the meta still gets its turn.
    Expect(SniffEncoding("<meta charset=utf-8>", "text/html; charset=x-nonesuch") == Encoding::Utf8,
           "the algorithm continues rather than guessing");
  });

  AddTest(tests, "Encoding/Utf16PairsAndStraysAreHandledSeparately", [] {
    // A surrogate pair is one character; a lone surrogate is U+FFFD and must not be passed through,
    // because encoding one as UTF-8 produces bytes no decoder accepts.
    ExpectEqString(Decode(std::string("\x34\xD8\x1E\xDD", 4), Encoding::Utf16Le), "𝄞",
                   "a pair is one character");
    ExpectEqString(Decode(std::string("\x34\xD8", 2), Encoding::Utf16Le), kReplacement,
                   "an unpaired high surrogate is a replacement");
    ExpectEqString(Decode(std::string("\x00\xD8\x00", 3), Encoding::Utf16Le), kReplacement,
                   "and a high plus an odd byte is still one replacement, not two");
    ExpectEqString(Decode(std::string("\x1E\xDD", 2), Encoding::Utf16Le), kReplacement,
                   "and so is a lone low surrogate");
    // An odd trailing byte is a replacement rather than a silently dropped half-unit.
    ExpectEqString(Decode(std::string("h\0i", 3), Encoding::Utf16Le), "h" + kReplacement,
                   "an odd length ends in a replacement");
    // Big-endian is the same text with the units swapped, which is what the label distinguishes.
    ExpectEqString(Decode(std::string("\0h\0i", 4), Encoding::Utf16Be), "hi", "big endian");
  });
  AddTest(tests, "Encoding/TheLegacyMultiByteLabelsResolve", [] {
    // Every spelling in the Encoding Standard's label list, because a page whose label is unrecognised
    // falls through to windows-1252 -- and windows-1252 applied to Japanese is mojibake rather than
    // text. `ms_kanji`, `x-sjis` and `windows-31j` are all in real documents.
    Expect(EncodingFromLabel("Shift_JIS") == Encoding::ShiftJis, "the canonical name");
    Expect(EncodingFromLabel("ms_kanji") == Encoding::ShiftJis, "and an alias");
    Expect(EncodingFromLabel("windows-31j") == Encoding::ShiftJis, "and another");
    Expect(EncodingFromLabel("EUC-JP") == Encoding::EucJp, "euc-jp");
    Expect(EncodingFromLabel("euc-kr") == Encoding::EucKr, "euc-kr");
    Expect(EncodingFromLabel("ks_c_5601-1987") == Encoding::EucKr, "and its Korean alias");
    Expect(EncodingFromLabel("big5") == Encoding::Big5, "big5");
    Expect(EncodingFromLabel("ISO-2022-JP") == Encoding::Iso2022Jp, "iso-2022-jp");
    Expect(EncodingFromLabel("csiso2022jp") == Encoding::Iso2022Jp, "and its alias");
    // **GBK is its own encoding here and `gb18030` is the only label for the other one.** They share
    // a decoder -- GB18030 is a superset, so a GBK document decoded with it produces the same
    // characters -- and they do not share an *encoder*: GBK refuses everything the two-byte form
    // cannot reach where GB18030 emits four bytes. A page labelled `gbk` whose form sent four-byte
    // sequences would be sending bytes its own server has no decoder for.
    Expect(EncodingFromLabel("gbk") == Encoding::Gbk, "gbk is gbk");
    Expect(EncodingFromLabel("gb2312") == Encoding::Gbk, "and so is gb2312");
    Expect(EncodingFromLabel("gb18030") == Encoding::Gb18030, "and gb18030 is not");
  });

  AddTest(tests, "Encoding/TheMultiByteDecodersProduceRealText", [] {
    // One sentence per encoding, from bytes to characters. The expected strings are not typed from a
    // table: every one of them was produced by the platform's own codec for that encoding, and the
    // *whole* two-byte space of all five was swept against an independent implementation of the
    // standard's algorithm in the same session -- 27,972 sequences each, zero disagreements, plus
    // 4,000 random byte strings including truncated and interleaved ones. What is here is the part a
    // reader can check by eye.
    ExpectEqString(Decode("\x93\xFA\x96\x7B\x8C\xEA", Encoding::ShiftJis), "日本語",
                   "Shift_JIS");
    ExpectEqString(Decode("\xC6\xFC\xCB\xDC\xB8\xEC", Encoding::EucJp), "日本語", "EUC-JP");
    ExpectEqString(Decode("\xC7\xD1\xB1\xB9\xB8\xBB", Encoding::EucKr), "한국말", "EUC-KR");
    ExpectEqString(Decode("\xA5\xBF\xC5\xE9\xA6\x72", Encoding::Big5), "正體字", "Big5");
    ExpectEqString(Decode("\xD6\xD0\xCE\xC4", Encoding::Gb18030), "中文", "GB18030");
    // Halfwidth katakana is a single byte in Shift_JIS and a two-byte 0x8E sequence in EUC-JP -- the
    // same characters reached two different ways, which is the pair most likely to be got wrong.
    ExpectEqString(Decode("\xB1\xB2\xB3", Encoding::ShiftJis), "ｱｲｳ", "single-byte katakana");
    ExpectEqString(Decode("\x8E\xB1\x8E\xB2\x8E\xB3", Encoding::EucJp), "ｱｲｳ",
                   "and the 0x8E form");
    // ASCII passes through all five, which is what makes an HTML document in any of them parseable.
    ExpectEqString(Decode("<b>a</b>", Encoding::Big5), "<b>a</b>", "ASCII is ASCII");
  });

  AddTest(tests, "Encoding/AMultiByteDecoderRefusesRatherThanGuesses", [] {
    // A lead with no trail, a lead with a trail out of range, and a truncated tail. In each case the
    // answer is U+FFFD and **the byte that ended the sequence is decoded on its own** -- which is the
    // property the fuzz target exists for: a decoder that swallowed it would delete the `<` a
    // sanitiser was looking for.
    ExpectEqString(Decode("\x93", Encoding::ShiftJis), kReplacement, "a lead at the end");
    ExpectEqString(Decode("\x93\x3C", Encoding::ShiftJis), kReplacement + "<",
                   "a lead then a less-than: the less-than survives");
    ExpectEqString(Decode("\xC6\x41", Encoding::EucJp), kReplacement + "A",
                   "EUC-JP's trail range excludes ASCII entirely");
    ExpectEqString(Decode("\x81\x40\x3C", Encoding::EucKr), kReplacement + "@<",
                   "an unassigned EUC-KR lead pair");
    ExpectEqString(Decode("\x81\x40", Encoding::Big5), kReplacement + "@",
                   "Big5 restores an ASCII trail on a hole, same as EUC-KR");
    ExpectEqString(Decode("\x81\x81", Encoding::Big5), kReplacement,
                   "and a non-ASCII trail on a hole is consumed");
    // The 0x8E prefix with a byte that is not katakana, and a 0x8F -- JIS X 0212 -- whose trail bytes
    // are not trail bytes. Both consume one byte, not two or three.
    ExpectEqString(Decode("\x8E\x41", Encoding::EucJp), kReplacement + "A", "0x8E then ASCII");
    ExpectEqString(Decode("\x8F\x3C\x3C", Encoding::EucJp), kReplacement + "<<",
                   "0x8F whose trail bytes are not trail bytes keeps both");
    // GB18030's four-byte form: the *shape* is checked before four bytes are consumed, so
    // `81 30 3C 3C` costs only the 0x81 and the `<<` after it is text.
    ExpectEqString(Decode("\x81\x30\x3C\x3C", Encoding::Gb18030), kReplacement + "0<<",
                   "a malformed four-byte sequence does not eat the text after it");
    // Encoding Standard: a two-byte hole restores an ASCII trail; a non-ASCII trail is consumed.
    // Incomplete four-byte GB18030 at end-of-queue is one error for the remainder.
    ExpectEqString(Decode("\x41\xC7\x41", Encoding::EucKr), "A" + kReplacement + "A",
                   "EUC-KR hole then ASCII");
    ExpectEqString(Decode("\xAD\xAD", Encoding::EucKr), kReplacement, "EUC-KR hole consumes both");
    ExpectEqString(Decode("\x8E\x8E", Encoding::EucJp), kReplacement,
                   "EUC-JP 0x8E with a non-katakana trail consumes both");
    ExpectEqString(Decode("\x8F\xA1", Encoding::EucJp), kReplacement,
                   "EUC-JP 0x8F prefix at flush is one error");
    ExpectEqString(Decode("\xA0\x30\x2B", Encoding::Gb18030), kReplacement + "0+",
                   "GB18030 restores a 4-byte prefix whose third byte is out of range");
    ExpectEqString(Decode("\x81\x31", Encoding::Gb18030), kReplacement,
                   "incomplete GB18030 four-byte at flush is one error");
    ExpectEqString(Decode("\x81\x30\xFE", Encoding::Gb18030), kReplacement,
                   "and so is three bytes of a four-byte sequence");
    ExpectEqString(Decode("\x81\xFF", Encoding::Gb18030), kReplacement,
                   "GB18030 0xFF trail is consumed, not restored");
    ExpectEqString(Decode("\x81\x3A", Encoding::Gb18030), kReplacement + ":",
                   "GB18030 restores an ASCII byte that is not a trail");
    std::string fatal_out;
    Expect(!html::DecodeBytes("\xC7\x41", Encoding::EucKr, fatal_out, true),
           "fatal refuses an EUC-KR hole");
    Expect(!html::DecodeBytes("\x81\x40", Encoding::Big5, fatal_out, true),
           "and a Big5 hole");
    Expect(!html::DecodeBytes("\x85\x85", Encoding::ShiftJis, fatal_out, true),
           "and a Shift_JIS hole");
    Expect(!html::DecodeBytes("\x81\x31", Encoding::Gb18030, fatal_out, true),
           "and an incomplete GB18030 four-byte");
  });

  AddTest(tests, "Encoding/TheDecodeOnlyIndexesAndTheFourByteForm", [] {
    // Two things a decoder has and no encoder produces, both added in the ISO-2022-JP session.
    //
    // JIS X 0212 is EUC-JP's second index, reached through a 0x8F prefix. The standard's own encoder
    // never emits one, so a document containing one was written by something older than the web --
    // which is exactly why refusing it is not an option: those documents are the reason EUC-JP is
    // still decoded at all.
    ExpectEqString(Decode("\x8F\xB0\xA1", Encoding::EucJp), "丂",
                   "JIS X 0212's first ideograph, through 0x8F");
    // GB18030's four-byte form, which is what makes it the one legacy encoding that can say anything
    // Unicode can -- including above the BMP.
    ExpectEqString(Decode("\x81\x30\x81\x30", Encoding::Gb18030), "\xC2\x80",
                   "the four-byte form's first pointer is U+0080");
    ExpectEqString(Decode("\x94\x39\xDA\x33", Encoding::Gb18030), "💩",
                   "and it reaches above the BMP");
    // The GB18030-2005 revision's one inline exception, which is a pointer the range table cannot
    // express and which every implementation has to write out.
    ExpectEqString(Decode("\x81\x35\xF4\x37", Encoding::Gb18030), "\xEE\x9F\x87",
                   "pointer 7457 is U+E7C7 and nothing else");
  });

  AddTest(tests, "Encoding/Iso2022JpIsStatefulAndSaysSo", [] {
    // The escape sequence decides what every byte after it means, which is why this encoding is a
    // security question before it is a rendering one: a sanitiser that scanned the bytes for `<`
    // scanned the wrong thing if an escape it did not model came earlier.
    ExpectEqString(Decode("\x1B$B" "F|K\\" "\x1B(B", Encoding::Iso2022Jp), "日本",
                   "an escape into jis0208 and back");
    ExpectEqString(Decode("\x1B(J" "\x5C\x7E" "\x1B(B", Encoding::Iso2022Jp), "¥‾",
                   "Roman's two exceptions are the yen sign and the overline");
    // The katakana set is *halfwidth* -- `0xFF61 - 0x21 + byte` -- which is the one place this
    // decoder's arithmetic looks like a typo and is not.
    ExpectEqString(Decode("\x1B(I" "\x21", Encoding::Iso2022Jp), "｡",
                   "and the katakana set, which is the halfwidth one");
    // A partial escape sequence is *restored* to the input and decoded as text rather than swallowed.
    // Swallowing it is how a decoder deletes the character a filter was looking for.
    ExpectEqString(Decode("\x1B(X<", Encoding::Iso2022Jp), kReplacement + "(X<",
                   "an escape that is not one gives back both its bytes");
    // An escape to the state already in force is an error rather than a no-op, which is the
    // standard's `ISO-2022-JP output` flag. A document that writes one is hiding something in what
    // looks like redundancy.
    ExpectEqString(Decode("\x1B(B\x1B(B" "A", Encoding::Iso2022Jp), kReplacement + "A",
                   "a redundant escape is an error");
    // Concatenating two encoder outputs is not always valid: the second `ESC ( J` arrives while
    // `ISO-2022-JP output` is still set from the first stream's return to ASCII.
    const std::string yen = "\x1B(J" "\x5C" "\x1B(B";
    ExpectEqString(Decode(yen + yen, Encoding::Iso2022Jp), "¥" + kReplacement + "¥",
                   "two yen encodings concatenated");
    std::string fatal_out;
    Expect(!html::DecodeBytes(yen + yen, Encoding::Iso2022Jp, fatal_out, true),
           "and fatal refuses that concatenation");
    ExpectEqString(Decode("\x1B$", Encoding::Iso2022Jp), kReplacement + "$",
                   "a truncated escape restores the byte after ESC");
    std::string leftover;
    std::string streamed;
    Expect(html::DecodeBytesStreaming(leftover, "\x7E", Encoding::Iso2022Jp, streamed, false, true),
           "ASCII tilde");
    ExpectEqString(streamed, "~", "in ASCII");
    Expect(!html::DecodeBytesStreaming(leftover, "\x1B(J\xFF", Encoding::Iso2022Jp, streamed, true,
                                       true),
           "streamed Roman then a bad byte is fatal");
    Expect(html::DecodeBytesStreaming(leftover, "\x7E", Encoding::Iso2022Jp, streamed, false, true),
           "and the next decode is still Roman");
    ExpectEqString(streamed, "‾", "so 0x7E is the overline");
  });

  AddTest(tests, "Encoding/TheEncodersAreNotTheDecodersBackwards", [] {
    // Every one of these is a rule the standard states separately for one encoding, and every one of
    // them produces *plausible wrong bytes* if it is left out -- bytes that decode back to the right
    // character and are still not what any other browser sends.
    const auto encode = [](std::string_view text, Encoding encoding) {
      return EncodeWithNumericEscapes(text, encoding);
    };
    // Shift_JIS drops index pointers 8272-8835 before taking the first match, so a code point the
    // index lists twice encodes to the later of the two.
    ExpectEqString(encode("日本語", Encoding::ShiftJis), "\x93\xFA\x96\x7B\x8C\xEA", "Shift_JIS");
    ExpectEqString(encode("¥", Encoding::ShiftJis), "\x5C",
                   "and the yen sign is a backslash, which is why it cannot round-trip");
    ExpectEqString(encode("日本語", Encoding::EucJp), "\xC6\xFC\xCB\xDC\xB8\xEC", "EUC-JP");
    ExpectEqString(encode("한국말", Encoding::EucKr), "\xC7\xD1\xB1\xB9\xB8\xBB", "EUC-KR");
    // Big5 drops the Hong Kong supplement so it is never produced literally, and takes the *last*
    // pointer for six code points rather than the first.
    ExpectEqString(encode("正體字", Encoding::Big5), "\xA5\xBF\xC5\xE9\xA6\x72", "Big5");
    ExpectEqString(encode("十", Encoding::Big5), "\xA4\x51", "Big5's last-pointer exception");
    ExpectEqString(encode("中文", Encoding::Gb18030), "\xD6\xD0\xCE\xC4", "GB18030");
    // The difference between the two Chinese encoders, in one character: GBK has no four-byte form,
    // so a character outside its two-byte space is an error where GB18030 has bytes for it.
    ExpectEqString(encode("💩", Encoding::Gb18030), "\x94\x39\xDA\x33", "GB18030 reaches U+1F4A9");
    ExpectEqString(encode("💩", Encoding::Gbk), "&#128169;", "GBK does not, and says so");
    // The GB18030-2022 side table: eighteen private-use code points that do not encode where the
    // index says, so that bytes in deployed documents keep meaning what they used to.
    ExpectEqString(encode("\xEE\x9E\x8D", Encoding::Gbk), "\xA6\xD9", "U+E78D through the side table");
    // ISO-2022-JP writes an escape when it changes character set and another at the end of the
    // stream. A stream that ended in the jis0208 state would decode wrong the moment anything was
    // concatenated after it.
    ExpectEqString(encode("日本", Encoding::Iso2022Jp), "\x1B$B" "F|K\\" "\x1B(B",
                   "an escape in, and one back out at the end");
    ExpectEqString(encode("A日B", Encoding::Iso2022Jp), "A\x1B$B" "F|" "\x1B(B" "B",
                   "and one each way around a kanji run");
    // The escape back to ASCII comes *before* the numeric escape, not after: nine ASCII bytes inside
    // a jis0208 run would be read back as kanji.
    // Ж would not do here: JIS X 0208 has the Cyrillic alphabet in it, which is the kind of thing
    // that makes a hand-written expectation wrong in a way only the index can settle.
    ExpectEqString(encode("日한", Encoding::Iso2022Jp), "\x1B$B" "F|" "\x1B(B" "&#54620;",
                   "an unencodable character leaves the shift state first");
    // A code point no encoding here has is the same escape everywhere, and it is the caller's
    // spelling rather than the encoder's -- see Encoding.h.
    ExpectEqString(encode("한", Encoding::ShiftJis), "&#54620;", "an unencodable code point");
    // A lone surrogate is not a character. U+FFFD is what the conversion to a scalar value string
    // produces, and `&#55357;` -- a reference to something that cannot exist -- is what writing the
    // surrogate through would produce instead.
    ExpectEqString(encode("\xED\xA0\xBD", Encoding::EucJp), "&#65533;",
                   "a lone surrogate is U+FFFD before the encoder sees it");
  });

  AddTest(tests, "Encoding/TheAwkwardCornersOfTheLegacyIndexes", [] {
    // Five things that are each a decoder's own exception, and each one a place a from-scratch
    // implementation goes wrong quietly.
    //
    // Big5 has four pointers that decode to *two* code points -- a vowel plus a combining tone mark.
    // A decoder that emitted only the base would drop the tone from Taiwanese Mandarin transcription.
    ExpectEqString(Decode("\x88\x62", Encoding::Big5), "Ê\xCC\x84", "Big5's two-code-point pointer");
    // 0x88A3 rather than 0x88A5, and the two expectations this replaced were both mine and both
    // wrong: pointer 1166 is the *caron* pair and 1164 is the macron one. Written down because getting
    // one of these four backwards is invisible without a reference.
    ExpectEqString(Decode("\x88\xA3", Encoding::Big5), "ê\xCC\x84", "and its lowercase pair");
    // GB18030's single-byte euro, which no other encoding here has.
    ExpectEqString(Decode("\x80", Encoding::Gb18030), "€", "0x80 is the euro sign");
    // Shift_JIS's 0x80 is U+0080 -- and it is *encoded* rather than passed through. Pushing the raw
    // byte emits a lone continuation byte, which is ill-formed UTF-8 out of a decoder whose entire
    // job is to produce UTF-8. The differential sweep in this session found exactly that.
    ExpectEqString(Decode("\x80", Encoding::ShiftJis), "\xC2\x80", "and Shift_JIS's is U+0080");
    // Shift_JIS's extension area is a private-use code point rather than an index entry, which is what
    // keeps a vendor character from becoming U+FFFD.
    Expect(Decode("\xF0\x40", Encoding::ShiftJis) != kReplacement,
           "the extension area decodes to private use");
    // And GB18030's index disagrees with the vendor table in twenty places: pointer 6555 is an
    // ideographic space in the standard's index and a private-use character in cp936. The standard is
    // what a page was authored against in a browser.
    ExpectEqString(Decode("\xA3\xA0", Encoding::Gb18030), "\xE3\x80\x80",
                   "the index wins over the vendor table");
  });

  AddTest(tests, "Encoding/ADeclaredLegacyEncodingIsSniffedAndDecoded", [] {
    // End to end, which is the only assertion here that exercises the *wiring*: a label in a
    // `Content-Type`, a label in a `<meta>`, and the bytes decoded accordingly.
    Expect(SniffEncoding("<html>", "text/html; charset=Shift_JIS") == Encoding::ShiftJis,
           "from the header");
    Expect(SniffEncoding("<meta charset=big5>") == Encoding::Big5, "and from the document");
    ExpectEqString(DecodeToUtf8("\x93\xFA", SniffEncoding("<meta charset=shift_jis>")), "日",
                   "and DecodeToUtf8 dispatches to it");
  });

  AddTest(tests, "Encoding/DecodeBytesKeepsABomThatDecodeToUtf8Strips", [] {
    // TextDecoder's ignoreBOM:true needs the bytes themselves. Document loading
    // still uses DecodeToUtf8, which skips a leading BOM so the tokenizer never
    // sees one.
    const std::string utf8_bom_a = std::string("\xEF\xBB\xBF", 3) + "a";
    ExpectEqString(DecodeToUtf8(utf8_bom_a, Encoding::Utf8), "a", "document path skips the BOM");
    ExpectEqString(html::DecodeBytes(utf8_bom_a, Encoding::Utf8),
                   std::string("\xEF\xBB\xBF", 3) + "a", "TextDecoder path keeps it");
  });

  AddTest(tests, "Encoding/StreamingHoldsAnIncompleteUtf8PrefixAndFlushesItAsOneReplacement", [] {
    // Encoding Standard: a valid incomplete prefix is held across stream:true
    // chunks and becomes one U+FFFD at flush -- not one per leftover byte.
    std::string leftover;
    std::string out;
    Expect(html::DecodeBytesStreaming(leftover, std::string("\xF0\x9F\x92", 3), Encoding::Utf8, out,
                                      true, false),
           "a three-byte prefix of a four-byte sequence is not an error");
    Expect(out.empty() && leftover.size() == 3,
           "and it emits nothing while those bytes are held");
    Expect(html::DecodeBytesStreaming(leftover, std::string("\xA9", 1), Encoding::Utf8, out, false,
                                      false),
           "the fourth byte completes it");
    ExpectEqString(out, "\xF0\x9F\x92\xA9", "U+1F4A9");

    leftover.clear();
    Expect(html::DecodeBytesStreaming(leftover, std::string("\xF0", 1), Encoding::Utf8, out, true,
                                      false),
           "a lone lead is held");
    Expect(out.empty() && leftover.size() == 1, "and emits nothing");
    Expect(html::DecodeBytesStreaming(leftover, {}, Encoding::Utf8, out, false, false),
           "flushing it is not a failure");
    ExpectEqString(out, kReplacement, "one replacement for the incomplete sequence");

    leftover.clear();
    Expect(html::DecodeBytesStreaming(leftover, std::string("\xC1", 1), Encoding::Utf8, out, true,
                                      false),
           "0xC1 is never a lead");
    ExpectEqString(out, kReplacement, "so it is U+FFFD immediately, even when streaming");
    Expect(leftover.empty(), "and nothing is held");

    leftover.clear();
    Expect(html::DecodeBytesStreaming(leftover, std::string("\x93", 1), Encoding::ShiftJis, out, true,
                                      false),
           "a Shift_JIS lead is held");
    Expect(out.empty() && leftover.size() == 1, "while streaming");
    Expect(html::DecodeBytesStreaming(leftover, std::string("\xFA", 1), Encoding::ShiftJis, out, false,
                                      false),
           "and completes");
    ExpectEqString(out, "日", "as 日");
  });
}

}  // namespace microbrowser::tests

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
    // 0x81 is one of the five unassigned windows-1252 bytes, and it is U+FFFD rather than passed
    // through -- the table says so.
    ExpectEqString(Decode("\x81", Encoding::Windows1252), kReplacement, "an unassigned byte");
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
    // `shift_jis` and `gb18030` were the examples here until session 32 built them, so the examples
    // are now labels this browser still lacks -- ISO-2022-JP, which is stateful, and the standard's
    // own `replacement` encoding. **The two changed assertions are the session's own change**, and
    // they are the whole reason the test still means something: an example that has become supported
    // proves nothing about the fall-through.
    Expect(!EncodingFromLabel("iso-2022-jp").has_value(), "an encoding this browser lacks is nothing");
    Expect(!EncodingFromLabel("hz-gb-2312").has_value(), "and so is another");
    Expect(!EncodingFromLabel("").has_value(), "and an empty label");
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
    // GBK and GB2312 decode *as* GB18030, which is what the standard says: it is a superset, so a GBK
    // document decoded with it produces the same characters rather than an approximation.
    Expect(EncodingFromLabel("gbk") == Encoding::Gb18030, "gbk is gb18030");
    Expect(EncodingFromLabel("gb2312") == Encoding::Gb18030, "and so is gb2312");
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
    // The 0x8E prefix with a byte that is not katakana, and 0x8F -- JIS X 0212, which this browser has
    // no index for and therefore refuses. Both consume one byte, not two or three.
    ExpectEqString(Decode("\x8E\x41", Encoding::EucJp), kReplacement + "A", "0x8E then ASCII");
    ExpectEqString(Decode("\x8F\x3C\x3C", Encoding::EucJp), kReplacement + "<<",
                   "0x8F whose trail bytes are not trail bytes keeps both");
    // GB18030's four-byte form, refused -- and the *shape* is checked before four bytes are consumed.
    // `81 30 3C 3C` is not a four-byte sequence, so only the 0x81 is eaten.
    ExpectEqString(Decode("\x81\x30\x3C\x3C", Encoding::Gb18030), kReplacement + "0<<",
                   "a malformed four-byte sequence does not eat the text after it");
    ExpectEqString(Decode("\x81\x30\x81\x30", Encoding::Gb18030), kReplacement,
                   "a well-formed one is one refusal rather than four characters");
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
}

}  // namespace microbrowser::tests

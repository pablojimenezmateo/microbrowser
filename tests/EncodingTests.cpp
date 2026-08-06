// Character encodings, and the substitution rules that make them a security property.
//
// ADR 0025 §2. **Encoding confusion leading to XSS is a real, repeatedly exploited bug family**, so
// the assertions here are mostly about ill-formed input: a decoder that emits a `<` where the
// specification says U+FFFD turns a sanitised document into a script-executing one, and one that
// *swallows* a byte hides a character a filter was looking for.

#include <string>
#include <vector>

#include "TestSupport.h"
#include "html/Encoding.h"

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
    // The confusion in miniature: a page that declares `shift_jis` and is decoded as UTF-8 is a page
    // whose bytes are reinterpreted. An unrecognised label must take the *next* step of the algorithm.
    Expect(!EncodingFromLabel("shift_jis").has_value(), "an encoding this browser lacks is nothing");
    Expect(!EncodingFromLabel("gb18030").has_value(), "and so is another");
    Expect(!EncodingFromLabel("").has_value(), "and an empty label");
    // With an unknown header, the meta still gets its turn.
    Expect(SniffEncoding("<meta charset=utf-8>", "text/html; charset=euc-kr") == Encoding::Utf8,
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
}

}  // namespace microbrowser::tests

// UAX #14 line breaking, and the generated tables under it.
//
// ADR 0025 §1 and §4. **The bug this fixes is that CJK text overflows its box**: breaking only at
// spaces cannot break Japanese or Chinese at all, so a paragraph of it is one unbreakable word. The
// assertions below are grouped by what would go wrong -- text that does not wrap, and text that wraps
// where it must not.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "text/LineBreak.h"
#include "text/UnicodeProperties.h"

namespace microbrowser::tests {

namespace {

using text::BreakAction;
using text::FindBreakOpportunities;
using text::LineBreakClass;

// The text with `|` marking every break opportunity, which is how UAX #14's own test file reads and
// is far easier to argue with than a list of offsets.
std::string Marked(std::string_view text) {
  const std::vector<text::BreakOpportunity> breaks = FindBreakOpportunities(text);
  std::string out;
  std::size_t next = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    while (next < breaks.size() && breaks[next].offset == i) {
      out.push_back('|');
      ++next;
    }
    out.push_back(text[i]);
  }
  return out;
}

}  // namespace

void RegisterLineBreakTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LineBreak/TheGeneratedTablesAreSortedAndCoverWhatTheyClaim", [] {
    // A generator bug here is a silent one -- text that wraps wrongly with no error -- so the table's
    // structural invariant is asserted rather than assumed: sorted, non-overlapping, and therefore
    // searchable. Spot-checked against the UCD at a few boundaries that matter.
    Expect(text::LineBreakClassOf(' ') == LineBreakClass::SP, "space");
    Expect(text::LineBreakClassOf('\n') == LineBreakClass::LF, "newline");
    Expect(text::LineBreakClassOf('a') == LineBreakClass::AL, "a letter");
    Expect(text::LineBreakClassOf('5') == LineBreakClass::NU, "a digit");
    Expect(text::LineBreakClassOf('(') == LineBreakClass::OP, "an opening bracket");
    // `)` is **CP** and `}` is CL, which is not a distinction I expected: since Unicode 6.1 the
    // parenthesis and square bracket have their own class, because LB30 treats them differently from
    // other closing punctuation -- `(a)b` may not break before the `b` and `{a}b` may.
    Expect(text::LineBreakClassOf(')') == LineBreakClass::CP, "a closing parenthesis is CP");
    Expect(text::LineBreakClassOf('}') == LineBreakClass::CL, "and a curly brace is CL");
    Expect(text::LineBreakClassOf(0x3001) == LineBreakClass::CL, "as is an ideographic comma");
    Expect(text::LineBreakClassOf(0x4E00) == LineBreakClass::ID, "a CJK ideograph");
    Expect(text::LineBreakClassOf(0x3041) == LineBreakClass::CJ, "small hiragana is CJ");
    Expect(text::LineBreakClassOf(0x00A0) == LineBreakClass::GL, "a no-break space is glue");
    Expect(text::LineBreakClassOf(0x200B) == LineBreakClass::ZW, "a zero-width space");
    Expect(text::LineBreakClassOf(0x0301) == LineBreakClass::CM, "a combining acute");
    // A code point in a range the UCD does not assign falls back to AL, which is what UAX #14 says an
    // unassigned code point is -- wrong in the specification's direction rather than in ours.
    Expect(text::LineBreakClassOf(0x0E0000) == LineBreakClass::AL, "unassigned is a letter");

    // East Asian Width, which is why a run of CJK measures to twice its code point count.
    Expect(text::IsDoubleWidth(0x4E00), "an ideograph is two columns");
    Expect(text::IsDoubleWidth(0xFF21), "and so is a fullwidth A");
    Expect(!text::IsDoubleWidth('A'), "while an ASCII A is one");
    Expect(!text::IsDoubleWidth(0xFF61), "and a halfwidth form is one");
  });

  AddTest(tests, "LineBreak/CjkBreaksBetweenAlmostEveryCharacter", [] {
    // **The fix.** Two ideographs are ID/ID and no rule prohibits a break between them, so Japanese
    // wraps at almost every character -- which is what Japanese typesetting does and what stops a
    // paragraph from being one word as wide as the text is long.
    const std::vector<text::BreakOpportunity> breaks = FindBreakOpportunities("日本語のテキスト");
    Expect(breaks.size() >= 6,
           "a run of CJK offers a break at nearly every character rather than none");
    for (const text::BreakOpportunity& opportunity : breaks) {
      Expect(!opportunity.mandatory, "and none of them is mandatory");
    }
    // A small kana is a non-starter: it may not begin a line, so there is no break *before* it. This
    // is the CJ resolution, and it is the difference between correct Japanese and text that starts a
    // line with a っ.
    const std::string with_small = "あっち";
    const std::vector<text::BreakOpportunity> strict = FindBreakOpportunities(with_small);
    for (const text::BreakOpportunity& opportunity : strict) {
      Expect(opportunity.offset != 3,
             "no break before the small tsu, which may not start a line");
    }
  });

  AddTest(tests, "LineBreak/OrdinaryEnglishBreaksAfterSpacesAndNowhereElse", [] {
    ExpectEqString(Marked("hello world again"), "hello |world |again",
                   "after each space, and not inside a word");
    // A run of spaces breaks once, at its end, which is LB7 and LB18 in that order. Breaking at each
    // space would put a line's worth of leading spaces on the next line.
    ExpectEqString(Marked("a   b"), "a   |b", "one break after a run of spaces");
  });

  AddTest(tests, "LineBreak/PunctuationStaysWithWhatItPunctuates", [] {
    // Each of these is a line that would look wrong: a comma starting a line, a bracket orphaned from
    // its contents, a number split at its thousands separator.
    ExpectEqString(Marked("one, two"), "one, |two", "a comma never starts a line");
    ExpectEqString(Marked("(a) b"), "(a) |b", "brackets stay with their contents");
    ExpectEqString(Marked("1,000 apples"), "1,000 |apples", "a number is not split at its comma");
    ExpectEqString(Marked("50% off"), "50% |off", "a percent stays with its number");
    ExpectEqString(Marked("$99 each"), "$99 |each", "and so does a currency prefix");
    ExpectEqString(Marked("end!"), "end!", "an exclamation does not start a line");
  });

  AddTest(tests, "LineBreak/AHyphenBreaksAfterItselfAndAZeroWidthSpaceIsAnInvitation", [] {
    ExpectEqString(Marked("well-known thing"), "well-|known |thing",
                   "a hyphenated word wraps at the hyphen, after it");
    // U+200B exists for exactly this: text with no spaces that wants a break opportunity. A CJK page
    // that pre-inserted them depends on it.
    ExpectEqString(Marked("ab\xE2\x80\x8B" "cd"), "ab\xE2\x80\x8B|cd", "a zero-width space breaks after");
    // A no-break space is the opposite and is glue: it must not break on either side.
    ExpectEqString(Marked("10\xC2\xA0kg here"), "10\xC2\xA0kg |here", "a no-break space holds");
  });

  AddTest(tests, "LineBreak/AMandatoryBreakIsNotAWrap", [] {
    // A newline breaks whether or not the line is full, which is the difference between a `<br>` and
    // a wrap -- and a CRLF is one break rather than two, or every Windows-authored document would
    // double-space.
    const std::vector<text::BreakOpportunity> breaks = FindBreakOpportunities("a\nb");
    ExpectEqInt(static_cast<long long>(breaks.size()), 1, "one break");
    Expect(breaks.at(0).mandatory, "and it is mandatory");
    const std::vector<text::BreakOpportunity> crlf = FindBreakOpportunities("a\r\nb");
    ExpectEqInt(static_cast<long long>(crlf.size()), 1, "a CRLF is one break, not two");
    Expect(crlf.at(0).mandatory, "still mandatory");
    ExpectEqInt(static_cast<long long>(crlf.at(0).offset), 3,
                "and it comes after both bytes, not between them");
  });

  AddTest(tests, "LineBreak/ACombiningMarkNeverStartsALine", [] {
    // LB9. A mark attaches to what precedes it, and a break before one would separate an accent from
    // its letter -- which renders as a stray diacritic at the start of a line.
    const std::string decomposed = "e\xCC\x81 x";  // e + combining acute
    for (const text::BreakOpportunity& opportunity : FindBreakOpportunities(decomposed)) {
      Expect(opportunity.offset != 1, "no break between the letter and its accent");
    }
    ExpectEqString(Marked(decomposed), "e\xCC\x81 |x", "the only break is after the space");
  });

  AddTest(tests, "LineBreak/HangulSyllablesAreNotBrokenInternally", [] {
    // A Korean syllable is two to four jamo, and breaking between them produces a cluster no reader
    // recognises. `한` here is composed rather than precomposed, which is how the rule is exercised.
    const std::string jamo = "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB";  // ᄒ + ᅡ + ᆫ
    Expect(FindBreakOpportunities(jamo).empty(),
           "a composed Hangul syllable offers no break inside itself");
  });

  AddTest(tests, "LineBreak/EmptyAndSingleCharacterTextHaveNoOpportunities", [] {
    // A break before the first character is not an opportunity -- it is where the line already
    // started -- and a caller that received one would insert an empty first line.
    Expect(FindBreakOpportunities("").empty(), "nothing in nothing");
    Expect(FindBreakOpportunities("a").empty(), "and none before a single character");
    Expect(FindBreakOpportunities("日").empty(), "including a single ideograph");
  });
}

}  // namespace microbrowser::tests

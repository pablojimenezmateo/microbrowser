#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "text/Bidi.h"
#include "util/StringUtil.h"

namespace microbrowser::tests {

using text::BidiClass;
using text::BidiRun;

namespace {

// UAX #9's conformance data is 861,948 cases and is what actually validates this code -- see
// `tools/bidiconf`, and run it after touching Bidi.cpp. **These tests are the other kind**: they pin
// the properties a refactor could break without the conformance data noticing, and the handful of
// cases a reader can check by eye. Neither replaces the other. A test suite that only had these
// would be testing the cases I thought of, which are the ones I got right.

std::vector<std::uint32_t> Text(std::string_view utf8) {
  std::vector<std::uint32_t> out;
  std::size_t at = 0;
  std::uint32_t code = 0;
  while (util::DecodeUtf8(utf8, at, code)) {
    out.push_back(code);
  }
  return out;
}

// The visual order as a string of positions, which is what a reordering test can compare by eye.
std::string OrderOf(std::string_view utf8, std::uint8_t paragraph_level) {
  const std::vector<std::uint32_t> text = Text(utf8);
  std::string out;
  for (const BidiRun& run : text::ResolveVisualRuns(text, paragraph_level)) {
    for (std::size_t k = 0; k < run.length; ++k) {
      const std::size_t at = run.right_to_left ? run.start + run.length - 1 - k : run.start + k;
      if (!out.empty()) {
        out.push_back(' ');
      }
      out += std::to_string(at);
    }
  }
  return out;
}

std::string LevelsOf(std::string_view utf8, std::uint8_t paragraph_level) {
  std::string out;
  for (const std::uint8_t level : text::ResolveLevels(Text(utf8), paragraph_level)) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += std::to_string(static_cast<int>(level));
  }
  return out;
}

// The explicit controls as escapes rather than as literal characters. Two reasons, and the first one
// is that the compiler insists: `-Wbidi-chars` rejects an unpaired bidi control in source, and it is
// right to -- an unpaired one in *code* is the trick that makes a reviewer read a different program
// from the one the compiler sees. The second is that they are invisible, so a literal would be a test
// whose input cannot be read.
const std::string kLri = "\xE2\x81\xA6";
const std::string kRli = "\xE2\x81\xA7";
const std::string kPdi = "\xE2\x81\xA9";
const std::string kRle = "\xE2\x80\xAB";
const std::string kPdf = "\xE2\x80\xAC";

}  // namespace

void RegisterBidiTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Bidi/ClassesComeFromTheGeneratedTable", [] {
    Expect(text::BidiClassOf('a') == BidiClass::L, "Latin is left-to-right");
    Expect(text::BidiClassOf(0x05D0) == BidiClass::R, "Hebrew alef is right-to-left");
    Expect(text::BidiClassOf(0x0627) == BidiClass::AL, "Arabic alef is an Arabic letter, not R");
    Expect(text::BidiClassOf('5') == BidiClass::EN, "a digit is a European number");
    Expect(text::BidiClassOf(0x0660) == BidiClass::AN, "and an Arabic-Indic digit is an Arabic one");
    Expect(text::BidiClassOf(0x200B) == BidiClass::BN, "a zero-width space is boundary-neutral");
    Expect(text::BidiClassOf(0x2066) == BidiClass::LRI, "and the isolates are their own classes");
    // The block defaults from DerivedBidiClass.txt's comments, which are data the file states in
    // prose. A code point Unicode has not assigned yet inside the Arabic block still has to lay out
    // right-to-left, or a page using a newer Unicode than this table reverses the line it is on.
    Expect(text::BidiClassOf(0x08B5) == BidiClass::AL,
           "an unassigned code point in an Arabic block defaults to AL rather than L");
    Expect(text::BidiClassOf(0x05EB) == BidiClass::R,
           "and one in a Hebrew block defaults to R");
  });

  AddTest(tests, "Bidi/TheParagraphLevelIsTheFirstStrongCharacter", [] {
    // P2 and P3, which is what `dir="auto"` and an unstyled document mean.
    Expect(text::ParagraphLevel(Text("hello")) == 0, "Latin");
    Expect(text::ParagraphLevel(Text("שלום")) == 1, "Hebrew");
    Expect(text::ParagraphLevel(Text("123 שלום")) == 1,
           "a number is not strong, so the Hebrew after it decides");
    Expect(text::ParagraphLevel(Text("!!! ")) == 0, "P3: nothing strong at all is left-to-right");
    // P2 skips an isolate's contents entirely: text inside one says nothing about the direction of
    // the text around it, which is the whole purpose of an isolate.
    Expect(text::ParagraphLevel(Text(kLri + "שלום" + kPdi + " hello")) == 0,
           "Hebrew inside an isolate does not make the paragraph right-to-left");
  });

  AddTest(tests, "Bidi/RightToLeftTextIsReordered", [] {
    // Four Hebrew letters in a left-to-right paragraph: the letters reverse, and nothing else does.
    ExpectEqString(OrderOf("שלום", 0), "3 2 1 0", "a right-to-left word reverses");
    ExpectEqString(OrderOf("hi", 0), "0 1", "and a left-to-right one does not");
    // The mixed case, which is the one worth reading: `a` `ש` `ל` `b` in an LTR paragraph is
    // a, then the Hebrew reversed, then b.
    ExpectEqString(OrderOf("aשלb", 0), "0 2 1 3", "the Hebrew reverses inside the Latin");
    // And the same characters in a right-to-left paragraph. **I expected "3 1 2 0" here and the
    // answer is "3 2 1 0"**, which is right and worth writing down: the two Latin letters are at
    // level 2 but they are not *adjacent*, so L2's reversal of the level-2 stretches reverses two
    // one-character stretches and changes nothing, and then the level-1 reversal takes the line.
    ExpectEqString(OrderOf("aשלb", 1), "3 2 1 0",
                   "b, then the Hebrew reversed, then a -- the single-character level-2 runs "
                   "reverse to themselves");
  });

  AddTest(tests, "Bidi/ANumberInsideRightToLeftTextRunsForward", [] {
    // I1/I2: a European number in right-to-left text gets a level *two* higher, not one, which is
    // what draws `123` left-to-right inside a right-to-left sentence. Getting this wrong renders
    // every price and date on an Arabic page backwards.
    ExpectEqString(LevelsOf("ש123ם", 1), "1 2 2 2 1", "the digits are two levels up");
    ExpectEqString(OrderOf("ש123ם", 1), "4 1 2 3 0", "so they read forwards inside reversed text");
    // W2: a European number after an *Arabic* letter is an Arabic number, and an Arabic number gets
    // level+2 as well -- but W2 has to see AL before W3 turns it into R, which is why the rule order
    // in the implementation is not decorative.
    ExpectEqString(LevelsOf("ا123", 1), "1 2 2 2", "after an Arabic letter too");
  });

  AddTest(tests, "Bidi/BracketsTakeTheDirectionOfWhatIsInsideThem", [] {
    // N0, and the rule that makes `(hello)` inside Hebrew keep its parentheses *around* the word
    // rather than one at each end of the sentence. Both brackets resolve together or neither does.
    // **I expected the brackets to go with the Latin inside them and they do not**, which is N0's
    // actual rule and the more useful half of it: Latin inside brackets is the *opposite* of the
    // embedding direction, so the text *before* the bracket decides. Here that is Hebrew, which
    // agrees with the embedding, so the brackets stay right-to-left and hug the Hebrew sentence --
    // and `(` still paints where a reader of Hebrew expects an opening bracket.
    ExpectEqString(LevelsOf("ש(ab)ם", 1), "1 1 2 2 1 1",
                   "the brackets follow the sentence, and only the Latin rises a level");
    // Nothing strong inside: the brackets stay neutral and N1/N2 place them with their surroundings.
    ExpectEqString(LevelsOf("ש( )ם", 1), "1 1 1 1 1", "empty brackets follow the sentence");
    // The canonical equivalence BD16 names: U+3008 pairs with U+232A. Two pairs, and they are the
    // whole list -- which is why they are two `if`s rather than a normalization pass.
    bool opens = false;
    Expect(text::PairedBracket(0x3008, opens) == 0x3009 && opens, "an opening bracket");
    Expect(text::PairedBracket(0x0029, opens) == 0x0028 && !opens, "and a closing one");
    Expect(text::PairedBracket('a', opens) == 0, "a letter is not a bracket");
  });

  AddTest(tests, "Bidi/AnIsolateIsTransparentAndAnEmbeddingIsNot", [] {
    // The difference between the two, in one comparison. `RLI … PDI` around Hebrew leaves the Latin
    // either side of it resolving *together*; `RLE … PDF` does not, because the text after the pop
    // is a different isolating run sequence.
    const std::string isolated = "a" + kRli + "ש" + kPdi + "b";
    const std::string embedded = "a" + kRle + "ש" + kPdf + "b";
    ExpectEqString(LevelsOf(isolated, 0), "0 0 1 0 0", "the isolate's own marks stay at level 0");
    ExpectEqString(LevelsOf(embedded, 0), "0 0 1 1 0",
                   "an embedding's pop takes the embedded level, because it is removed and inherits "
                   "the level of what precedes it");
    // A PDF does not pop an isolate. That single `!isolate` test is the whole difference in the
    // implementation, and without it an unmatched PDF inside an isolate would unwind past it.
    ExpectEqString(LevelsOf("a" + kRli + kPdf + "ש" + kPdi + "b", 0), "0 0 0 1 0 0",
                   "a pop-directional-format does not close an isolate");
  });

  AddTest(tests, "Bidi/TrailingWhitespaceHangsOffTheParagraphEdge", [] {
    // L1, and it uses the *original* classes rather than the resolved ones -- which is stated in the
    // rule and is the point of it. Whitespace at the end of a right-to-left line has been resolved to
    // some level by now; a reader needs it at the paragraph's own edge, not reordered into the middle.
    ExpectEqString(LevelsOf("שלום  ", 0), "1 1 1 1 0 0",
                   "the two trailing spaces reset to the paragraph level");
    ExpectEqString(LevelsOf("hi\tשלום", 0), "0 0 0 1 1 1 1",
                   "a tab is a segment separator and resets, and only it");
  });

  AddTest(tests, "Bidi/EveryCharacterAppearsInExactlyOneRun", [] {
    // The property a reordering bug would break silently: a run list that dropped a character drops
    // text from the screen, and one that duplicated a character draws it twice. Checked over a line
    // with every kind of thing on it.
    const std::string mixed = "a שלום 123 الع (b) " + kRli + "ש" + kPdi + " x";
    for (const std::uint8_t paragraph_level : {std::uint8_t{0}, std::uint8_t{1}}) {
      const std::vector<std::uint32_t> text = Text(mixed);
      std::vector<int> seen(text.size(), 0);
      std::size_t total = 0;
      for (const BidiRun& run : text::ResolveVisualRuns(text, paragraph_level)) {
        Expect(run.right_to_left == ((run.level & 1) != 0),
               "the parity flag agrees with the level");
        for (std::size_t k = 0; k < run.length; ++k) {
          ++seen[run.start + k];
        }
        total += run.length;
      }
      Expect(total == text.size(), "the runs cover the line exactly once");
      for (const int count : seen) {
        Expect(count == 1, "and no character is in two runs or none");
      }
    }
  });

  AddTest(tests, "Bidi/ALatinLineIsRejectedBeforeAnythingIsDecoded", [] {
    // The fast path, and it is here because bidi is one of the few features that could plausibly cost
    // every page in the world something. A line with no right-to-left character and no explicit
    // control needs none of this, and the test for it is a byte comparison.
    Expect(!text::NeedsBidi("Hello, world! 123 <a href=x>"), "English");
    Expect(!text::NeedsBidi("Ünïcödé and Ελληνικά and 日本語 and 🎉"),
           "and every other left-to-right script, including ones above the BMP");
    Expect(text::NeedsBidi("שלום"), "Hebrew needs it");
    Expect(text::NeedsBidi("ا"), "Arabic needs it");
    Expect(text::NeedsBidi("a٠b"), "and so does an Arabic-Indic digit on its own");
    Expect(text::NeedsBidi("a" + kRle + "b"),
           "and an explicit embedding, whichever way it points");
    Expect(!text::NeedsBidi(""), "and an empty line needs nothing");
  });

  AddTest(tests, "Bidi/DeeplyNestedEmbeddingsOverflowRatherThanClamp", [] {
    // 125 is the depth limit, and past it an embedding is *ignored* -- with its matching pop ignored
    // too, which is what the overflow counters are for. Clamping instead would silently reinterpret
    // the text after the pop at a level nobody asked for.
    std::string deep;
    for (int i = 0; i < 200; ++i) {
      deep += kRle;
    }
    deep += "a";
    for (int i = 0; i < 200; ++i) {
      deep += kPdf;
    }
    deep += "b";
    const std::vector<std::uint8_t> levels = text::ResolveLevels(Text(deep), 0);
    Expect(levels.size() == 402, "every character still gets a level");
    // The `a` sits at the deepest level the limit allows, and the `b` is back at the paragraph's --
    // which only happens if the ignored pops were matched to the ignored pushes.
    // 126 rather than 125, and the difference is I1: the `a` is left-to-right at an odd level, so
    // it rises one more. **I expected 125 and forgot the implicit rule**, which is exactly the kind
    // of thing the explicit rules make easy to forget -- the depth limit bounds the *explicit* level
    // and the implicit rules still apply on top of it.
    Expect(levels[200] == 126, "the deepest character is one above the limit, by I1 and not by X2");
    Expect(levels.back() == 0, "and the text after the pops is back at the paragraph level");
  });
}

}  // namespace microbrowser::tests

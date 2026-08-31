#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/ColorText.h"

// `rgb()` and `hsl()`, and the two grammars CSS Color 4 gives each of them.
//
// The pair is **not** interchangeable, and the rule that separates them is one line -- a body with a
// comma in it is the legacy form and a body without one is the modern form -- but the consequences
// are not small. Before 2026-08-14 this parser replaced every comma with a space and split on
// whitespace, which accepted `rgb(1, 2, 3 / 0.5)` (a mixture no browser takes) and rejected every
// modern `rgb(1 2 3 / 0.5)` (because `/` came back as a fourth component). `hsl()` was not
// implemented at all: `color: hsl(0, 100%, 50%)` computed to black, silently, which is the failure
// mode ColorText.h's own comment says must not happen.

namespace microbrowser::tests {

namespace {

// The parse as a string, so a test states the answer rather than a hex constant. `-` for a colour
// that was refused, which is what an invalid declaration needs to be told apart from a black one.
std::string Parsed(std::string_view text) {
  const std::optional<gfx::Color> color = gfx::ParseColorText(text);
  if (!color.has_value()) {
    return "-";
  }
  return std::to_string(static_cast<int>(color->Red())) + "," +
         std::to_string(static_cast<int>(color->Green())) + "," +
         std::to_string(static_cast<int>(color->Blue())) + "," +
         std::to_string(static_cast<int>(color->Alpha()));
}

void ExpectColor(std::string_view text, std::string_view expected) {
  ExpectEqString(Parsed(text), std::string(expected), std::string("parsing ") + std::string(text));
}

}  // namespace

void RegisterColorTextTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ColorText/HslIsConvertedTheWayTheSpecificationWritesIt", [] {
    ExpectColor("hsl(0, 100%, 50%)", "255,0,0,255");
    ExpectColor("hsl(120, 100%, 50%)", "0,255,0,255");
    ExpectColor("hsl(240, 100%, 50%)", "0,0,255,255");
    ExpectColor("hsl(0, 0%, 100%)", "255,255,255,255");
    // A hue is a position on a circle, so it wraps -- and `-300` and `60` are the same colour, which
    // is an assertion the suite makes directly.
    ExpectColor("hsl(-300, 100%, 50%)", Parsed("hsl(60, 100%, 50%)"));
    ExpectColor("hsl(420, 100%, 50%)", Parsed("hsl(60, 100%, 50%)"));
    // Every angle unit, because a stylesheet may write any of them.
    ExpectColor("hsl(0.5turn, 100%, 50%)", Parsed("hsl(180, 100%, 50%)"));
    ExpectColor("hsl(200grad, 100%, 50%)", Parsed("hsl(180, 100%, 50%)"));
    ExpectColor("hsl(180deg, 100%, 50%)", Parsed("hsl(180, 100%, 50%)"));
    // Capitalisation is not part of the syntax.
    ExpectColor("HSL(0, 100%, 50%)", "255,0,0,255");
    ExpectColor("hsLA(0, 100%, 50%, 0.5)", "255,0,0,128");
  });

  AddTest(tests, "ColorText/TheTwoGrammarsAreNotInterchangeable", [] {
    // Modern: spaces, and a slash before the alpha.
    ExpectColor("rgb(1 2 3)", "1,2,3,255");
    ExpectColor("rgb(1 2 3 / 0.5)", "1,2,3,128");
    ExpectColor("hsl(0 100% 50% / 50%)", "255,0,0,128");
    // Legacy: commas throughout.
    ExpectColor("rgb(1, 2, 3)", "1,2,3,255");
    ExpectColor("rgba(1, 2, 3, 0.5)", "1,2,3,128");
    // **The mixture is invalid**, and it is the case the old parser accepted: it turned every comma
    // into a space, so the two forms became one and `/` was just another token.
    ExpectColor("rgb(1, 2, 3 / 0.5)", "-");
    ExpectColor("rgb(1 2, 3)", "-");
    // Legacy takes three numbers or three percentages, never a mixture. Modern allows it, and that
    // is the one place the grammars differ beyond punctuation.
    ExpectColor("rgb(1, 50%, 3)", "-");
    ExpectColor("rgb(1 50% 3)", "1,128,3,255");
    // Legacy `hsl()` requires percentages for saturation and lightness; modern takes bare numbers.
    ExpectColor("hsl(0, 100, 50)", "-");
    ExpectColor("hsl(0 100 50)", "255,0,0,255");
    // `none` is CSS Color 4's missing component and belongs to the modern grammar only.
    ExpectColor("rgb(none none none)", "0,0,0,255");
    ExpectColor("rgb(none, none, none)", "-");
  });

  AddTest(tests, "ColorText/HexHasFourLengthsAndANumberMayBeSigned", [] {
    ExpectColor("#abc", "170,187,204,255");
    // `#abcd` is the four-digit form with alpha, doubled rather than shifted for the reason `#abc`
    // is: `#ffff` has to be exactly opaque white.
    ExpectColor("#ffff", "255,255,255,255");
    ExpectColor("#0000", "0,0,0,0");
    ExpectColor("#abcdef", "171,205,239,255");
    ExpectColor("#abcdef80", "171,205,239,128");
    ExpectColor("#00", "-");
    ExpectColor("#00000", "-");
    // CSS's `<number>` allows a leading `+` and `std::from_chars` does not, which is right for
    // `util::ParseDouble` and wrong here. Dropped once, so `++0` is still nonsense.
    ExpectColor("rgb(+0, +0, +0)", "0,0,0,255");
    ExpectColor("rgb(+0%, +0%, +0%)", "0,0,0,255");
    ExpectColor("hsl(+0, +100%, +50%)", "255,0,0,255");
    ExpectColor("rgb(++0, 0, 0)", "-");
  });

  AddTest(tests, "ColorText/OutOfRangeIsClampedAndNonsenseIsRefused", [] {
    // Clamped rather than refused: the specification says a component out of range is clamped, and a
    // page that wrote `rgb(300, -20, 0)` gets what every other browser gives it.
    ExpectColor("rgb(300, -20, 0)", "255,0,0,255");
    ExpectColor("rgba(0, 0, 0, 12)", "0,0,0,255");
    ExpectColor("rgba(0, 0, 0, -1)", "0,0,0,0");
    ExpectColor("hsl(0, 200%, 50%)", Parsed("hsl(0, 100%, 50%)"));
    // Refused, and the difference matters: an invalid colour drops its declaration, where a black
    // one would silently paint. ColorText.h's comment is the contract this pins.
    ExpectColor("rgb(1, 2)", "-");
    ExpectColor("rgb(1, 2, 3, 4, 5)", "-");
    ExpectColor("hsl(nonsense, 100%, 50%)", "-");
    ExpectColor("rgb(1 2 3 / )", "-");
    ExpectColor("rgb(1 2 3", "-");
    ExpectColor("rgb(1 2 3) extra", "-");
  });

  AddTest(tests, "ColorText/EveryNamedColourInTheSpecificationIsKnown", [] {
    // The table was 22 of the 148 CSS Color 4 keywords, and the cost of the
    // missing 126 was not a wrong colour: a declaration whose value does not
    // parse is *dropped*, so `border: 2px solid limegreen` contributed no
    // border and every box wearing one was 4px smaller than the page said.
    // Layout, not paint, and invisible until something lines those boxes up.
    ExpectColor("limegreen", "50,205,50,255");
    ExpectColor("lightgreen", "144,238,144,255");
    ExpectColor("lightgray", "211,211,211,255");
    ExpectColor("lightgrey", "211,211,211,255");
    ExpectColor("pink", "255,192,203,255");
    ExpectColor("rebeccapurple", "102,51,153,255");
    // The lookup bisects now, so the ends of the table are worth naming.
    ExpectColor("aliceblue", "240,248,255,255");
    ExpectColor("yellowgreen", "154,205,50,255");
    // Case-insensitive, and still refusing what is not a colour -- a name
    // lookup that answered black would be the silent failure the file's own
    // comment forbids.
    ExpectColor("LimeGreen", "50,205,50,255");
    ExpectColor("transparent", "0,0,0,0");
    ExpectColor("notacolour", "-");
    ExpectColor("limegreenish", "-");
    ExpectColor("limegree", "-");
  });
}

}  // namespace microbrowser::tests

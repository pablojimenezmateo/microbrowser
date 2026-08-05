#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/MediaQuery.h"

namespace microbrowser::tests {

using css::MediaContext;
using css::MediaQueryListMatches;
using css::ResolveAbsoluteLength;

namespace {

MediaContext Screen(float width, float height, float density) {
  MediaContext context;
  context.viewport_width = width;
  context.viewport_height = height;
  context.device_pixel_ratio = density;
  return context;
}

bool Matches(std::string_view query, const MediaContext& context) {
  return MediaQueryListMatches(query, context);
}

}  // namespace

void RegisterMediaQueryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MediaQuery/AnEmptyListMatchesEverything", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(Matches("", context), "an empty query list");
    Expect(Matches("   ", context), "whitespace only");
  });

  AddTest(tests, "MediaQuery/MediaTypes", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(Matches("screen", context), "screen");
    Expect(Matches("all", context), "all");
    Expect(!Matches("print", context), "print");
    Expect(!Matches("tty", context), "an unknown type is false, not true");
    Expect(Matches("not print", context), "not print");
    Expect(!Matches("not screen", context), "not screen");
    Expect(Matches("only screen", context), "only is a no-op to a browser that parses this");
  });

  AddTest(tests, "MediaQuery/WidthAndHeight", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(Matches("(min-width: 600px)", context), "min-width below");
    Expect(!Matches("(min-width: 1200px)", context), "min-width above");
    Expect(Matches("(max-width: 1200px)", context), "max-width above");
    Expect(Matches("(width: 1000px)", context), "exact width");
    Expect(!Matches("(width: 999px)", context), "exact width, wrong");
    Expect(Matches("(min-height: 700px)", context), "min-height");
    Expect(Matches("(min-width: 40em)", context), "em is the initial font size here");
    Expect(!Matches("(min-width: 600)", context), "a unitless number is not a length");
  });

  AddTest(tests, "MediaQuery/AndOrAndNot", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(Matches("(min-width: 600px) and (max-width: 1200px)", context), "and, both true");
    Expect(!Matches("(min-width: 600px) and (max-width: 900px)", context), "and, one false");
    Expect(Matches("(min-width: 1200px) or (max-width: 1200px)", context), "or");
    Expect(!Matches("not (min-width: 600px)", context), "not, over a condition");
    Expect(Matches("screen and (min-width: 600px)", context), "a type with a condition");
    Expect(!Matches("print and (min-width: 600px)", context), "the type still has to match");
    // Mixing `and` with `or` at one level is a syntax error rather than a
    // precedence question, and an unparsable query is false.
    Expect(!Matches("(min-width: 1px) and (min-width: 2px) or (min-width: 3px)", context),
           "mixed operators");
  });

  AddTest(tests, "MediaQuery/NestedConditions", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(Matches("((min-width: 600px) and (max-width: 1200px))", context), "one level of nesting");
    Expect(!Matches("not ((min-width: 600px) and (max-width: 1200px))", context), "negated");
  });

  AddTest(tests, "MediaQuery/ResolutionAndOrientation", [] {
    const MediaContext hidpi = Screen(1000.0f, 800.0f, 2.0f);
    Expect(Matches("(min-resolution: 2dppx)", hidpi), "dppx");
    Expect(Matches("(min-resolution: 2x)", hidpi), "x is dppx spelled shorter");
    Expect(Matches("(min-resolution: 192dpi)", hidpi), "dpi is 96 to the CSS pixel");
    Expect(!Matches("(min-resolution: 3dppx)", hidpi), "above the device");
    Expect(Matches("(orientation: landscape)", hidpi), "wider than tall");
    Expect(Matches("(orientation: portrait)", Screen(800.0f, 1000.0f, 1.0f)), "taller than wide");
  });

  AddTest(tests, "MediaQuery/AListMatchesWhenAnyQueryDoes", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(Matches("print, screen", context), "the second query matches");
    Expect(Matches("(min-width: 2000px), (min-width: 100px)", context), "conditions in a list");
    Expect(!Matches("print, tty", context), "neither matches");
  });

  AddTest(tests, "MediaQuery/AnUnknownFeatureIsFalse", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    // Every feature this evaluator answers is a fact about the user's machine
    // that any page may ask for. The default has to be silence.
    Expect(!Matches("(prefers-color-scheme: dark)", context), "prefers-color-scheme");
    Expect(!Matches("(hover: hover)", context), "hover");
    Expect(!Matches("(device-width: 1000px)", context), "device-width is not viewport width");
    Expect(!Matches("(min-monitor-count: 1)", context), "a feature nobody has heard of");
  });

  AddTest(tests, "MediaQuery/MalformedInputIsFalseRatherThanAGuess", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    Expect(!Matches("(min-width: 600px", context), "unclosed parenthesis");
    Expect(!Matches("(", context), "one parenthesis");
    Expect(!Matches(")", context), "the wrong parenthesis");
    Expect(!Matches("(min-width: 600px) extra", context), "trailing tokens");
    Expect(!Matches("and (min-width: 600px)", context), "a leading operator");
    Expect(!Matches("(min-width: 600px) and", context), "a trailing operator");
    Expect(!Matches("()", context), "an empty feature");
    Expect(!Matches("screen and", context), "a type and nothing to and it with");
  });

  AddTest(tests, "MediaQuery/NestingIsBounded", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 1.0f);
    // The depth is attacker-chosen, so it is bounded rather than left to the
    // stack. Past the bound the query is false, which is what an unparsable
    // query is anyway.
    std::string deep;
    for (int i = 0; i < 4096; ++i) {
      deep += "(";
    }
    deep += "min-width: 1px";
    for (int i = 0; i < 4096; ++i) {
      deep += ")";
    }
    Expect(!Matches(deep, context), "four thousand parentheses");
  });

  AddTest(tests, "ResolveAbsoluteLength/UnitsAndRefusals", [] {
    const MediaContext context = Screen(1000.0f, 800.0f, 2.0f);
    Expect(ResolveAbsoluteLength("20px", context) == 20.0f, "px");
    Expect(ResolveAbsoluteLength("2em", context) == 32.0f, "em is the initial font size");
    Expect(ResolveAbsoluteLength("50vw", context) == 500.0f, "vw");
    Expect(ResolveAbsoluteLength("25vh", context) == 200.0f, "vh");
    Expect(ResolveAbsoluteLength("0", context) == 0.0f, "unitless zero is a length");
    Expect(!ResolveAbsoluteLength("5", context).has_value(), "a unitless number is not");
    Expect(!ResolveAbsoluteLength("50%", context).has_value(),
           "a percentage has no containing block here");
    Expect(!ResolveAbsoluteLength("20px 30px", context).has_value(), "two lengths are not one");
    Expect(!ResolveAbsoluteLength("", context).has_value(), "nothing is not a length");
  });
}

}  // namespace microbrowser::tests

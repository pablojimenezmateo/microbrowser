#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/MediaQuery.h"
#include "css/StyleSheet.h"

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

  // --- @media, which used to drop every parenthesised prelude ----------------

  AddTest(tests, "MediaQuery/AtMediaKeepsTheRulesItsPreludeMatches", [] {
    // The bug this pair is here for: `MediaListItemMatches` accepted a single
    // Ident, so `@media (min-width: 600px)` dropped its whole block -- on every
    // page this browser has ever rendered. ADR 0014 counts `@media` at 791
    // occurrences and calls it supported; it was not.
    const std::string_view css =
        "p { color: black }"
        "@media (min-width: 600px) { p { color: red } }"
        "@media screen and (max-width: 599px) { p { color: blue } }"
        "@media print { p { color: green } }"
        "@media not all and (min-width: 600px) { p { color: purple } }"
        "@media (min-width: 600px), (orientation: portrait) { p { font-size: 20px } }";

    css::MediaContext wide;
    wide.viewport_width = 1280.0f;
    wide.viewport_height = 800.0f;
    const css::StyleSheet at_1280 = css::ParseStyleSheet(css, wide);

    css::MediaContext narrow;
    narrow.viewport_width = 500.0f;
    narrow.viewport_height = 800.0f;
    const css::StyleSheet at_500 = css::ParseStyleSheet(css, narrow);

    // At 1280: the base rule, `min-width: 600px`, `not all and (min-width…)`
    // being false, and the comma list matching on its first item.
    ExpectEqInt(static_cast<long long>(at_1280.rules.size()), 3,
                "base, min-width, and the comma list");
    // At 500: the base rule, the `max-width: 599px` one, and `not all and
    // (min-width: 600px)` -- which is true precisely because the condition is
    // false. The comma list matches through `(orientation: portrait)`, since 500
    // by 800 is portrait, so it is there too.
    ExpectEqInt(static_cast<long long>(at_500.rules.size()), 4,
                "base, max-width, the negation, and the comma list through orientation");
    Expect(at_1280.rules.size() != at_500.rules.size(),
           "the two viewports disagree, which is the whole point");
  });

  AddTest(tests, "MediaQuery/AnUnreadablePreludeDropsItsBlockRatherThanKeepingIt", [] {
    css::MediaContext wide;
    wide.viewport_width = 1280.0f;
    // A feature this evaluator does not implement, and nonsense. Both are false
    // rather than a guess, which is what the specification says and is the safe
    // direction: a rule kept on a condition nobody evaluated is a rule applied
    // for no reason.
    const css::StyleSheet sheet = css::ParseStyleSheet(
        "@media (min-color-index: 2) { p { color: red } }"
        "@media ((((( { p { color: blue } }",
        wide);
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 0, "neither applies");
    // An *empty* prelude is the opposite case and is not a mistake: "an empty
    // media query list evaluates to true", which is also what makes
    // `sizes="100vw"` a valid entry with no condition in front of it.
    const css::StyleSheet empty =
        css::ParseStyleSheet("@media { p { color: green } }", wide);
    ExpectEqInt(static_cast<long long>(empty.rules.size()), 1, "@media {} applies");
  });

  AddTest(tests, "MediaQuery/ADefaultContextAnswersWhatTheOldCodeAnswered", [] {
    // A caller with no viewport -- the user-agent sheet, a test about selectors
    // -- gets a zero-sized one, which matches `max-width` and not `min-width`.
    // Every parenthesised prelude was dropped before the evaluator was wired in,
    // so this is deliberately *not* a behaviour change for those callers.
    const css::StyleSheet sheet =
        css::ParseStyleSheet("@media (min-width: 1px) { p { color: red } }"
                             "@media screen { a { color: blue } }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 1,
                "the bare media type still applies and the width query does not");
  });
}

}  // namespace microbrowser::tests

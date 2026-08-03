#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "js/RegExp.h"
#include "TestSupport.h"

// The regular expression engine, tested below the JavaScript layer.
//
// Everything here is about the matcher's answers rather than about
// `RegExp.prototype`, so a failure points at the engine rather than at the
// binding. The JS-visible surface is tested in JsInterpreterTests.

namespace microbrowser::tests {

namespace {

using js::RegExp;
using js::RegExpFlags;
using js::RegExpMatch;

RegExp Compile(std::string_view pattern, std::string_view flags = "") {
  const std::optional<RegExpFlags> parsed = RegExpFlags::Parse(flags);
  Expect(parsed.has_value(), "flags parse");
  std::string error;
  RegExp expression = RegExp::Compile(pattern, *parsed, error);
  Expect(expression.IsValid(), "pattern compiles: " + error);
  return expression;
}

// The matched text, or "-" when there is no match. One string makes the
// expectations read like the pattern they are testing.
std::string MatchText(const RegExp& expression, std::string_view input,
                      std::size_t start = 0) {
  const std::optional<RegExpMatch> match = expression.Exec(input, start, false);
  if (!match.has_value()) {
    return "-";
  }
  return std::string(match->Group(input, 0));
}

// The whole match and every group, separated by '|'. An absent group is "~",
// which is a different answer from a group that matched nothing.
std::string MatchGroups(const RegExp& expression, std::string_view input) {
  const std::optional<RegExpMatch> match = expression.Exec(input, 0, false);
  if (!match.has_value()) {
    return "-";
  }
  std::string out;
  for (std::size_t group = 0; group < match->GroupCount(); ++group) {
    if (group != 0) {
      out.push_back('|');
    }
    out += match->Participated(group) ? std::string(match->Group(input, group)) : "~";
  }
  return out;
}

}  // namespace

void RegisterJsRegExpTests(std::vector<TestCase>& tests) {
  // --- Literals, classes and the search ------------------------------------

  AddTest(tests, "JsRegExp/FindsALiteralAnywhereInTheInput", [] {
    const RegExp expression = Compile("cat");
    ExpectEqString(MatchText(expression, "the cat sat"), "cat", "found in the middle");
    ExpectEqString(MatchText(expression, "cat"), "cat", "found at the start");
    ExpectEqString(MatchText(expression, "the dog sat"), "-", "and absent when it is");
  });

  AddTest(tests, "JsRegExp/MatchesCharacterClassesAndRanges", [] {
    ExpectEqString(MatchText(Compile("[0-9]+"), "abc 4711 def"), "4711", "a digit run");
    ExpectEqString(MatchText(Compile("[^0-9]+"), "47xyz"), "xyz", "a negated class");
    ExpectEqString(MatchText(Compile("[a-c-]+"), "zz-abc"), "-abc",
                   "a trailing dash in a class is a literal dash");
    ExpectEqString(MatchText(Compile("\\d\\w\\s"), "a1b "), "1b ", "the shorthand classes");
  });

  AddTest(tests, "JsRegExp/DotSkipsLineTerminatorsUnlessDotAllIsSet", [] {
    ExpectEqString(MatchText(Compile("a.b"), "a\nb"), "-", "`.` does not cross a newline");
    ExpectEqString(MatchText(Compile("a.b", "s"), "a\nb"), "a\nb", "and does with `s`");
  });

  // --- Quantifiers ---------------------------------------------------------

  AddTest(tests, "JsRegExp/GreedyAndLazyQuantifiersTakeDifferentAmounts", [] {
    ExpectEqString(MatchText(Compile("<.*>"), "<a><b>"), "<a><b>", "greedy takes everything");
    ExpectEqString(MatchText(Compile("<.*?>"), "<a><b>"), "<a>", "lazy stops at the first");
    ExpectEqString(MatchText(Compile("a+?"), "aaa"), "a", "and `+?` takes one");
  });

  AddTest(tests, "JsRegExp/CountedQuantifiersRespectTheirBounds", [] {
    ExpectEqString(MatchText(Compile("a{2,3}"), "aaaa"), "aaa", "at most three");
    ExpectEqString(MatchText(Compile("a{2}"), "aaaa"), "aa", "exactly two");
    ExpectEqString(MatchText(Compile("a{4,}"), "aaa"), "-", "and at least four is not three");
    ExpectEqString(MatchText(Compile("a{2,3}?"), "aaaa"), "aa", "lazy takes the minimum");
  });

  AddTest(tests, "JsRegExp/ABraceThatIsNotAQuantifierIsALiteral", [] {
    // Annex B. `a{x}` is four characters, not a malformed quantifier, and a
    // page that writes a CSS selector into a pattern depends on it.
    ExpectEqString(MatchText(Compile("a{x}"), "za{x}"), "a{x}", "left as written");
  });

  AddTest(tests, "JsRegExp/AQuantifiedEmptyBodyTerminates", [] {
    // `(a*)*` can loop forever on a body that consumes nothing. The progress
    // check is what stops it, and this is the case that proves it runs.
    ExpectEqString(MatchText(Compile("(a*)*"), "aab"), "aa", "and matches what it should");
    ExpectEqString(MatchText(Compile("(|a)*"), "b"), "", "an empty alternative still ends");
  });

  // --- Alternation, groups and backreferences -------------------------------

  AddTest(tests, "JsRegExp/AlternationPrefersTheLeftmostBranch", [] {
    ExpectEqString(MatchText(Compile("ab|abc"), "abcd"), "ab", "the first branch that fits");
    ExpectEqString(MatchText(Compile("abc|ab"), "abcd"), "abc", "order decides, not length");
  });

  AddTest(tests, "JsRegExp/CapturesReportWhereEachGroupLanded", [] {
    ExpectEqString(MatchGroups(Compile("(\\d+)-(\\d+)"), "tel 555-1234"), "555-1234|555|1234",
                   "two groups and the whole match");
    ExpectEqString(MatchGroups(Compile("(a)|(b)"), "b"), "b|~|b",
                   "a branch not taken leaves its group absent, not empty");
    ExpectEqString(MatchGroups(Compile("(?:x)(y)"), "xy"), "xy|y",
                   "a non-capturing group takes no number");
  });

  AddTest(tests, "JsRegExp/AQuantifiedGroupResetsItsCapturesEachIteration", [] {
    // The last iteration is the one whose captures survive, and it did not
    // match group 1.
    ExpectEqString(MatchGroups(Compile("(?:(a)|b)+"), "ab"), "ab|~",
                   "group 1 is absent after an iteration that matched `b`");
  });

  AddTest(tests, "JsRegExp/BackreferencesMatchWhatTheGroupMatched", [] {
    ExpectEqString(MatchText(Compile("(ab)\\1"), "xabab"), "abab", "the same text again");
    ExpectEqString(MatchText(Compile("(ab)\\1"), "xabcd"), "-", "and nothing else");
    ExpectEqString(MatchText(Compile("(a)\\1", "i"), "aA"), "aA", "case-insensitively with `i`");
  });

  AddTest(tests, "JsRegExp/NamedGroupsAreNumberedAndReferable", [] {
    const RegExp expression = Compile("(?<year>\\d{4})-(?<month>\\d{2})");
    ExpectEqInt(static_cast<long long>(expression.GroupNamed("year")), 1, "year is group 1");
    ExpectEqInt(static_cast<long long>(expression.GroupNamed("month")), 2, "month is group 2");
    ExpectEqInt(static_cast<long long>(expression.GroupNamed("day")), 0, "and day is no group");
    ExpectEqString(MatchGroups(expression, "on 2026-08 ok"), "2026-08|2026|08", "and it matches");
    ExpectEqString(MatchText(Compile("(?<c>a)\\k<c>"), "aa"), "aa", "a named backreference");
  });

  // --- Assertions ----------------------------------------------------------

  AddTest(tests, "JsRegExp/AnchorsBindToTheInputUnlessMultilineIsSet", [] {
    ExpectEqString(MatchText(Compile("^b"), "a\nb"), "-", "`^` is the input's start");
    ExpectEqString(MatchText(Compile("^b", "m"), "a\nb"), "b", "and each line's with `m`");
    ExpectEqString(MatchText(Compile("a$"), "a\nb"), "-", "`$` is the input's end");
    ExpectEqString(MatchText(Compile("a$", "m"), "a\nb"), "a", "and each line's with `m`");
  });

  AddTest(tests, "JsRegExp/WordBoundariesSitBetweenWordAndNonWord", [] {
    ExpectEqString(MatchText(Compile("\\bcat\\b"), "a cat here"), "cat", "a whole word");
    ExpectEqString(MatchText(Compile("\\bcat\\b"), "concatenate"), "-", "not part of one");
    ExpectEqString(MatchText(Compile("\\Bcat"), "concat"), "cat", "and `\\B` is the opposite");
  });

  AddTest(tests, "JsRegExp/LookaheadConstrainsWithoutConsuming", [] {
    ExpectEqString(MatchText(Compile("foo(?=bar)"), "foobar"), "foo", "positive, and `bar` stays");
    ExpectEqString(MatchText(Compile("foo(?=bar)"), "foobaz"), "-", "and fails when it must");
    ExpectEqString(MatchText(Compile("foo(?!bar)"), "foobaz"), "foo", "negative");
    ExpectEqString(MatchText(Compile("foo(?!bar)"), "foobar"), "-", "and its failure");
  });

  AddTest(tests, "JsRegExp/LookbehindLooksAtWhatCameBefore", [] {
    ExpectEqString(MatchText(Compile("(?<=\\$)\\d+"), "costs $42"), "42", "positive");
    ExpectEqString(MatchText(Compile("(?<=\\$)\\d+"), "costs 42"), "-", "and its failure");
    ExpectEqString(MatchText(Compile("(?<!\\$)\\d+"), "costs 42"), "42", "negative");
    ExpectEqString(MatchText(Compile("(?<=ab)c"), "xabc"), "c", "a multi-character lookbehind");
  });

  AddTest(tests, "JsRegExp/ANegativeLookaheadKeepsNoCaptures", [] {
    // The assertion matched `b` on its way to failing the whole pattern, and
    // the group it filled in must not survive that.
    ExpectEqString(MatchGroups(Compile("a(?!(b))(.)"), "ac"), "ac|~|c",
                   "group 1 stays absent");
  });

  // --- Flags ---------------------------------------------------------------

  AddTest(tests, "JsRegExp/IgnoreCaseFoldsLiteralsAndClasses", [] {
    ExpectEqString(MatchText(Compile("cat", "i"), "CaT"), "CaT", "a literal");
    ExpectEqString(MatchText(Compile("[a-z]+", "i"), "ABC"), "ABC", "a range");
    ExpectEqString(MatchText(Compile("[^a]+", "i"), "Ab"), "b",
                   "and a negated class excludes both cases");
  });

  AddTest(tests, "JsRegExp/StickyMatchingStartsExactlyWhereItIsTold", [] {
    const RegExp expression = Compile("\\d+");
    Expect(expression.Exec("ab12", 0, true) == std::nullopt, "anchored at 0 finds nothing");
    Expect(expression.Exec("ab12", 2, true).has_value(), "anchored at 2 finds the digits");
    ExpectEqString(MatchText(expression, "ab12", 0), "12", "unanchored searches forward");
  });

  AddTest(tests, "JsRegExp/FlagsRoundTripAndRejectRepeats", [] {
    const std::optional<RegExpFlags> flags = RegExpFlags::Parse("gimsuy");
    Expect(flags.has_value(), "every flag parses");
    ExpectEqString(flags->Text(), "gimsuy", "and prints in canonical order");
    Expect(!RegExpFlags::Parse("gg").has_value(), "a repeated flag is an error");
    Expect(!RegExpFlags::Parse("q").has_value(), "and so is one that is not a flag");
  });

  // --- Escapes -------------------------------------------------------------

  AddTest(tests, "JsRegExp/EscapesNameTheCharactersTheyStandFor", [] {
    ExpectEqString(MatchText(Compile("\\x41"), "xAy"), "A", "a hex escape");
    ExpectEqString(MatchText(Compile("\\u0041"), "xAy"), "A", "a unicode escape");
    ExpectEqString(MatchText(Compile("a\\.b"), "a.b"), "a.b", "an escaped metacharacter");
    ExpectEqString(MatchText(Compile("a\\.b"), "axb"), "-", "which is not the metacharacter");
    ExpectEqString(MatchText(Compile("\\t"), "a\tb"), "\t", "a tab");
  });

  AddTest(tests, "JsRegExp/ANonAsciiLiteralMatchesItsBytes", [] {
    // Byte-oriented, and deliberately: see the note on the RegExp class. The
    // pattern's UTF-8 bytes have to match the input's, in order.
    ExpectEqString(MatchText(Compile("é"), "café"), "é", "a two-byte character");
  });

  // --- Refusals and bounds -------------------------------------------------

  AddTest(tests, "JsRegExp/AMalformedPatternIsRefusedRatherThanApproximated", [] {
    const auto refuses = [](std::string_view pattern) {
      std::string error;
      const RegExp expression = RegExp::Compile(pattern, RegExpFlags{}, error);
      Expect(!expression.IsValid(), std::string("refuses ") + std::string(pattern));
      Expect(!error.empty(), "and says why");
    };
    refuses("(");
    refuses("[a");
    refuses("a)");
    refuses("*a");
    refuses("a{3,2}");
    refuses("a\\");
  });

  AddTest(tests, "JsRegExp/APatternThatWouldExplodeIsRefusedAtCompileTime", [] {
    // A counted repeat is compiled by copying its body, so nested ones
    // multiply: fifteen characters, a hundred thousand instructions. The size
    // limit is the only thing between this and however much memory the page
    // asks for.
    std::string error;
    const RegExp expression = RegExp::Compile("((ab|cd){100}){100}", RegExpFlags{}, error);
    Expect(!expression.IsValid(), "refused");
    ExpectEqString(error, "regular expression is too large", "and named as too large");

    // A repeated single-byte class does *not* expand -- it is one instruction
    // whatever the count -- so the limit does not refuse something cheap.
    std::string ok_error;
    Expect(RegExp::Compile("a{200000}", RegExpFlags{}, ok_error).IsValid(),
           "a counted class repeat stays one instruction");
  });

  AddTest(tests, "JsRegExp/CatastrophicBacktrackingGivesUpInsteadOfHanging", [] {
    // The classic: every way of splitting the run of `a` between the two
    // quantifiers is tried before the final `b` fails. Exponential, and a page
    // can write it by accident. The step budget turns it into a wrong answer
    // rather than a hung browser -- which is the trade this test records.
    const RegExp expression = Compile("(a+)+b");
    ExpectEqString(MatchText(expression, std::string(40, 'a') + "c"), "-",
                   "no match, and it returned");
  });

  AddTest(tests, "JsRegExp/AGreedyRunOverALargeInputDoesNotExhaustAnything", [] {
    // The specialized repeat instruction is what makes this one backtrack
    // frame instead of a hundred thousand.
    const std::string input = std::string(100'000, 'a') + "b";
    ExpectEqString(MatchText(Compile("a*b"), input).size() == input.size() ? "whole" : "short",
                   "whole", "the greedy run gives bytes back without a frame each");
  });
}

}  // namespace microbrowser::tests

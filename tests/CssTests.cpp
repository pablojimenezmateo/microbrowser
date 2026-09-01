#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/ComputedStyle.h"
#include "css/Cssom.h"
#include "css/DeclarationText.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "css/Tokenizer.h"
#include "dom/Node.h"
#include "html/TreeBuilder.h"

namespace microbrowser::tests {

using css::ApplyDeclaration;
using css::ComputedStyle;
using css::Declaration;
using css::ParseDeclarationList;
using css::ParseSelectorList;
using css::ParseStyleSheet;
using css::Selector;
using css::StyleSheet;
using css::Token;
using dom::Element;

namespace {

std::vector<Token> TokensOf(std::string_view input) {
  std::vector<Token> tokens = css::Tokenize(input);
  // Whitespace and EOF are structural noise for most assertions.
  std::vector<Token> out;
  for (const Token& token : tokens) {
    if (token.kind != Token::Kind::Whitespace && token.kind != Token::Kind::EndOfFile) {
      out.push_back(token);
    }
  }
  return out;
}

bool SelectorMatches(std::string_view selector_text, std::string_view html,
                     std::string_view target_tag) {
  const std::vector<Selector> selectors = ParseSelectorList(selector_text);
  Expect(!selectors.empty(), std::string("selector did not parse: ") + std::string(selector_text));
  const std::unique_ptr<dom::Document> document = html::ParseDocument(html);
  const Element* target = document->FirstElementByTagName(target_tag);
  Expect(target != nullptr, "target element not found");
  return selectors.front().Matches(*target);
}

// The 1-based positions, among a container's element children, that a selector
// picks: "1 3 5 " for the first, third and fifth. An `An+B` is a statement
// about a whole sequence, and checking one element at a time hides exactly the
// off-by-one at each end that the grammar is easiest to get wrong at.
std::string MatchedChildren(std::string_view selector_text, std::string_view html,
                            std::string_view container_tag) {
  const std::vector<Selector> selectors = ParseSelectorList(selector_text);
  Expect(!selectors.empty(), std::string("selector did not parse: ") + std::string(selector_text));
  const std::unique_ptr<dom::Document> document = html::ParseDocument(html);
  const Element* container = document->FirstElementByTagName(container_tag);
  Expect(container != nullptr, "container element not found");
  std::string out;
  int index = 0;
  for (const std::unique_ptr<dom::Node>& child : container->Children()) {
    if (!child->IsElement()) {
      continue;
    }
    ++index;
    if (selectors.front().Matches(static_cast<const Element&>(*child))) {
      out += std::to_string(index);
      out.push_back(' ');
    }
  }
  return out;
}

bool SelectorParses(std::string_view selector_text) {
  return !ParseSelectorList(selector_text).empty();
}

}  // namespace

void RegisterCssTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CssTokenizer/ProducesTheSpecsTokenKinds", [] {
    const std::vector<Token> tokens = TokensOf("div .cls #id 42 42px 50% \"s\" url(x) @media !");
    ExpectEqInt(static_cast<long long>(tokens.at(0).kind), static_cast<long long>(Token::Kind::Ident),
                "an identifier");
    Expect(tokens.at(1).kind == Token::Kind::Delim && tokens.at(1).value == ".", "a delim");
    Expect(tokens.at(3).kind == Token::Kind::Hash, "a hash");
    Expect(tokens.at(4).kind == Token::Kind::Number && tokens.at(4).is_integer, "a number");
    Expect(tokens.at(5).kind == Token::Kind::Dimension && tokens.at(5).value == "px",
           "a dimension carries its unit");
    Expect(tokens.at(6).kind == Token::Kind::Percentage, "a percentage");
    Expect(tokens.at(7).kind == Token::Kind::String && tokens.at(7).value == "s", "a string");
    Expect(tokens.at(8).kind == Token::Kind::Url && tokens.at(8).value == "x",
           "an unquoted url is its own token");
    Expect(tokens.at(9).kind == Token::Kind::AtKeyword && tokens.at(9).value == "media",
           "an at-keyword");
  });

  AddTest(tests, "CssTokenizer/ParsesEveryNumberForm", [] {
    const std::vector<Token> tokens = TokensOf("1 +2 -3 .5 1.5 1e3 1E-2 -0.25");
    Expect(tokens.at(0).number == 1.0, "integer");
    Expect(tokens.at(1).number == 2.0, "explicit plus");
    Expect(tokens.at(2).number == -3.0, "negative");
    Expect(tokens.at(3).number == 0.5, "leading dot");
    Expect(tokens.at(4).number == 1.5, "decimal");
    Expect(tokens.at(5).number == 1000.0, "exponent");
    Expect(tokens.at(6).number == 0.01, "negative exponent");
    Expect(tokens.at(7).number == -0.25, "negative decimal");
    Expect(!tokens.at(4).is_integer, "a decimal is not an integer");
  });

  AddTest(tests, "CssTokenizer/CustomPropertiesAreIdentifiers", [] {
    const std::vector<Token> tokens = TokensOf("--main-color");
    ExpectEqInt(static_cast<long long>(tokens.size()), 1,
                "`--x` is one identifier; treating the dashes as delims silently loses every "
                "custom property");
    ExpectEqString(tokens.at(0).value, "--main-color", "with the dashes in its name");
  });

  AddTest(tests, "CssTokenizer/HandlesEscapes", [] {
    ExpectEqString(TokensOf("\\41").at(0).value, "A", "a hex escape");
    ExpectEqString(TokensOf("\\31 23").at(0).value, "123",
                   "one whitespace after the digits ends the escape, so this is `1` then `23` "
                   "rather than code point 0x3123");
    ExpectEqString(TokensOf("\\-x").at(0).value, "-x", "an escaped literal");
    ExpectEqString(TokensOf("\\0").at(0).value, "\xEF\xBF\xBD",
                   "a null escape becomes U+FFFD rather than a NUL in the middle of a name");
  });

  AddTest(tests, "CssTokenizer/PreprocessesNulAndEscapedEof", [] {
    const std::string nul_ident = std::string("foo") + '\0' + "bar";
    ExpectEqString(TokensOf(nul_ident).at(0).value, std::string("foo") + "\xEF\xBF\xBD" + "bar",
                   "a NUL in an ident is U+FFFD, not a second ident");
    ExpectEqString(TokensOf("foo\\").at(0).value, std::string("foo") + "\xEF\xBF\xBD",
                   "an escaped EOF is U+FFFD in an ident");
    ExpectEqString(css::ReconstructTokens(css::Tokenize("foo\\"), 0, 2),
                   std::string("foo") + "\xEF\xBF\xBD", "and reconstruction keeps it");
  });

  AddTest(tests, "CssTokenizer/InsertsACommentBetweenTokensThatWouldMerge", [] {
    const std::vector<Token> tokens = css::Tokenize("foo bar");
    // Drop the whitespace so the two idents sit next to each other, which is
    // what `var(--a)var(--b)` produces after substitution.
    std::vector<Token> adjacent;
    adjacent.push_back(tokens.at(0));
    adjacent.push_back(tokens.at(2));
    ExpectEqString(css::ReconstructTokens(adjacent, 0, 2), "foo/**/bar",
                   "concatenating two idents would make one ident");
    const std::vector<Token> anb = css::Tokenize("2n+1");
    ExpectEqString(css::ReconstructTokens(anb, 0, anb.size()), "2n+1",
                   "a signed number keeps the plus that An+B needs");
  });

  AddTest(tests, "Css/UnicodeRangeSerializesWithoutARedundantEnd", [] {
    std::string canonical;
    Expect(css::CanonicaliseDeclaration("unicode-range", "u+a", &canonical) ==
               css::DeclarationValidity::Canonical,
           "u+a parses");
    ExpectEqString(canonical, "U+A", "a single code point is not written as a range of itself");
    Expect(css::CanonicaliseDeclaration("unicode-range", "u/**/+/**/a/**/?", &canonical) ==
               css::DeclarationValidity::Canonical,
           "comments between the tokens of a urange are skipped");
    ExpectEqString(canonical, "U+A0-AF", "and wildcards expand");
    Expect(css::CanonicaliseDeclaration("unicode-range", "u+ abc", &canonical) ==
               css::DeclarationValidity::Invalid,
           "whitespace inside a urange is a parse error");
  });

  AddTest(tests, "Css/UaPlusAIsASelectorNotAUnicodeRange", [] {
    const std::vector<Selector> selectors = ParseSelectorList("u+a");
    ExpectEqInt(static_cast<long long>(selectors.size()), 1, "it parses");
    ExpectEqString(css::SerializeSelectorList(selectors), "u + a",
                   "as a next-sibling pair, which is what CSS Syntax 3 made of the old token");
    const StyleSheet sheet = ParseStyleSheet("u+a { color: green }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 1,
                "and the cascade parser sees the same selector, not a dropped urange");
  });

  AddTest(tests, "Css/EofClosesAnUnclosedSelectorConstruct", [] {
    Expect(!ParseSelectorList("[foo").empty(), "`[foo` at EOF is `[foo]`");
    Expect(!ParseSelectorList(":nth-child(1").empty(), "`:nth-child(1` at EOF is `:nth-child(1)`");
  });

  AddTest(tests, "Css/AtRuleInADeclarationListDoesNotEatTheNextDeclaration", [] {
    const std::vector<Declaration> declarations =
        ParseDeclarationList("@at {} color: green; @at at; font-size: 16px");
    ExpectEqInt(static_cast<long long>(declarations.size()), 2,
                "both real declarations survive the at-rules");
    ExpectEqString(declarations.at(0).property, "color", "the one after a block at-rule");
    ExpectEqString(declarations.at(1).property, "font-size", "and the one after a semicolon at-rule");
    const std::vector<css::CssomRule> page = css::ParseCssom(
        "@page { @at {} margin-top: 20px; @at at; margin-bottom: 10px; }");
    ExpectEqInt(static_cast<long long>(page.size()), 1, "one page rule");
    ExpectEqInt(static_cast<long long>(page.at(0).declarations.size()), 2,
                "both page descriptors survive the nested at-rules");
  });

  AddTest(tests, "CssTokenizer/RecognizesBadStringsAndUrls", [] {
    Expect(TokensOf("\"unterminated\nrest").at(0).kind == Token::Kind::BadString,
           "a newline inside a string makes it a bad-string, which invalidates its "
           "declaration rather than the whole sheet");
    Expect(TokensOf("url(a b)").at(0).kind == Token::Kind::BadUrl, "a space inside a bare url");
  });

  AddTest(tests, "CssTokenizer/CommentsDisappearAndAnUnterminatedOneEatsTheRest", [] {
    ExpectEqInt(static_cast<long long>(TokensOf("a/*c*/b").size()), 2, "a comment is not a token");
    ExpectEqInt(static_cast<long long>(TokensOf("a/*unterminated").size()), 1,
                "an unterminated comment swallows the rest, which is what every browser does");
  });

  // --- Parsing --------------------------------------------------------------

  AddTest(tests, "CssParser/ParsesRulesAndDeclarations", [] {
    const StyleSheet sheet = ParseStyleSheet("p { color: red; margin: 0 auto } a { color: blue }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 2, "two rules");
    ExpectEqInt(static_cast<long long>(sheet.rules.at(0).declarations.size()), 2,
                "two declarations in the first");
    ExpectEqString(sheet.rules.at(0).declarations.at(0).property, "color", "property");
    ExpectEqString(sheet.rules.at(0).declarations.at(0).value, "red", "value");
    ExpectEqString(sheet.rules.at(0).declarations.at(1).value, "0 auto",
                   "a multi-token value is reconstructed with single spaces");
  });

  AddTest(tests, "CssParser/RecognizesImportant", [] {
    const std::vector<Declaration> declarations =
        ParseDeclarationList("color: red !important; width: 10px");
    Expect(declarations.at(0).important, "the first is important");
    ExpectEqString(declarations.at(0).value, "red",
                   "and the marker is not part of the value");
    Expect(!declarations.at(1).important, "the second is not");
  });

  // CSS error recovery is normative. A bad declaration loses itself and nothing
  // else; a bad rule loses itself and nothing else.
  AddTest(tests, "CssParser/RecoversFromErrorsTheWayTheSpecSays", [] {
    const std::vector<Declaration> declarations =
        ParseDeclarationList("color: red; bogus; width: 10px; : 5; height: 2px");
    ExpectEqInt(static_cast<long long>(declarations.size()), 3,
                "the three well-formed declarations survive the two broken ones between them");
    ExpectEqString(declarations.at(2).property, "height", "including the one after the worst");

    const StyleSheet sheet = ParseStyleSheet("p { color: red } { } a { color: blue }");
    Expect(sheet.rules.size() >= 2, "a rule with no selector does not lose its neighbours");
  });

  AddTest(tests, "CssParser/DropsARuleWhoseSelectorItCannotParse", [] {
    // Applying the declarations to something else would be worse than losing
    // them, which is why an unparsable selector drops the whole rule.
    // `::before` and `:has()` both parse now; `:nth-col()` does not.
    const StyleSheet sheet = ParseStyleSheet("p:nth-col(2) { color: red } a { color: blue }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 1, "only the parsable rule remains");
    ExpectEqString(sheet.rules.at(0).selectors.at(0).compounds.at(0).parts.at(0).name, "a",
                   "and it is the right one");
    Expect(sheet.skipped > 0, "the drop is counted rather than silent");
  });

  AddTest(tests, "CssParser/ParsesBeforeAndAfterPseudoElements", [] {
    const StyleSheet sheet =
        ParseStyleSheet("div::before { content: \"\"; display: block; padding-top: 56% } "
                        "span:after { content: none }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 2, "both pseudo-element rules parse");
    ExpectEqInt(static_cast<long long>(sheet.skipped), 0, "and neither is skipped");
    Expect(sheet.rules.at(0).selectors.at(0).SubjectPseudoElement() == css::PseudoElement::Before,
           "double-colon before");
    Expect(sheet.rules.at(1).selectors.at(0).SubjectPseudoElement() == css::PseudoElement::After,
           "legacy single-colon after");
  });

  AddTest(tests, "CssParser/AppliesSupportedMediaBlocks", [] {
    const StyleSheet sheet =
        ParseStyleSheet("@media screen { p { color: red } } "
                        "@media all, screen { a { color: blue } } "
                        "@media print, screen { div { color: green } }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 3,
                "screen, all, and mixed media lists apply to this screen-only engine");
    ExpectEqString(sheet.rules.at(0).selectors.at(0).compounds.at(0).parts.at(0).name, "p",
                   "the first nested rule survives");
    ExpectEqString(sheet.rules.at(1).selectors.at(0).compounds.at(0).parts.at(0).name, "a",
                   "and so does a comma-separated matching media list");
    ExpectEqString(sheet.rules.at(2).selectors.at(0).compounds.at(0).parts.at(0).name, "div",
                   "unsupported media entries do not poison a matching one");
  });

  // `@supports` is the page asking what this engine can do, and every wrong
  // answer sends it down a branch written for a browser this is not. The
  // property side of that is checked in StyleResolverTests/AnswersSupports;
  // this is the condition grammar around it.
  AddTest(tests, "CssParser/EvaluatesSupportsConditions", [] {
    const auto selectors = [](std::string_view css) {
      std::string names;
      for (const auto& rule : ParseStyleSheet(css).rules) {
        names += rule.selectors.at(0).compounds.at(0).parts.at(0).name + " ";
      }
      return names;
    };
    ExpectEqString(selectors("@supports (display: flex) { a { color: red } }"), "a ",
                   "a condition this engine meets applies its block");
    ExpectEqString(selectors("@supports (display: grid) { a { color: red } }"), "",
                   "and one it does not is dropped, not applied");
    ExpectEqString(selectors("@supports not (display: grid) { a { color: red } }"), "a ", "not");
    ExpectEqString(selectors("@supports (display: flex) and (color: red) { a { x: y } }"), "a ",
                   "and, with both true");
    ExpectEqString(selectors("@supports (display: flex) and (display: grid) { a { x: y } }"), "",
                   "and, with one false");
    ExpectEqString(selectors("@supports (display: grid) or (display: flex) { a { x: y } }"), "a ",
                   "or");
    ExpectEqString(
        selectors("@supports ((display: grid) or (display: flex)) and (color: red) { a{x:y} }"),
        "a ", "a condition may hold another");
    ExpectEqString(selectors("@supports (display:grid) and (color:red) or (color:blue) {a{x:y}}"),
                   "",
                   "`and` mixed with `or` and no parentheses is a syntax error, not a "
                   "precedence question -- and a prelude that does not parse is false");
    ExpectEqString(selectors("@supports selector(a > b) { a { x: y } }"), "a ",
                   "selector() asks the same parser a stylesheet uses");
    ExpectEqString(selectors("@supports selector(!!) { a { x: y } }"), "",
                   "an invalid selector is unsupported, not a crash");
    ExpectEqString(selectors("@supports (display: flex) garbage { a { x: y } }"), "",
                   "and so does a prelude with something left over");
    ExpectEqString(selectors("@supports (--x: 1) { a { x: y } }"), "a ",
                   "a custom property has no grammar to fail, so it is always supported");
    ExpectEqString(
        selectors("@supports ((((((((((display: flex)))))))))) { a { x: y } }"), "",
        "nesting past the bound is false rather than a deeper recursion over hostile input");
    ExpectEqString(selectors("@supports (display: flex { a { x: y } }"), "",
                   "an unclosed condition consumes the rest of the prelude and answers no");
  });

  AddTest(tests, "CssParser/SkipsUnsupportedAtRulesAndCountsThem", [] {
    const StyleSheet sheet =
        ParseStyleSheet("@media print { p { color: red } } "
                        "@media screen and (min-width: 1px) { div { color: green } } "
                        "a { color: blue }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 1,
                "unsupported media blocks are not applied unconditionally, which is what "
                "parsing into the top level would do");
    Expect(sheet.skipped > 0, "and the gap is observable");
  });

  AddTest(tests, "CssParser/ABadStringInvalidatesOnlyItsDeclaration", [] {
    const std::vector<Declaration> declarations =
        ParseDeclarationList("a: \"bad\nstring\"; color: red");
    Expect(std::none_of(declarations.begin(), declarations.end(),
                        [](const Declaration& d) { return d.property == "a"; }),
           "the broken declaration is gone");
  });

  // --- Selectors ------------------------------------------------------------

  AddTest(tests, "CssSelector/MatchesTheSimpleForms", [] {
    Expect(SelectorMatches("p", "<p>x</p>", "p"), "type");
    Expect(SelectorMatches("*", "<p>x</p>", "p"), "universal");
    Expect(SelectorMatches(".c", "<p class='c'>x</p>", "p"), "class");
    Expect(SelectorMatches(".c", "<p class='a c b'>x</p>", "p"), "one class among several");
    Expect(!SelectorMatches(".c", "<p class='cx'>x</p>", "p"),
           "a class is a whole word, not a prefix");
    Expect(SelectorMatches("#i", "<p id='i'>x</p>", "p"), "id");
    Expect(!SelectorMatches("#i", "<p id='j'>x</p>", "p"), "a different id");
    Expect(SelectorMatches("p.c#i", "<p class='c' id='i'>x</p>", "p"), "a compound");
    Expect(!SelectorMatches("p.c#i", "<p class='c' id='j'>x</p>", "p"),
           "a compound needs every part");
  });

  AddTest(tests, "CssSelector/MatchesCombinators", [] {
    Expect(SelectorMatches("div p", "<div><section><p>x</p></section></div>", "p"),
           "descendant reaches through");
    Expect(SelectorMatches("div > section", "<div><section><p>x</p></section></div>", "section"),
           "child matches a direct child");
    Expect(!SelectorMatches("div > p", "<div><section><p>x</p></section></div>", "p"),
           "and only a direct child");
    Expect(SelectorMatches("h1 + p", "<h1>t</h1><p>x</p>", "p"), "adjacent sibling");
    Expect(!SelectorMatches("h1 + p", "<h1>t</h1><div>d</div><p>x</p>", "p"),
           "adjacent means immediately adjacent");
    Expect(SelectorMatches("h1 ~ p", "<h1>t</h1><div>d</div><p>x</p>", "p"),
           "a later sibling need not be adjacent");
    Expect(!SelectorMatches("h2 ~ p", "<h1>t</h1><div>d</div><p>x</p><h2>later</h2>", "p"),
           "general sibling does not look forward");
  });

  AddTest(tests, "CssSelector/MatchesAttributeSelectors", [] {
    Expect(SelectorMatches("[href]", "<a href='/x'>l</a>", "a"), "existence");
    Expect(SelectorMatches("[href='/x']", "<a href='/x'>l</a>", "a"), "equality");
    Expect(!SelectorMatches("[href='/y']", "<a href='/x'>l</a>", "a"), "and inequality");
    Expect(SelectorMatches("[class~='b']", "<a class='a b c'>l</a>", "a"), "word inclusion");
    Expect(SelectorMatches("[lang|='en']", "<a lang='en-GB'>l</a>", "a"), "dash match");
    Expect(SelectorMatches("[href^='/x']", "<a href='/xyz'>l</a>", "a"), "prefix");
    Expect(SelectorMatches("[href$='z']", "<a href='/xyz'>l</a>", "a"), "suffix");
    Expect(SelectorMatches("[href*='xy']", "<a href='/xyz'>l</a>", "a"), "substring");
  });

  AddTest(tests, "CssSelector/MatchesStructuralPseudoClasses", [] {
    Expect(SelectorMatches("p:first-child", "<div><p>a</p><p>b</p></div>", "p"), "first-child");
    Expect(!SelectorMatches("p:last-child", "<div><p>a</p><p>b</p></div>", "p"),
           "the first paragraph is not the last child");
    Expect(SelectorMatches("p:only-child", "<div><p>a</p></div>", "p"), "only-child");
    Expect(SelectorMatches("p:empty", "<div><p></p></div>", "p"), "empty");
    Expect(SelectorMatches("p:empty", "<div><p><!-- note --></p></div>", "p"),
           "comments do not make an element non-empty");
    Expect(SelectorMatches("p:first-of-type", "<div><span>a</span><p>b</p></div>", "p"),
           "first-of-type ignores earlier elements with other tag names");
    Expect(SelectorMatches("p:last-of-type", "<div><p>a</p><span>b</span></div>", "p"),
           "last-of-type ignores later elements with other tag names");
    Expect(SelectorMatches("p:only-of-type", "<div><span>a</span><p>b</p><em>c</em></div>", "p"),
           "only-of-type counts siblings with the same tag name");
    Expect(!SelectorMatches("p:only-of-type", "<div><p>a</p><span>b</span><p>c</p></div>", "p"),
           "only-of-type fails when another element has the same tag name");
  });

  AddTest(tests, "CssSelector/AnUnknownPseudoClassMatchesNothing", [] {
    // Matching would apply a rule the author scoped to a state we cannot
    // observe — `:hover` styles would be permanently on.
    const std::vector<Selector> selectors = ParseSelectorList("p:hover");
    Expect(!selectors.empty(), "it parses");
    const std::unique_ptr<dom::Document> document = html::ParseDocument("<p>x</p>");
    Expect(!selectors.front().Matches(*document->FirstElementByTagName("p")),
           "but it must not match, or every :hover rule is always applied");
  });

  AddTest(tests, "CssSelector/ComputesSpecificityAsThreeCountsRatherThanASum", [] {
    const auto specificity = [](std::string_view text) {
      return ParseSelectorList(text).front().ComputeSpecificity();
    };
    Expect(specificity("p") < specificity(".c"), "a class beats a type");
    Expect(specificity(".c") < specificity("#i"), "an id beats a class");
    // The classic bug: summing the counts makes eleven classes beat one id.
    Expect(specificity(".a.b.c.d.e.f.g.h.i.j.k") < specificity("#i"),
           "eleven classes must not beat one id, which is exactly what summing would do");
    Expect(specificity("*") < specificity("p"), "the universal selector contributes nothing");
  });

  AddTest(tests, "CssSelector/ParsesSelectorLists", [] {
    const std::vector<Selector> selectors = ParseSelectorList("h1, h2 > span, .c");
    ExpectEqInt(static_cast<long long>(selectors.size()), 3, "three selectors");
    ExpectEqInt(static_cast<long long>(selectors.at(1).compounds.size()), 2,
                "the second has two compounds");
  });

  AddTest(tests, "CssParser/ProducesAStyleSheetForAnyInput", [] {
    // Same property as HTML: CSS has no failure mode.
    for (const std::string_view input : {
             "", "{", "}", "}}}", "p {", "p { color", "p { color:", "@", "@media", "/*",
             "\"", "url(", "#", ".", ":", "[", "p{color:red", "\xFF\xFE",
         }) {
      const StyleSheet sheet = ParseStyleSheet(input);
      Expect(sheet.rules.size() + sheet.skipped < 1000,
             std::string("runaway parse for: ") + std::string(input));
    }
  });

  AddTest(tests, "CssSelector/NotMatchesTheElementsItsArgumentDoesNot", [] {
    Expect(SelectorMatches("p:not(.x)", "<p>a</p>", "p"), "no class, so :not(.x) holds");
    Expect(!SelectorMatches("p:not(.x)", "<p class=x>a</p>", "p"), "and fails when it does");
    // The argument is a *complex* selector, so it walks up from the element on
    // its own. `:not(div p)` is not `:not(p)`.
    Expect(!SelectorMatches("p:not(div p)", "<div><p>a</p></div>", "p"),
           "a complex argument matched from the same element");
    Expect(SelectorMatches("p:not(section p)", "<div><p>a</p></div>", "p"),
           "and fails to match when its ancestor is wrong");
    // A list: the element must match none of them.
    Expect(!SelectorMatches("p:not(.x, .y)", "<p class=y>a</p>", "p"),
           "one of two arguments matching is enough to reject");
    Expect(SelectorMatches(".fraud:not(:empty)", "<div class=fraud>text</div>", "div"),
           "a pseudo-class nests inside :not, which reddit's stylesheet does");
    Expect(!SelectorMatches(".fraud:not(:empty)", "<div class=fraud></div>", "div"),
           "and answers the other way when it is empty");
    Expect(!SelectorParses("p:not()"), "an empty argument list is not a selector");
  });

  AddTest(tests, "CssSelector/IsAndWhereMatchAnyOfTheirArguments", [] {
    Expect(SelectorMatches(":is(h1, h2, p)", "<p>a</p>", "p"), ":is matches the third");
    Expect(!SelectorMatches(":is(h1, h2)", "<p>a</p>", "p"), "and nothing when none match");
    Expect(SelectorMatches(":where(h1, p) span", "<p><span>a</span></p>", "span"),
           ":where matches the same elements :is does");
    // One `:is(...)` standing in for a dozen rules is how modern stylesheets
    // compress, which is why failing to match it loses more than its count.
    Expect(SelectorMatches(":is(.menu, .nav) > :is(a, button)",
                           "<div class=nav><button>go</button></div>", "button"),
           "an :is on both sides of a combinator");
    Expect(SelectorMatches("a:not(:is(.x, .y))", "<a href=#>a</a>", "a"),
           ":is nests inside :not");
  });

  AddTest(tests, "CssSelector/WhereContributesNoSpecificityAndIsAndNotContributeTheirMost", [] {
    const auto specificity = [](std::string_view text) {
      return ParseSelectorList(text).front().ComputeSpecificity();
    };
    // The entire point of `:where()`. Getting it wrong does not look like a
    // selector bug -- it looks like a rendering bug, because the cascade order
    // inverts and the wrong declaration wins.
    Expect(specificity(":where(#id)") == specificity("*"), ":where() contributes zero");
    Expect(specificity(":is(#id)") == specificity("#id"),
           ":is() takes the specificity of its most specific argument");
    Expect(specificity(":not(#id)") == specificity("#id"), "and so does :not()");
    Expect(specificity(":is(p, .c, #id)") == specificity("#id"),
           "the *most* specific argument, not the first or the last");
    Expect(specificity(":where(#a) p") == specificity("p"),
           ":where() is transparent to the rest of the selector");
    Expect(specificity("li:nth-child(2n)") == specificity("li.c"),
           "a functional pseudo-class still counts as one pseudo-class");
  });

  AddTest(tests, "CssSelector/NthChildImplementsTheWholeAnPlusBGrammar", [] {
    const std::string_view list = "<ul><li>1</li><li>2</li><li>3</li><li>4</li><li>5</li>"
                                 "<li>6</li><li>7</li></ul>";
    const auto picked = [&](std::string_view selector) {
      return MatchedChildren(selector, list, "ul");
    };
    ExpectEqString(picked("li:nth-child(odd)"), "1 3 5 7 ", "odd");
    ExpectEqString(picked("li:nth-child(even)"), "2 4 6 ", "even");
    ExpectEqString(picked("li:nth-child(3)"), "3 ", "a bare integer is B with A of zero");
    ExpectEqString(picked("li:nth-child(2n)"), "2 4 6 ", "2n");
    ExpectEqString(picked("li:nth-child(2n+1)"), "1 3 5 7 ", "2n+1, one dimension and one number");
    ExpectEqString(picked("li:nth-child(2n-1)"), "1 3 5 7 ",
                   "2n-1, where the -1 is inside the dimension's unit");
    ExpectEqString(picked("li:nth-child(2n + 1)"), "1 3 5 7 ", "spaces around the sign");
    ExpectEqString(picked("li:nth-child(2n- 1)"), "1 3 5 7 ",
                   "2n- 1, where the sign is inside the unit and the digits are not");
    ExpectEqString(picked("li:nth-child(n)"), "1 2 3 4 5 6 7 ", "n alone is every element");
    ExpectEqString(picked("li:nth-child(+n)"), "1 2 3 4 5 6 7 ", "and so is +n");
    ExpectEqString(picked("li:nth-child(n+4)"), "4 5 6 7 ", "n+4 is the fourth onwards");
    ExpectEqString(picked("li:nth-child(-n+3)"), "1 2 3 ",
                   "-n+3 is the first three, which is the one negative A that pages use");
    ExpectEqString(picked("li:nth-child(-n-3)"), "", "-n-3 selects nothing at all");
    ExpectEqString(picked("li:nth-child(0n+3)"), "3 ", "an explicit zero A");
    ExpectEqString(picked("li:nth-child(1n+7)"), "7 ",
                   "1n+7, which is what reddit writes for its comment listing");
    ExpectEqString(picked("li:nth-last-child(2)"), "6 ", "counting from the end");
    ExpectEqString(picked("li:nth-last-child(-n+2)"), "6 7 ", "the last two");
  });

  AddTest(tests, "CssSelector/NthOfTypeCountsOnlySiblingsWithTheSameTagName", [] {
    const std::string_view mixed = "<ul><li>1</li><span>2</span><li>3</li><span>4</span>"
                                  "<li>5</li></ul>";
    ExpectEqString(MatchedChildren("li:nth-of-type(2)", mixed, "ul"), "3 ",
                   "the second li is the third child");
    ExpectEqString(MatchedChildren("li:nth-child(2)", mixed, "ul"), "",
                   "and the second child is not an li at all");
    ExpectEqString(MatchedChildren("span:nth-of-type(odd)", mixed, "ul"), "2 ",
                   "odd within the spans, not within the children");
    ExpectEqString(MatchedChildren("li:nth-last-of-type(1)", mixed, "ul"), "5 ",
                   "the last of its type");
  });

  AddTest(tests, "CssSelector/AnInvalidAnPlusBDropsItsRuleRatherThanGuessing", [] {
    // Every one of these is a selector no page meant to write. Matching them
    // permissively would apply a rule to elements the author never named, which
    // is harder to diagnose than the rule going missing.
    for (const std::string_view text : {
             "li:nth-child()", "li:nth-child(2n 1)", "li:nth-child(+ n)", "li:nth-child(n+)",
             "li:nth-child(2n+)", "li:nth-child(an+b)", "li:nth-child(2.5n)", "li:nth-child(1.5)",
             "li:nth-child(2n+1 extra)", "li:nth-child(99999999999)",
             "li:nth-child(2n-)", "li:nth-child(odd even)",
             // `of S` is only on the `-child` forms: `:nth-of-type()` already
             // says which sequence it counts over, and two answers is not a
             // grammar.
             "li:nth-of-type(2n of .x)", "li:nth-child(2n of)", "li:nth-child(2n of !)",
         }) {
      Expect(!SelectorParses(text), std::string("must not parse: ") + std::string(text));
    }
    // And the functional pseudo-classes this engine still does not implement go
    // the same way rather than becoming a stub that feature detection believes.
    Expect(!SelectorParses("p:nth-col(2)"), ":nth-col() is not implemented");
    Expect(!SelectorParses("p:not(!)"), "an argument that is not a selector invalidates :not()");
  });

  AddTest(tests, "CssSelector/NestedSelectorListsAreBounded", [] {
    // A stylesheet is attacker-controlled input and the parser recurses over
    // it, so the nesting is bounded for the same reason ADR 0009 bounds script.
    const auto nested = [](int depth) {
      std::string text;
      for (int i = 0; i < depth; ++i) {
        text += ":not(";
      }
      text += "a";
      text.append(static_cast<std::size_t>(depth), ')');
      return text;
    };
    Expect(SelectorParses(nested(css::kMaxSelectorNestingDepth)), "the bound itself parses");
    Expect(!SelectorParses(nested(css::kMaxSelectorNestingDepth + 1)),
           "one past it does not, rather than recursing as deep as the input asks");
  });

  AddTest(tests, "CssSelector/NotInvertsAPseudoClassTheEngineDoesNotImplement", [] {
    // Recorded because it is a decision rather than an accident. An
    // unimplemented pseudo-class never matches, so `:not()` of one always
    // does. For `:hover` that is the right answer -- in a snapshot nothing is
    // hovered, and reddit's video controls say `:not(:hover):not(:active)`
    // meaning exactly the resting state. For `:checked` it will be wrong until
    // ADR 0016 §2 makes that state a bit on the element.
    Expect(SelectorMatches("p:not(:hover)", "<p>a</p>", "p"),
           "nothing is hovered, so :not(:hover) holds");
    Expect(!SelectorMatches("p:hover", "<p>a</p>", "p"), "while :hover itself still matches nothing");
  });

  AddTest(tests, "Css/AnUnquotedUrlSurvivesReconstruction", [] {
    // `url(x.png)` scans as one token holding just the target, and reconstructing
    // it without the wrapper leaves a bare `x.png` that reads as an identifier
    // rather than a resource. Before this the two spellings of the same value
    // produced different declarations, so a stylesheet that omitted the quotes
    // -- which most do -- lost every background image it named.
    const StyleSheet unquoted = ParseStyleSheet("div { background-image: url(x.png) }");
    const StyleSheet quoted = ParseStyleSheet("div { background-image: url(\"x.png\") }");
    Expect(unquoted.rules.size() == 1 && quoted.rules.size() == 1, "one rule each");
    Expect(!unquoted.rules[0].declarations.empty(), "with a declaration");
    ExpectEqString(unquoted.rules[0].declarations[0].value,
                   quoted.rules[0].declarations[0].value,
                   "the quoted and unquoted spellings mean the same thing");
  });

  AddTest(tests, "Css/BackgroundOriginAndClipAreTwoDifferentBoxes", [] {
    // **Their initial values are different boxes and that is the point.**
    // `background-origin` is `padding-box`, so an image starts inside the
    // border; `background-clip` is `border-box`, so what runs past the padding
    // edge still shows *under* a dashed or translucent border. This renderer
    // used the border box for both, which put every background image on a
    // bordered element a border-width up and to the left of where CSS says it
    // goes -- a default-case bug that no test could see while neither property
    // existed.
    ComputedStyle parent;
    ComputedStyle style;
    Expect(style.background.origin == css::BackgroundBox::PaddingBox, "origin starts at the padding box");
    Expect(style.background.clip == css::BackgroundBox::BorderBox, "and clip at the border box");

    Expect(ApplyDeclaration(Declaration{"background-origin", "content-box"}, parent, style),
           "background-origin takes a box");
    Expect(style.background.origin == css::BackgroundBox::ContentBox, "and stores the one written");
    Expect(style.background.clip == css::BackgroundBox::BorderBox, "without touching the other property");

    // A list is one value: every entry has to parse or the declaration is
    // invalid in full. A loop that stopped at the first entry would accept
    // `content-box, nonsense` and paint a content-box clip for it.
    Expect(ApplyDeclaration(Declaration{"background-clip", "content-box, border-box"}, parent, style),
           "a layer list of boxes is a value");
    Expect(style.background.clip == css::BackgroundBox::ContentBox, "the first layer is the one painted");
    Expect(!ApplyDeclaration(Declaration{"background-clip", "content-box, nonsense"}, parent, style),
           "and an entry that does not parse invalidates the whole list");
    Expect(style.background.clip == css::BackgroundBox::ContentBox, "leaving what was there");

    // `text` is refused rather than approximated: it clips the background to the
    // glyphs, and accepting it would make `@supports (background-clip: text)`
    // say yes to a page whose whole design then depends on it (ADR 0012).
    Expect(!ApplyDeclaration(Declaration{"background-clip", "text"}, parent, style),
           "background-clip: text is an honest no");

    // The shorthand resets both, and to their own initial values rather than to
    // a shared one -- resetting both to the same box would move every
    // background image on the page by a border width.
    Expect(ApplyDeclaration(Declaration{"background", "red"}, parent, style), "the shorthand applies");
    Expect(style.background.origin == css::BackgroundBox::PaddingBox &&
               style.background.clip == css::BackgroundBox::BorderBox,
           "and puts each back to its own initial value");
  });

  AddTest(tests, "Css/BackgroundPositionHasTwoLonghandsAndTheyAreNotInterchangeable", [] {
    // The keywords are per-axis: `right` is 100% horizontally and is not a value
    // at all vertically. That is what makes these two properties rather than one
    // indexed one, and it is why the parser takes an axis.
    ComputedStyle parent;
    ComputedStyle style;
    Expect(ApplyDeclaration(Declaration{"background-position-x", "right"}, parent, style),
           "`right` is a horizontal position");
    Expect(style.background.position_x.unit == css::Length::Unit::Percent &&
               style.background.position_x.value == 100.0f,
           "and resolves to 100%");
    Expect(!ApplyDeclaration(Declaration{"background-position-y", "right"}, parent, style),
           "while `right` is not a vertical one");
    Expect(ApplyDeclaration(Declaration{"background-position-y", "bottom"}, parent, style),
           "`bottom` is");
    Expect(style.background.position_y.unit == css::Length::Unit::Percent &&
               style.background.position_y.value == 100.0f,
           "and is the same 100%");
    // The two-word edge-offset form is refused rather than half-read: reading
    // only `right` would put the image at the far edge and silently drop the
    // offset the page asked for.
    Expect(!ApplyDeclaration(Declaration{"background-position-x", "right 10px"}, parent, style),
           "`right 10px` is an edge offset this renderer cannot express");
  });

  // --- @font-face, ADR 0024 --------------------------------------------------

  AddTest(tests, "Css/AtFontFaceIsADescriptorBlockRatherThanARule", [] {
    // It matches nothing and styles nothing: it adds a face to the font database.
    // Which is why it is a separate list on the sheet, and why the rule count does
    // not move.
    const StyleSheet sheet = ParseStyleSheet(
        "@font-face { font-family: Inter; src: url(inter.woff2) format(\"woff2\"),"
        " url(inter.woff) format(\"woff\"); font-weight: 700; font-style: italic;"
        " font-display: swap; unicode-range: U+0000-00FF }"
        "p { color: red }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 1, "one rule, the p");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 1, "and one face");
    const css::FontFace& face = sheet.font_faces.at(0);
    ExpectEqString(face.family, "Inter", "the family a font stack will name");
    ExpectEqInt(face.weight, 700, "bold, as a number");
    Expect(face.italic, "and italic");
    ExpectEqString(face.display, "swap", "font-display, lowercased");
    // **The range itself now, where this used to assert only that one existed.**
    // The reason it could not before was in the tokenizer: `U+0000-00FF` scanned as
    // an ident, a number and a dimension, and neither the leading zeros nor the hex
    // reading survived. `Token::Kind::UnicodeRange` scans it where the text still is.
    ExpectEqInt(static_cast<long long>(face.unicode_ranges.size()), 1, "one range");
    ExpectEqInt(static_cast<long long>(face.unicode_ranges.at(0).first), 0x0000, "from");
    ExpectEqInt(static_cast<long long>(face.unicode_ranges.at(0).last), 0x00FF, "to");
    // The order is the author's fallback chain: the first decodable one wins, and
    // the format hint is what lets an undecodable entry be skipped *without*
    // fetching it.
    ExpectEqInt(static_cast<long long>(face.sources.size()), 2, "two sources");
    ExpectEqString(face.sources.at(0).url, "inter.woff2", "in order");
    ExpectEqString(face.sources.at(0).format, "woff2", "with its format");
    ExpectEqString(face.sources.at(1).url, "inter.woff", "then the fallback");
  });

  AddTest(tests, "Css/UnicodeRangeIsScannedInEveryFormItIsWrittenIn", [] {
    // Google Fonts serves eight `@font-face` blocks per family, distinguished only
    // by this descriptor, so every spelling below is one a real stylesheet uses.
    const StyleSheet sheet = ParseStyleSheet(
        "@font-face { font-family: A; src: url(a.ttf);"
        " unicode-range: U+0-7F, U+4??, u+0100-024F, U+2C60-2C7F }");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 1, "one face");
    const std::vector<css::UnicodeRange>& ranges = sheet.font_faces.at(0).unicode_ranges;
    ExpectEqInt(static_cast<long long>(ranges.size()), 4, "four ranges");
    ExpectEqInt(static_cast<long long>(ranges.at(0).first), 0x0, "a short single run");
    ExpectEqInt(static_cast<long long>(ranges.at(0).last), 0x7F, "reads as itself");
    // The wildcard form is a range in disguise, expanded in the tokenizer so that
    // nothing downstream has to know what `?` means.
    ExpectEqInt(static_cast<long long>(ranges.at(1).first), 0x400, "U+4?? starts at 400");
    ExpectEqInt(static_cast<long long>(ranges.at(1).last), 0x4FF, "and ends at 4FF");
    ExpectEqInt(static_cast<long long>(ranges.at(2).first), 0x100, "lowercase u+ too");
    ExpectEqInt(static_cast<long long>(ranges.at(2).last), 0x24F, "with both ends");
    ExpectEqInt(static_cast<long long>(ranges.at(3).last), 0x2C7F, "and the last entry");
  });

  AddTest(tests, "Css/AUnicodeRangeIsNotAnIdentifierAndViceVersa", [] {
    // `u` is a legal identifier and this must not eat one. The value below has no
    // range in it at all, and a face with no range covers everything -- so reading
    // one here would silently *narrow* a face that claimed the whole alphabet.
    const StyleSheet sheet = ParseStyleSheet(
        "@font-face { font-family: A; src: url(a.ttf); unicode-range: u }");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 1, "the face is kept");
    Expect(sheet.font_faces.at(0).unicode_ranges.empty(), "with no range read out of it");
    // And a declaration elsewhere that begins with `u` still parses as a value.
    const StyleSheet other = ParseStyleSheet("p { font-family: u }");
    ExpectEqInt(static_cast<long long>(other.rules.size()), 1, "an ordinary rule");
    ExpectEqString(other.rules.at(0).declarations.at(0).value, "u", "keeps its value");
  });

  AddTest(tests, "Css/TransformIsParsedAsOperationsRatherThanAMatrix", [] {
    const StyleSheet sheet = ParseStyleSheet(
        "p { transform: translate(10px, 50%) rotate(90deg) scale(2) }");
    const std::vector<Declaration>& declarations = sheet.rules.at(0).declarations;
    ComputedStyle parent;
    ComputedStyle style;
    Expect(ApplyDeclaration(declarations.at(0), parent, style),
           "the declaration applies");
    ExpectEqInt(static_cast<long long>(style.transform.operations.size()), 3, "three operations");
    // Kept as operations because a percentage translation resolves against the box's
    // own border box, which the cascade does not know -- and because interpolating a
    // rotation needs the rotation rather than the matrix it became.
    const css::TransformOperation& translate = style.transform.operations.at(0);
    Expect(translate.kind == css::TransformOperation::Kind::Translate, "translate first");
    ExpectEqInt(static_cast<long long>(translate.length_x.value), 10, "10px");
    Expect(translate.length_y.IsPercent(), "and a percentage that stayed one");
    const css::TransformOperation& rotate = style.transform.operations.at(1);
    Expect(rotate.kind == css::TransformOperation::Kind::Rotate, "then rotate");
    Expect(std::abs(rotate.a - 1.5707964f) < 1e-5f, "90deg is a quarter turn in radians");
    const css::TransformOperation& scale = style.transform.operations.at(2);
    // `scale(2)` is uniform where `translate(10px)` leaves the other axis at zero:
    // the two functions disagree about a missing argument, and both are tested.
    Expect(scale.kind == css::TransformOperation::Kind::Scale, "then scale");
    Expect(scale.a == 2.0f && scale.b == 2.0f, "which is uniform from one argument");
  });

  AddTest(tests, "Css/OneBadFunctionDropsTheWholeTransform", [] {
    // The specification's error rule, and the safe direction: half a transform puts
    // the box somewhere the author never wrote, while a dropped one leaves it where
    // layout put it.
    ComputedStyle parent;
    ComputedStyle style;
    Expect(!ApplyDeclaration(Declaration{"transform", "translate(10px) rotate(oops)"}, parent, style),
           "the declaration is refused");
    Expect(style.transform.IsNone(), "and nothing of it was kept");
    // 3D is refused rather than flattened. `rotateY(90deg)` flattened to 2D is a box
    // at full width where the page meant an edge-on sliver -- a wrong page rather
    // than a missing effect.
    Expect(!ApplyDeclaration(Declaration{"transform", "rotateY(90deg)"}, parent, style), "3D is refused");
    Expect(!ApplyDeclaration(Declaration{"transform", "translate3d(1px, 2px, 3px)"}, parent, style),
           "including the translate that looks 2D");
    Expect(style.transform.IsNone(), "and none of it applied");
    Expect(ApplyDeclaration(Declaration{"transform", "translate3d(-100%, 0, 0)"}, parent, style),
           "Z of zero is a 2D translate (Polymer drawers)");
    Expect(!style.transform.IsNone(), "drawer translate kept");
  });

  AddTest(tests, "CssParser/VisibilityInheritsAndApplies", [] {
    css::ComputedStyle parent = css::StyleResolver::InitialStyle();
    css::ComputedStyle child = css::StyleResolver::InitialStyle();
    Expect(ApplyDeclaration(Declaration{"visibility", "hidden"}, parent, parent), "host hidden");
    Expect(parent.visibility == css::Visibility::Hidden, "applied");
    css::InheritInto(parent, child);
    Expect(child.visibility == css::Visibility::Hidden, "inherited");
    Expect(ApplyDeclaration(Declaration{"visibility", "visible"}, parent, child), "descendant");
    Expect(child.visibility == css::Visibility::Visible, "can re-show under hidden");
  });

  AddTest(tests, "CssParser/OpacityAppliesAndDoesNotInherit", [] {
    css::ComputedStyle parent = css::StyleResolver::InitialStyle();
    css::ComputedStyle child = css::StyleResolver::InitialStyle();
    Expect(ApplyDeclaration(Declaration{"opacity", "0"}, parent, parent), "zero");
    Expect(parent.opacity == 0.0f, "applied");
    Expect(ApplyDeclaration(Declaration{"opacity", "50%"}, parent, parent), "percent");
    Expect(std::abs(parent.opacity - 0.5f) < 1e-6f, "half");
    Expect(ApplyDeclaration(Declaration{"opacity", "2"}, parent, parent), "clamp high");
    Expect(parent.opacity == 1.0f, "clamped to one");
    Expect(!ApplyDeclaration(Declaration{"opacity", "nope"}, parent, parent), "junk refused");
    Expect(css::SupportsDeclaration("opacity", "0"), "@supports sees it");
    parent.opacity = 0.25f;
    css::InheritInto(parent, child);
    Expect(child.opacity == 1.0f, "not inherited — child stays initial");
  });

  AddTest(tests, "Css/ATransformResolvesAboutItsOriginAndNotTheBoxCorner", [] {
    // The default origin is the centre of the border box, which is why a rotation
    // looks right without the author saying anything -- and why applying the origin
    // is `TransformList`'s job rather than each caller's.
    ComputedStyle style;
    ComputedStyle parent;
    Expect(ApplyDeclaration(Declaration{"transform", "rotate(90deg)"}, parent, style), "rotate applies");
    const gfx::FloatSize size{100.0f, 40.0f};
    const gfx::FloatPoint centre{50.0f, 20.0f};
    const gfx::AffineTransform about_centre = style.transform.ToMatrix(size, centre, 16.0f);
    const gfx::FloatPoint mapped = about_centre.MapPoint(centre);
    Expect(std::abs(mapped.x - centre.x) < 1e-3f && std::abs(mapped.y - centre.y) < 1e-3f,
           "the origin is the one point a rotation leaves alone");
    // And the same rotation about the corner moves the centre, which is the
    // difference `transform-origin` names.
    const gfx::AffineTransform about_corner =
        style.transform.ToMatrix(size, gfx::FloatPoint{0.0f, 0.0f}, 16.0f);
    const gfx::FloatPoint moved = about_corner.MapPoint(centre);
    Expect(std::abs(moved.x - centre.x) > 1.0f || std::abs(moved.y - centre.y) > 1.0f,
           "about the corner it does not");
  });

  AddTest(tests, "Css/TransformOriginTakesKeywordsAndOneValueMeansCentredVertically", [] {
    ComputedStyle style;
    ComputedStyle parent;
    Expect(ApplyDeclaration(Declaration{"transform-origin", "left top"}, parent, style), "keywords");
    Expect(style.transform_origin_x.IsPercent() && style.transform_origin_x.value == 0.0f, "left");
    Expect(style.transform_origin_y.IsPercent() && style.transform_origin_y.value == 0.0f, "top");
    Expect(ApplyDeclaration(Declaration{"transform-origin", "10px"}, parent, style), "one length");
    ExpectEqInt(static_cast<long long>(style.transform_origin_x.value), 10, "x is the length");
    Expect(style.transform_origin_y.IsPercent() && style.transform_origin_y.value == 50.0f,
           "and the missing axis is centred rather than zero");
  });

  AddTest(tests, "Css/TransformIsPaintOnlyAndTheInvalidationIndexKnowsIt", [] {
    // ADR 0016 §3. A transformed box occupies the space it would have occupied
    // untransformed, so a `:hover` rule that only transforms must not relayout the
    // page -- which is most of what makes `transform` worth having over `top`/`left`.
    Expect(!css::PropertyAffectsLayout("transform"), "transform is paint-only");
    Expect(!css::PropertyAffectsLayout("transform-origin"), "and so is its origin");
    Expect(css::PropertyAffectsLayout("width"), "while width is not, for contrast");
  });

  AddTest(tests, "Css/AFontFaceWithNoFamilyOrNoSourceIsSkipped", [] {
    // It names nothing and fetches nothing, and counting it is how a page whose
    // font never appears finds out the browser read the block and found it empty.
    const StyleSheet sheet = ParseStyleSheet(
        "@font-face { src: url(a.woff2) }"
        "@font-face { font-family: Nope }"
        "@font-face { font-family: Yes; src: url(b.woff2) }");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 1, "only the complete one");
    ExpectEqInt(static_cast<long long>(sheet.skipped), 2, "and the other two are counted");
  });

  AddTest(tests, "Css/AFontFaceDefaultsToNormalWeightAndUpright", [] {
    const StyleSheet sheet =
        ParseStyleSheet("@font-face { font-family: A; src: url(a.woff2) }");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 1, "one face");
    // Defaulted rather than absent: a face with no `font-weight` *is* a
    // normal-weight face, and an absent value meaning "any" would make one face
    // answer for all nine.
    ExpectEqInt(sheet.font_faces.at(0).weight, 400, "normal");
    Expect(!sheet.font_faces.at(0).italic, "and upright");
  });

  AddTest(tests, "Css/AVariableFontsWeightRangeTakesItsFirstValue", [] {
    // `font-weight: 100 900` is a variable font. Taken as 100 rather than
    // rejected: the face still renders, and refusing it would drop a working font
    // over a descriptor this browser cannot vary along.
    const StyleSheet sheet = ParseStyleSheet(
        "@font-face { font-family: A; src: url(a.woff2); font-weight: 100 900 }"
        "@font-face { font-family: B; src: url(b.woff2); font-weight: bold }");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 2, "both faces");
    ExpectEqInt(sheet.font_faces.at(0).weight, 100, "the range's first value");
    ExpectEqInt(sheet.font_faces.at(1).weight, 700, "and the keyword");
  });

  AddTest(tests, "Css/ALocalSourceIsSkippedRatherThanAnswered", [] {
    // `local(...)` names a font on the *machine*, and answering it from the system
    // database would let a page ask which fonts are installed -- the
    // fingerprinting surface ADR 0029 prices separately.
    const StyleSheet sheet = ParseStyleSheet(
        "@font-face { font-family: A; src: local(\"Helvetica\"), url(a.woff2) }");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.size()), 1, "the face survives");
    ExpectEqInt(static_cast<long long>(sheet.font_faces.at(0).sources.size()), 1,
                "with only the URL source");
    ExpectEqString(sheet.font_faces.at(0).sources.at(0).url, "a.woff2", "the one it can fetch");
  });

  AddTest(tests, "Cssom/SerializesSelectorsTheCssomWay", [] {
    ExpectEqString(css::SerializeSelectorList("div"), "div", "type");
    ExpectEqString(css::SerializeSelectorList(".style1"), ".style1", "class");
    ExpectEqString(css::SerializeSelectorList("#container"), "#container", "id");
    ExpectEqString(css::SerializeSelectorList("*"), "*", "universal");
    ExpectEqString(css::SerializeSelectorList("div > span"), "div > span", "child");
    ExpectEqString(css::SerializeSelectorList(":after"), "::after", "legacy pseudo-element");
    ExpectEqString(css::SerializeSelectorList("div.class"), "div.class", "compound drops star");
    ExpectEqString(css::SerializeSelectorList("[|lang]"), "[lang]", "null-namespace attribute");
    ExpectEqString(css::SerializeSelectorList("*|div"), "div", "any-namespace type drops star-pipe");
    ExpectEqString(css::SerializeSelectorList(":nth-child(even)"), ":nth-child(2n)",
                   "even is 2n");
    ExpectEqString(css::SerializeSelectorList(":nth-child(odd)"), ":nth-child(2n+1)",
                   "odd is 2n+1, with the plus that keeps the two tokens apart");
    const std::vector<css::CssomRule> nth = css::ParseCssom(":nth-child(odd) { color: blue; }");
    ExpectEqInt(static_cast<long long>(nth.size()), 1, "odd survives a CSSOM round-trip");
    ExpectEqString(nth.at(0).prelude, ":nth-child(2n+1)", "as the serialized An+B");
    const std::vector<css::CssomRule> foo_rule = css::ParseCssom("foo { color: blue; }");
    ExpectEqInt(static_cast<long long>(foo_rule.size()), 1,
                "a type selector with a colour is one CSSOM rule");
    ExpectEqString(foo_rule.at(0).prelude, "foo", "the selector survives");
    ExpectEqString(css::SerializeSelectorList("!!"), "", "unparseable is empty");
  });

  AddTest(tests, "Cssom/NamespacedSelectorsSerializeWithDefaultNamespace", [] {
    const std::vector<css::CssomRule> no_default = css::ParseCssom(
        "@namespace ns url(ns); *|e { x: 1 } ns|e { x: 1 } |e { x: 1 }");
    ExpectEqInt(static_cast<long long>(no_default.size()), 4, "namespace plus three style rules");
    ExpectEqString(no_default[1].prelude, "e", "any-namespace drops star-pipe without default");
    ExpectEqString(no_default[2].prelude, "ns|e", "named prefix is kept");
    ExpectEqString(no_default[3].prelude, "|e", "no-namespace type keeps the pipe");

    const std::vector<css::CssomRule> with_default = css::ParseCssom(
        "@namespace url(default_ns); @namespace ns url(ns); *|e { x: 1 } e { x: 1 }");
    ExpectEqString(with_default[2].prelude, "*|e", "any-namespace keeps star-pipe with default");
    ExpectEqString(with_default[3].prelude, "e", "unprefixed type stays unprefixed");
  });

  AddTest(tests, "Cssom/DeclarationBlockCollapsesShorthands", [] {
    ExpectEqString(css::SerializeCssDeclarationBlock({{"overflow-x", "scroll", false},
                                                      {"overflow-y", "hidden", false}}),
                   "overflow: scroll hidden;", "overflow longhands");
    ExpectEqString(css::SerializeCssDeclarationBlock({{"overflow-x", "scroll", false},
                                                      {"overflow-y", "scroll", false}}),
                   "overflow: scroll;", "matching overflow longhands");
    ExpectEqString(css::SerializeCssDeclarationBlock({{"margin-top", "1px", false},
                                                      {"margin-right", "2px", false},
                                                      {"margin-bottom", "3px", false},
                                                      {"margin-left", "4px", false}}),
                   "margin: 1px 2px 3px 4px;", "margin longhands");
    ExpectEqString(css::SpecifiedShorthandValue(
                       "overflow", {{"overflow-x", "scroll", false}, {"overflow-y", "hidden", false}}),
                   "scroll hidden", "overflow getter");
  });

  AddTest(tests, "Cssom/ParseKeepsMediaNestedAndCanonicalisesSelectors", [] {
    const std::vector<css::CssomRule> rules =
        css::ParseCssom("DIV { color: red } @media print { a { color: green } }");
    ExpectEqInt(static_cast<long long>(rules.size()), 2, "two top-level rules");
    ExpectEqInt(static_cast<long long>(rules[0].type), 1, "style");
    ExpectEqString(rules[0].prelude, "div", "selector is serialized");
    ExpectEqInt(static_cast<long long>(rules[1].type), 4, "media");
    ExpectEqInt(static_cast<long long>(rules[1].children.size()), 1, "nested, not flattened");
  });

  AddTest(tests, "Cssom/ParseKeepsPageMarginRulesAndDeclarations", [] {
    const std::vector<css::CssomRule> rules =
        css::ParseCssom("@page { margin: 1in; @top-left { content: none } }");
    ExpectEqInt(static_cast<long long>(rules.size()), 1, "one page rule");
    ExpectEqInt(static_cast<long long>(rules[0].type), 6, "page");
    ExpectEqInt(static_cast<long long>(rules[0].declarations.size()), 1, "page descriptors");
    ExpectEqString(rules[0].declarations[0].property, "margin", "descriptor name");
    ExpectEqInt(static_cast<long long>(rules[0].children.size()), 1, "margin at-rule");
    ExpectEqInt(static_cast<long long>(rules[0].children[0].type), 9, "margin");
    ExpectEqString(rules[0].children[0].at_name, "top-left", "margin name");
  });

  AddTest(tests, "Cssom/SpecifiedValuesSerializeCanonically", [] {
    // CSSOM specified-value serialization, not computed: `.1em` is `0.1em`, a
    // quoted family that is a valid ident sequence drops its quotes, and a
    // functional colour keeps the alpha the page wrote rather than an 8-bit
    // channel. css/cssom/serialize-values.html is 172 subtests of this.
    const auto canon = [](std::string_view property, std::string_view value) {
      std::string out;
      const css::DeclarationValidity result =
          css::CanonicaliseDeclaration(property, value, &out);
      Expect(result != css::DeclarationValidity::Invalid,
             std::string(property) + " " + std::string(value) + " parsed");
      return out.empty() ? std::string(value) : out;
    };
    ExpectEqString(canon("width", ".1em"), "0.1em", "leading zero on a number");
    ExpectEqString(canon("width", "-.1em"), "-0.1em", "and on a negative");
    ExpectEqString(canon("line-height", "-0"), "0", "minus zero is zero");
    ExpectEqString(canon("background-image", "url(http://localhost/)"),
                   "url(\"http://localhost/\")", "url() quotes its argument");
    ExpectEqString(canon("content", "'string'"), "\"string\"", "strings use double quotes");
    ExpectEqString(canon("content", "counter(par-num, decimal)"), "counter(par-num)",
                   "counter() drops the default list style");
    ExpectEqString(canon("color", "rgba(5, 7, 10, 0.5)"), "rgba(5, 7, 10, 0.5)",
                   "specified alpha is not an 8-bit channel");
    ExpectEqString(canon("color", "#000"), "rgb(0, 0, 0)", "hash still becomes rgb()");
    ExpectEqString(canon("outline-color", "invert"), "invert", "outline-color's extra keyword");
    ExpectEqString(canon("font-family", "'Lucida Grande'"), "Lucida Grande",
                   "quoted ident sequence drops quotes");
    ExpectEqString(canon("font-family", "'34J'"), "\"34J\"", "digit-starting names stay quoted");
    ExpectEqString(canon("font-family", "\"serif\""), "\"serif\"",
                   "quoted generic family stays a family name");
    ExpectEqString(css::SerializeCssIdent("-"), "\\-", "a lone dash is backslash-escaped");
    ExpectEqString(css::SerializeCssIdent("0a"), "\\30 a", "a leading digit is hex-escaped");
    ExpectEqString(css::SerializeCssIdent("-0a"), "-\\30 a", "and after a leading dash too");
  });

  AddTest(tests, "Css/SvgPresentationPropertiesAreOrdinaryProperties", [] {
    // ADR 0043 §2. Before this, `getComputedStyle(rect).fill` was the empty
    // string -- which is not "black" and not "unknown", it is the answer a
    // property that does not exist gives.
    const ComputedStyle parent;
    ComputedStyle style;
    Expect(ApplyDeclaration(Declaration{"fill", "rebeccapurple"}, parent, style), "fill parses");
    ExpectEqInt(static_cast<long long>(style.svg.fill.color.Red()), 102, "and is the right colour");
    Expect(style.svg.fill.kind == css::SvgStyle::PaintKind::Color, "as a colour");
    Expect(ApplyDeclaration(Declaration{"fill", "url(#grad)"}, parent, style), "a reference parses");
    Expect(style.svg.fill.kind == css::SvgStyle::PaintKind::Reference &&
               style.svg.fill.reference == "#grad",
           "and is kept as text rather than resolved -- the cascade cannot see the tree");
    Expect(ApplyDeclaration(Declaration{"stroke", "none"}, parent, style), "`none` is a paint");
    Expect(style.svg.stroke.kind == css::SvgStyle::PaintKind::None, "and its own kind");

    // A bare number is a length here and nowhere else in CSS. `stroke-width="2"`
    // is how the property is written more often than not, and refusing it would
    // drop the common case.
    Expect(ApplyDeclaration(Declaration{"stroke-width", "2"}, parent, style), "a unitless width");
    ExpectEqInt(static_cast<long long>(style.svg.stroke_width.Resolve(16.0f)), 2, "is 2px");
    Expect(!ApplyDeclaration(Declaration{"stroke-width", "-1"}, parent, style), "negative is invalid");

    // Clamped rather than refused, which is the difference between a value out
    // of range and a value that is not one.
    Expect(ApplyDeclaration(Declaration{"fill-opacity", "2"}, parent, style), "over 1 is accepted");
    Expect(style.svg.fill_opacity == 1.0f, "and clamped");
    Expect(ApplyDeclaration(Declaration{"fill-opacity", "50%"}, parent, style), "a percentage");
    Expect(style.svg.fill_opacity == 0.5f, "is a fraction");
    Expect(!ApplyDeclaration(Declaration{"stroke-miterlimit", "0.5"}, parent, style),
           "but a miterlimit below 1 is invalid rather than clamped -- the spec says so");

    // A dash pattern of zeros is an infinite loop in any stroker that takes it
    // at face value, so SVG makes it invalid rather than a no-op.
    Expect(ApplyDeclaration(Declaration{"stroke-dasharray", "4, 2 1"}, parent, style),
           "commas and spaces both separate");
    ExpectEqInt(static_cast<long long>(style.svg.stroke_dasharray.size()), 3, "three dashes");
    Expect(!ApplyDeclaration(Declaration{"stroke-dasharray", "0 0"}, parent, style), "all zeros");
    Expect(!ApplyDeclaration(Declaration{"stroke-dasharray", "1 -1"}, parent, style), "or negative");

    // `pixelated` belongs to `image-rendering` and to no other `*-rendering`
    // property. A shared enum would make that a check somebody has to remember.
    Expect(ApplyDeclaration(Declaration{"image-rendering", "pixelated"}, parent, style), "image");
    Expect(!ApplyDeclaration(Declaration{"shape-rendering", "pixelated"}, parent, style), "shape");
    Expect(!ApplyDeclaration(Declaration{"text-rendering", "pixelated"}, parent, style), "text");

    // `normal` and the full list are the same order and must be the same value:
    // nothing downstream may be able to tell which keyword the page wrote.
    ComputedStyle named;
    ComputedStyle spelled;
    Expect(ApplyDeclaration(Declaration{"paint-order", "normal"}, parent, named), "normal");
    Expect(ApplyDeclaration(Declaration{"paint-order", "fill stroke markers"}, parent, spelled),
           "the same order spelled out");
    ExpectEqInt(named.svg.paint_order, spelled.svg.paint_order, "is the same value");
    Expect(!ApplyDeclaration(Declaration{"paint-order", "fill fill"}, parent, style), "a repeat");

    // Every one of them inherits, which is SVG's painting model: a `<g>` sets
    // the paint for everything under it.
    ComputedStyle painted;
    Expect(ApplyDeclaration(Declaration{"fill", "blue"}, parent, painted), "fill on the parent");
    ComputedStyle child;
    css::InheritInto(painted, child);
    ExpectEqInt(static_cast<long long>(child.svg.fill.color.Blue()), 255, "reaches the child");
  });

  AddTest(tests, "Css/IndividualTransformPropertiesAreTheirOwnProperties", [] {
    // CSS Transforms 2. `translate`, `rotate` and `scale` are separate
    // properties from `transform` -- separately animatable, separately reported
    // -- and they apply in that order whichever order the page declared them.
    const ComputedStyle parent;
    ComputedStyle style;
    Expect(ApplyDeclaration(Declaration{"translate", "10px 20px"}, parent, style), "translate");
    Expect(style.individual_transform.translate.has_value(), "is stored");
    Expect(ApplyDeclaration(Declaration{"rotate", "0 0 1 90deg"}, parent, style),
           "an explicit z axis is a z rotation");
    Expect(ApplyDeclaration(Declaration{"scale", "2"}, parent, style), "one number scales both");
    Expect(style.individual_transform.scale->a == 2.0f &&
               style.individual_transform.scale->b == 2.0f,
           "on both axes");
    Expect(ApplyDeclaration(Declaration{"translate", "none"}, parent, style), "`none` parses");
    Expect(!style.individual_transform.translate.has_value(),
           "and is the absent value rather than a zero one -- getComputedStyle must say `none`");

    // 3D is refused rather than flattened, which is the rule the transform
    // *functions* already follow: `rotateY(90deg)` flattened to 2D is a box at
    // full width where the page meant an edge-on sliver.
    Expect(!ApplyDeclaration(Declaration{"rotate", "x 45deg"}, parent, style), "a 3D axis");
    Expect(!ApplyDeclaration(Declaration{"rotate", "1 0 0 45deg"}, parent, style), "spelled out");
    Expect(!ApplyDeclaration(Declaration{"translate", "1px 2px 3px"}, parent, style), "a z offset");
    Expect(ApplyDeclaration(Declaration{"translate", "1px 2px 0"}, parent, style), "but a zero one");
    Expect(!ApplyDeclaration(Declaration{"scale", "1 1 2"}, parent, style), "a 3D scale");

    // The CSSOM serialization is of the *specified* value, so the unit survives:
    // `400grad` reads back as `400grad` and not as its degree equivalent. That is
    // why the canonicaliser works on tokens rather than on a parsed value.
    const auto canonical = [](const char* property, const char* value) {
      std::string out;
      const css::DeclarationValidity validity =
          css::CanonicaliseDeclaration(property, value, &out);
      return validity == css::DeclarationValidity::Invalid ? std::string("-") : out;
    };
    ExpectEqString(canonical("rotate", "0 0 1 400grad"), "400grad", "a z axis disappears");
    ExpectEqString(canonical("rotate", "0 0 -1 400grad"), "-400grad",
                   "and a negative one takes its sign into the angle");
    ExpectEqString(canonical("rotate", "400grad x"), "x 400grad", "the axis comes first");
    ExpectEqString(canonical("translate", "100px 0px"), "100px", "a zero y length goes");
    ExpectEqString(canonical("translate", "100px 0%"), "100px 0%", "a zero y percentage stays");
    ExpectEqString(canonical("translate", "0"), "0px", "a bare zero is a length");
    ExpectEqString(canonical("scale", "-100 -100 1"), "-100", "a trailing 1 and an equal y go");
    ExpectEqString(canonical("scale", "1%"), "0.01", "a percentage is a number");
  });
}

}  // namespace microbrowser::tests

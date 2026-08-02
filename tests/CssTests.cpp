#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/StyleSheet.h"
#include "css/Tokenizer.h"
#include "dom/Node.h"
#include "html/TreeBuilder.h"

namespace microbrowser::tests {

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
    const StyleSheet sheet = ParseStyleSheet("p::before { color: red } a { color: blue }");
    ExpectEqInt(static_cast<long long>(sheet.rules.size()), 1, "only the parsable rule remains");
    ExpectEqString(sheet.rules.at(0).selectors.at(0).compounds.at(0).parts.at(0).name, "a",
                   "and it is the right one");
    Expect(sheet.skipped > 0, "the drop is counted rather than silent");
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
}

}  // namespace microbrowser::tests

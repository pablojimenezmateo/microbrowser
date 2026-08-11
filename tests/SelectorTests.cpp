#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "html/TreeBuilder.h"

// The selectors D5 added: `:has()`, `An+B of S`, `:lang()`, `:dir()`, `:scope`,
// the attribute case flags, the namespace constraints, and the forgiving
// parsing of `:is()`/`:where()`.
//
// Separate from `CssTests.cpp` because it is a different kind of question: that
// file is about the parser producing the right *shape*, this one is about a
// selector picking the right *elements* out of a real parsed document. Almost
// every bug found while writing these was a matcher bug that a shape assertion
// would have passed.

namespace microbrowser::tests {

using css::ParseSelectorList;
using css::Selector;
using dom::Element;

namespace {

bool Parses(std::string_view selector_text) {
  return !ParseSelectorList(selector_text).empty();
}

// The `id`s a selector picks out of `html`, in document order, space separated.
// Ids rather than positions because these selectors are about relationships
// between elements at different depths, and a positional answer would say
// nothing about which of two nested candidates matched.
std::string Picked(std::string_view selector_text, std::string_view html) {
  const std::vector<Selector> selectors = ParseSelectorList(selector_text);
  Expect(!selectors.empty(), std::string("selector did not parse: ") + std::string(selector_text));
  const std::unique_ptr<dom::Document> document = html::ParseDocument(html);
  std::string out;
  document->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const Element&>(node);
    for (const Selector& selector : selectors) {
      if (selector.Matches(element)) {
        const std::string* id = element.GetAttribute("id");
        out += id == nullptr ? element.TagName() : *id;
        out.push_back(' ');
        return;
      }
    }
  });
  return out;
}

// A tree with an element at every relationship `:has()` can name.
constexpr std::string_view kRelations =
    "<div id=root>"
    "<div id=a><span id=a1 class=x></span></div>"
    "<div id=b><div id=b1><span id=b2 class=x></span></div></div>"
    "<div id=c></div>"
    "<p id=d class=x></p>"
    "</div>";

}  // namespace

void RegisterSelectorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CssHas/ReadsTheCombinatorOfItsLeftmostCompound", [] {
    // The four relative forms, and the thing that distinguishes them is the
    // combinator on compound *zero* -- the one every other selector ignores.
    ExpectEqString(Picked("div:has(.x)", kRelations), "root a b b1 ",
                   "descendant: every ancestor of an .x, however deep");
    ExpectEqString(Picked("div:has(> .x)", kRelations), "root a b1 ",
                   "child: only elements whose own child is .x");
    ExpectEqString(Picked("div:has(+ .x)", kRelations), "c ",
                   "next sibling: the div immediately before the .x paragraph");
    ExpectEqString(Picked("div:has(~ .x)", kRelations), "a b c ",
                   "later sibling: every div before it");
  });

  AddTest(tests, "CssHas/SearchesTheRightSpace", [] {
    // A sibling-relative argument must not be satisfied from inside the
    // element, and a descendant-relative one must not be satisfied from
    // outside it. Getting this wrong is not slow, it is wrong.
    ExpectEqString(Picked("#a:has(+ .x)", kRelations), "",
                   "a's .x is its child, not its sibling");
    ExpectEqString(Picked("#root:has(> span)", kRelations), "",
                   "root's spans are grandchildren");
    ExpectEqString(Picked("#b:has(div .x)", kRelations), "b ",
                   "a complex relative argument still anchors at the subject");
    ExpectEqString(Picked("#b:has(> .x)", kRelations), "", "and does not when it cannot");
    // The leading combinator speaks about the *leftmost* compound, not about
    // the subject -- so in `:has(> div .x)` the `>` is a claim about the `div`
    // and the `.x` may be arbitrarily deep under it. A cheap rejection applied
    // to the enumerated candidate instead made every multi-compound relative
    // selector match nothing, which is the bug this line exists for.
    ExpectEqString(Picked("#b:has(> div .x)", kRelations), "b ",
                   "the combinator constrains the leftmost compound");
    ExpectEqString(Picked("#b:has(> span .x)", kRelations), "",
                   "and the leftmost compound still has to match");
    ExpectEqString(Picked("#a:has(~ p)", kRelations), "a ", "a later sibling");
    ExpectEqString(Picked("#root:has(~ div .x)", kRelations), "",
                   "root has no siblings to look inside");
  });

  AddTest(tests, "CssHas/CombinesWithTheRestOfTheSelector", [] {
    ExpectEqString(Picked("#root > div:has(.x)", kRelations), "a b ",
                   ":has() on a subject that is itself the right-hand side of a combinator");
    ExpectEqString(Picked("div:has(.x):has(span)", kRelations), "root a b b1 ",
                   "two :has() in one compound both have to hold");
    ExpectEqString(Picked("div:not(:has(.x))", kRelations), "c ",
                   ":not(:has()) is legal and is the useful spelling");
    ExpectEqString(Picked("div:has(:not(.x))", kRelations), "root b ",
                   ":has(:not()) is the other way round and means something else");
  });

  AddTest(tests, "CssHas/IsUnforgivingAndCannotNest", [] {
    Expect(Parses(".a:has(.b)"), "the ordinary form");
    Expect(Parses(".a:has(> .b, + .c)"), "a list of relative selectors");
    Expect(!Parses(":has()"), "an empty argument is not a selector");
    Expect(!Parses(":has(.a, !)"), ":has() is unforgiving, unlike :is()");
    Expect(!Parses(":has(:has(.a))"), "nesting is forbidden by the grammar");
    // Forbidden at any depth, not only directly. Through `:not()`, which is
    // unforgiving, the refusal propagates all the way out; through `:is()` it
    // would only cost that one argument, which is what forgiving means.
    Expect(!Parses(":has(:not(:has(.a)))"), "forbidden through a nested selector list too");
    Expect(!Parses(":has"), "without an argument it is not a functional pseudo-class at all");
    // The leading combinator that makes a relative selector is only legal
    // inside one: `> div` on its own has a combinator nothing reads.
    Expect(!Parses("> div"), "a bare leading combinator is not a selector");
    Expect(!Parses("div > > p"), "and neither are two in a row");
  });

  AddTest(tests, "CssHas/TakesTheSpecificityOfItsArgumentAndAddsNothing", [] {
    const auto specificity = [](std::string_view text) {
      const std::vector<Selector> selectors = ParseSelectorList(text);
      Expect(!selectors.empty(), "parsed");
      return selectors.front().ComputeSpecificity();
    };
    Expect(specificity(":has(.foo)") == specificity(".baz"),
           ":has(.foo) ties with a bare class, so document order decides");
    Expect(specificity(".baz") < specificity(":has(#foo)"), "an id argument wins");
    Expect(specificity(":has(.foo, .bar)") == specificity(":has(.bar, .foo)"),
           "a list takes its most specific member, whichever order it is written in");
    Expect(specificity(":has(span)") < specificity(":has(span#foo)"), "and the most specific one");
  });

  AddTest(tests, "CssHas/StopsAtItsBoundRatherThanWalkingAnUnboundedSubtree", [] {
    // The one selector in this engine with a budget on it. Built here rather
    // than asserted from a page, because the point is what happens *past* the
    // bound and no real page comes near it -- see `kMaxHasCandidates` for the
    // measurement that put it two orders of magnitude above wikipedia.
    //
    // Wide rather than deep on purpose: a chain of a hundred thousand elements
    // would be testing the destructor's recursion depth as much as the bound.
    auto document = std::make_unique<dom::Document>();
    auto& root = static_cast<Element&>(document->Append(std::make_unique<Element>("div")));
    auto& branch = static_cast<Element&>(root.Append(std::make_unique<Element>("div")));
    for (std::size_t i = 0; i < css::kMaxHasCandidates + 8; ++i) {
      branch.Append(std::make_unique<Element>("span"));
    }
    static_cast<Element*>(branch.LastChild())->SetAttribute("class", "needle");

    const std::vector<Selector> selectors = ParseSelectorList("div:has(.needle)");
    Expect(!selectors.empty(), "parsed");
    Expect(!selectors.front().Matches(root),
           "past the bound the answer is no match, which is what an author gets "
           "from a selector that finds nothing rather than from one that finds everything");
    // And the same needle inside the bound is still found, so the bound is the
    // only thing that changed the answer.
    auto small = std::make_unique<dom::Document>();
    auto& small_root = static_cast<Element&>(small->Append(std::make_unique<Element>("div")));
    small_root.Append(std::make_unique<Element>("span"));
    static_cast<Element*>(small_root.LastChild())->SetAttribute("class", "needle");
    Expect(selectors.front().Matches(small_root), "the same selector on a small subtree matches");
  });

  AddTest(tests, "CssNth/CountsOverTheOfSequenceRatherThanEverySibling", [] {
    constexpr std::string_view kMixed =
        "<ul><li id=one class=a></li><li id=two></li><li id=three class=a></li>"
        "<li id=four class=a></li></ul>";
    ExpectEqString(Picked("li:nth-child(1 of .a)", kMixed), "one ",
                   "the first .a, which is also the first child");
    ExpectEqString(Picked("li:nth-child(2 of .a)", kMixed), "three ",
                   "the second .a is the third child");
    ExpectEqString(Picked("li:nth-child(2n of .a)", kMixed), "three ",
                   "even positions within the .a sequence");
    ExpectEqString(Picked("li:nth-last-child(1 of .a)", kMixed), "four ",
                   "counting the same sequence from the end");
    ExpectEqString(Picked(":nth-child(1 of .a)", kMixed), "one ",
                   "an element outside the sequence is never at a position in it");
    Expect(!Parses("li:nth-of-type(2 of .a)"), ":nth-of-type() already names its sequence");
  });

  AddTest(tests, "CssAttribute/AppliesTheCaseFlagsAndTheHtmlDefault", [] {
    constexpr std::string_view kAttrs = "<div id=t foo=BAR baz=quux data-x=Y></div>";
    ExpectEqString(Picked("[foo='bar' i]", kAttrs), "t ", "the i flag folds case");
    ExpectEqString(Picked("[foo='bar' I]", kAttrs), "t ", "and the flag itself is case-insensitive");
    ExpectEqString(Picked("[foo='bar']", kAttrs), "", "without it the comparison is exact");
    ExpectEqString(Picked("[baz='quux' s]", kAttrs), "t ", "the s flag is an explicit sensitive");
    ExpectEqString(Picked("[baz='QUUX' s]", kAttrs), "", "and means it");
    ExpectEqString(Picked("[data-x='y' i]", kAttrs), "t ", "flags work on every operator's operand");
    ExpectEqString(Picked("[foo^='ba' i]", kAttrs), "t ", "prefix");
    ExpectEqString(Picked("[foo$='ar' i]", kAttrs), "t ", "suffix");
    ExpectEqString(Picked("[foo*='a' i]", kAttrs), "t ", "substring");
    // HTML's own list: `type` is compared case-insensitively when the author
    // said nothing, which is why `Default` is a third state and not a synonym
    // for `Sensitive`.
    ExpectEqString(Picked("input[type='TEXT']", "<input id=i type=text>"), "i ",
                   "type is on HTML's case-insensitive list");
    ExpectEqString(Picked("input[type='TEXT' s]", "<input id=i type=text>"), "",
                   "and the s flag is the only thing that turns that off");
    ExpectEqString(Picked("[id='T']", kAttrs), "", "id is not on the list");
  });

  AddTest(tests, "CssAttribute/RejectsEverySpellingOfAFlagThatIsNotOne", [] {
    Expect(Parses("[foo='bar'i]"), "no space before the flag");
    Expect(Parses("[foo='bar' i ]"), "space after it");
    Expect(Parses("[foo=bar/**/i]"), "a comment where the space would be");
    Expect(!Parses("[foo='bar' i i]"), "two flags");
    Expect(!Parses("[foo='bar' ii]"), "an identifier that merely starts with one");
    Expect(!Parses("[foo='bar' j]"), "an identifier that is not one");
    Expect(!Parses("[foo='bar' 1]"), "a number");
    Expect(!Parses("[foo='bar' 'i']"), "a string");
    Expect(!Parses("[foo i]"), "a flag with nothing to be case-insensitive about");
    Expect(!Parses("[foo i ='bar']"), "a flag before the operator");
  });

  AddTest(tests, "CssNamespaces/AcceptTheTwoPrefixesThatNeedNoDeclaration", [] {
    constexpr std::string_view kAttrs = "<div id=t foo=bar></div>";
    ExpectEqString(Picked("[*|foo='bar']", kAttrs), "t ", "*| is any namespace");
    ExpectEqString(Picked("[|foo='bar']", kAttrs), "t ", "| is no namespace, which is what it is in");
    ExpectEqString(Picked("*|div", "<div id=t></div>"), "t ", "*|tag matches on the local name");
    // A *named* prefix needs an `@namespace` rule, and this engine has none --
    // so the selector is invalid rather than matching on a guess. Inside
    // `:is()` that costs only the one argument, which is what forgiving means.
    Expect(!Parses("svg|rect"), "a named prefix cannot be resolved");
    Expect(!Parses("[svg|href]"), "on an attribute either");
    Expect(Parses(":is(svg|rect, div)"), ":is() drops the argument and keeps the list");
    ExpectEqString(Picked(":is(svg|rect, #t)", kAttrs), "t ", "and the surviving argument matches");
  });

  AddTest(tests, "CssIsWhere/AreForgivingAndNotIsNot", [] {
    Expect(Parses(":is(.a, %%%, .b)"), ":is() keeps what it can parse");
    Expect(Parses(":where(%%%)"), "and is still a selector when it can parse nothing");
    ExpectEqString(Picked(":is(.a, %%%)", "<p id=t class=a></p>"), "t ",
                   "the surviving argument still matches");
    ExpectEqString(Picked(":is(%%%)", "<p id=t class=a></p>"), "",
                   "and an :is() with nothing left matches nothing");
    Expect(!Parses(":not(.a, %%%)"), ":not() is unforgiving");
    Expect(!Parses(":has(.a, %%%)"), "and so is :has()");
  });

  AddTest(tests, "CssLang/DoesRfc4647ExtendedFiltering", [] {
    constexpr std::string_view kLangs =
        "<div id=r lang=en>"
        "<p id=fx lang=fr-x></p><p id=fxs lang=fr-x-standard></p>"
        "<p id=fs lang=fr-standard></p><p id=f9 lang=fr-ninechars></p>"
        "<p id=exp lang=en-x-private></p><p id=inherit></p>"
        "</div>";
    ExpectEqString(Picked("p:lang(fr-x)", kLangs), "fx fxs ",
                   "a singleton subtag matches when the range names it");
    ExpectEqString(Picked("p:lang(fr-standard)", kLangs), "fs ",
                   "but cannot be skipped past: fr-x-standard is not fr-standard");
    ExpectEqString(Picked("p:lang(en-private)", kLangs), "",
                   "the same rule for the private-use singleton");
    ExpectEqString(Picked("p:lang(en-x)", kLangs), "exp ", "which is matchable by naming it");
    ExpectEqString(Picked("p:lang(fr)", kLangs), "fx fxs fs ",
                   "and fr-ninechars is not a language tag at all, so fr does not match it");
    ExpectEqString(Picked("p:lang(en)", kLangs), "exp inherit ",
                   "an element with no lang of its own inherits its ancestor's, and a "
                   "range that is a prefix of a whole tag matches it");
    ExpectEqString(Picked("p:lang(EN)", kLangs), "exp inherit ",
                   "a language tag is case-insensitive");
    ExpectEqString(Picked("p:lang(de, fr-standard)", kLangs), "fs ", "a list matches on any range");
    ExpectEqString(Picked("p:lang(*-standard)", kLangs), "fs ", "a wildcard subtag");
    Expect(!Parses(":lang()"), "an empty argument is not a selector");
    Expect(!Parses(":lang(en us)"), "and neither is a range with a space in it");
  });

  AddTest(tests, "CssDir/AnswersHtmlsDirectionalityAlgorithm", [] {
    constexpr std::string_view kDirs =
        "<div id=outer>"
        "<div id=plain></div>"
        "<div id=ltr dir=ltr><div id=under_ltr></div></div>"
        "<div id=rtl dir=rtl><div id=under_rtl></div><div id=back dir=ltr></div></div>"
        "<div id=bad dir=lol></div>"
        "<div id=auto_ltr dir=auto>hello</div>"
        "<div id=auto_rtl dir=auto>\xd7\xaa</div>"
        "<div id=auto_none dir=auto>123</div>"
        "</div>";
    ExpectEqString(Picked("div:dir(rtl)", kDirs), "rtl under_rtl auto_rtl ",
                   "an explicit rtl, what inherits it, and what auto resolves to rtl");
    ExpectEqString(Picked("#bad:dir(ltr)", kDirs), "bad ", "an invalid dir inherits");
    ExpectEqString(Picked("#auto_none:dir(ltr)", kDirs), "auto_none ",
                   "auto with no strong character is ltr");
    ExpectEqString(Picked("#back:dir(ltr)", kDirs), "back ", "and an inner dir wins over an outer");
    // `:dir(lol)` is a *valid* selector that matches nothing, where
    // `:dir('ltr')` is not a selector at all. The grammar and the match are
    // different questions and this is where the difference shows.
    Expect(Parses(":dir(lol)"), "any ident parses");
    ExpectEqString(Picked(":dir(lol)", kDirs), "", "and matches nothing");
    Expect(!Parses(":dir()"), "no argument");
    Expect(!Parses(":dir(ltr, rtl)"), "two arguments");
    Expect(!Parses(":dir('ltr')"), "a string rather than an ident");
  });

  AddTest(tests, "CssScope/NamesTheAnchorInsideHasAndTheRootOutsideIt", [] {
    ExpectEqString(Picked(":scope", kRelations), "html ",
                   "with nothing scoping the match, :scope is the document element -- "
                   "which the HTML parser supplies even when the markup does not");
    // `:has()` moves the *anchor*, not the scope. The implied `:scope` that
    // absolutizes a relative selector and the `:scope` pseudo-class an author
    // writes are two different things, and conflating them makes
    // `:has(:scope)` -- "which of my descendants contains the scoping root" --
    // answer "every element with a child" instead of "none".
    ExpectEqString(Picked("div:has(:scope)", kRelations), "",
                   "nothing under the root contains the root");
    ExpectEqString(Picked("html:has(:scope > body)", kRelations), "",
                   "and the root does not contain itself either");
  });
}

}  // namespace microbrowser::tests

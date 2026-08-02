#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/ComputedStyle.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "html/TreeBuilder.h"

namespace microbrowser::tests {

using css::ComputedStyle;
using css::Display;
using css::Length;
using css::Origin;
using css::ParseColor;
using css::ParseLength;
using css::ParseStyleSheet;
using css::StyleResolver;
using dom::Element;

namespace {

// Resolves the style of the first element with `tag` in `html`, after applying
// `author_css`.
ComputedStyle StyleOf(std::string_view html, std::string_view author_css,
                      std::string_view tag) {
  const std::unique_ptr<dom::Document> document = html::ParseDocument(html);
  StyleResolver resolver;
  resolver.AddStyleSheet(ParseStyleSheet(author_css), Origin::Author);

  ComputedStyle found;
  bool seen = false;
  resolver.ForEachStyledElement(*document, [&](const Element& element,
                                               const ComputedStyle& style) {
    if (!seen && element.TagName() == tag) {
      found = style;
      seen = true;
    }
  });
  Expect(seen, std::string("no element found: ") + std::string(tag));
  return found;
}

}  // namespace

void RegisterStyleResolverTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Css/ParsesColours", [] {
    Expect(ParseColor("red") == gfx::Color::Rgb(0xFF, 0, 0), "a named colour");
    Expect(ParseColor("#fff") == gfx::Color::Rgb(0xFF, 0xFF, 0xFF),
           "`#fff` is exactly white — each digit is doubled, not shifted, which a naive "
           "implementation renders as 0xF0F0F0");
    Expect(ParseColor("#1a2b3c") == gfx::Color::Rgb(0x1A, 0x2B, 0x3C), "six digits");
    Expect(ParseColor("#11223344") == gfx::Color::Rgba(0x11, 0x22, 0x33, 0x44), "eight digits");
    Expect(ParseColor("rgb(1, 2, 3)") == gfx::Color::Rgb(1, 2, 3), "rgb()");
    Expect(ParseColor("rgba(1,2,3,0.5)") == gfx::Color::Rgba(1, 2, 3, 128),
           "alpha is 0..1 rather than 0..255");
    Expect(ParseColor("transparent") == gfx::Color::Transparent(), "transparent");
    Expect(!ParseColor("notacolour").has_value(), "an unknown name is not a colour");
    Expect(!ParseColor("#12345").has_value(), "and neither is a five-digit hex");
  });

  AddTest(tests, "Css/ParsesLengths", [] {
    Expect(ParseLength("10px") == Length::Pixels(10.0f), "pixels");
    Expect(ParseLength("1.5em")->unit == Length::Unit::Em, "em keeps its unit for later");
    Expect(ParseLength("50%")->unit == Length::Unit::Percent,
           "a percentage cannot be resolved without a containing block, so it is carried");
    Expect(ParseLength("auto")->IsAuto(), "auto");
    Expect(ParseLength("0") == Length::Pixels(0.0f), "a unitless zero is a length");
    Expect(!ParseLength("5").has_value(),
           "but a unitless five is not: `width: 5` is an invalid declaration, and treating "
           "it as pixels renders differently from every other browser");
    Expect(ParseLength("12pt") == Length::Pixels(16.0f), "points convert at 4/3");
  });

  // Without a user-agent stylesheet a div is inline and the whole document is
  // one long line.
  AddTest(tests, "StyleResolver/AppliesTheUserAgentStyleSheet", [] {
    Expect(StyleOf("<div>x</div>", "", "div").display == Display::Block,
           "a div is block-level because the built-in sheet says so");
    Expect(StyleOf("<span>x</span>", "", "span").display == Display::Inline, "a span is inline");
    Expect(StyleOf("<p>x</p>", "", "p").display == Display::Block, "a paragraph is block");
    Expect(StyleOf("<b>x</b>", "", "b").font_weight > 400.0f, "b is bold");
    Expect(StyleOf("<h1>x</h1>", "", "h1").font_size > 16.0f, "h1 is larger than body text");
    Expect(StyleOf("<table><tr><td>x</td></tr></table>", "", "table").display ==
               Display::Table,
           "a table establishes table layout, not an ordinary block");
    Expect(StyleOf("<table><tr><td>x</td></tr></table>", "", "tr").display ==
               Display::TableRow,
           "a row stays a row for layout");
    Expect(StyleOf("<table><tr><td>x</td></tr></table>", "", "td").display ==
               Display::TableCell,
           "and cells are laid out across that row");
    Expect(StyleOf("<input>", "", "input").display == Display::InlineBlock,
           "an input is an inline-block control, not an invisible void element");
  });

  AddTest(tests, "StyleResolver/AuthorRulesBeatTheUserAgent", [] {
    Expect(StyleOf("<div>x</div>", "div { display: inline }", "div").display == Display::Inline,
           "an author rule wins, however specific the built-in one was");
  });

  // The cascade in full: origin, then important (which reverses origin), then
  // specificity, then document order.
  AddTest(tests, "StyleResolver/CascadesBySpecificityThenOrder", [] {
    Expect(StyleOf("<p class='c' id='i'>x</p>",
                   "p { color: red } .c { color: green } #i { color: blue }", "p")
                   .color == gfx::Color::Rgb(0, 0, 0xFF),
           "an id beats a class beats a type");

    Expect(StyleOf("<p class='c'>x</p>", ".c { color: red } .c { color: blue }", "p").color ==
               gfx::Color::Rgb(0, 0, 0xFF),
           "equal specificity is decided by document order, and the later one wins");

    Expect(StyleOf("<p class='a b c d e f g h i j k'>x</p>",
                   ".a.b.c.d.e.f.g.h.i.j.k { color: red } #none, p { color: blue } "
                   "p { color: green }",
                   "p")
                   .color == gfx::Color::Rgb(0xFF, 0, 0),
           "eleven classes beat a type selector, because specificity compares class counts "
           "before type counts");
  });

  AddTest(tests, "StyleResolver/ImportantReversesTheOrigins", [] {
    Expect(StyleOf("<p>x</p>", "p { color: red } p { color: blue !important }", "p").color ==
               gfx::Color::Rgb(0, 0, 0xFF),
           "an important declaration beats a later normal one");
    Expect(StyleOf("<p id='i'>x</p>", "#i { color: red } p { color: blue !important }", "p")
                   .color == gfx::Color::Rgb(0, 0, 0xFF),
           "and beats a more specific normal one, because important is compared before "
           "specificity");
  });

  AddTest(tests, "StyleResolver/TheStyleAttributeOutranksSelectors", [] {
    Expect(StyleOf("<p id='i' style='color: green'>x</p>", "#i { color: red }", "p").color ==
               gfx::Color::Rgb(0, 0x80, 0),
           "an inline style beats any selector, however specific");
    Expect(StyleOf("<p style='color: green'>x</p>", "p { color: red !important }", "p").color ==
               gfx::Color::Rgb(0xFF, 0, 0),
           "but not an important author rule");
  });

  AddTest(tests, "StyleResolver/BgcolorAttributeIsAPresentationalHint", [] {
    Expect(StyleOf("<table bgcolor='#123456'><tr><td>x</td></tr></table>", "", "table")
               .background_color == gfx::Color::Rgb(0x12, 0x34, 0x56),
           "old table markup uses bgcolor as a presentational background hint");
    Expect(StyleOf("<table bgcolor='red'><tr><td>x</td></tr></table>",
                   "table { background-color: blue }", "table")
               .background_color == gfx::Color::Rgb(0, 0, 0xFF),
           "author CSS still overrides the presentational hint");
    Expect(StyleOf("<table bgcolor='notacolour'><tr><td>x</td></tr></table>", "", "table")
               .background_color == gfx::Color::Transparent(),
           "an invalid bgcolor value is ignored like an invalid CSS colour");
  });

  AddTest(tests, "StyleResolver/InheritsTheInheritedPropertiesAndNotTheOthers", [] {
    const ComputedStyle child =
        StyleOf("<div style='color: red; margin: 20px'><span>x</span></div>", "", "span");
    Expect(child.color == gfx::Color::Rgb(0xFF, 0, 0), "colour is inherited");
    Expect(child.margin.top == Length::Pixels(0.0f),
           "margin is not; inheriting it would indent every nested element cumulatively");
  });

  AddTest(tests, "StyleResolver/FontSizeIsResolvedAgainstTheParentDuringTheCascade", [] {
    // `font-size: 2em` is relative to the *parent's* size, not its own, and
    // every other `em` on the element is relative to the resolved result.
    const ComputedStyle child = StyleOf(
        "<div style='font-size: 20px'><p style='font-size: 2em; margin: 1em'>x</p></div>", "",
        "p");
    Expect(child.font_size == 40.0f, "2em of a 20px parent is 40px");
    Expect(child.margin.top.unit == Length::Unit::Em && child.margin.top.value == 1.0f,
           "and the margin keeps its em unit, to be resolved against the element's own size");
    ExpectEqInt(static_cast<long long>(child.margin.top.Resolve(child.font_size)), 40,
                "which is 40px, not 20");
  });

  AddTest(tests, "StyleResolver/ParsesEdgeShorthands", [] {
    const ComputedStyle one = StyleOf("<p style='margin: 5px'>x</p>", "", "p");
    Expect(one.margin.top == Length::Pixels(5.0f) && one.margin.left == Length::Pixels(5.0f),
           "one value applies to all four sides");

    const ComputedStyle two = StyleOf("<p style='margin: 1px 2px'>x</p>", "", "p");
    Expect(two.margin.top == Length::Pixels(1.0f) && two.margin.right == Length::Pixels(2.0f),
           "two values are vertical then horizontal");
    Expect(two.margin.bottom == Length::Pixels(1.0f) && two.margin.left == Length::Pixels(2.0f),
           "and mirror");

    const ComputedStyle four = StyleOf("<p style='margin: 1px 2px 3px 4px'>x</p>", "", "p");
    Expect(four.margin.top == Length::Pixels(1.0f) && four.margin.right == Length::Pixels(2.0f) &&
               four.margin.bottom == Length::Pixels(3.0f) &&
               four.margin.left == Length::Pixels(4.0f),
           "four values run clockwise from the top");
  });

  AddTest(tests, "StyleResolver/ParsesTheBorderShorthandInAnyOrder", [] {
    const ComputedStyle style = StyleOf("<p style='border: 2px solid blue'>x</p>", "", "p");
    Expect(style.has_border, "a border was set");
    Expect(style.border_width.top == Length::Pixels(2.0f), "with its width");
    Expect(style.border_color == gfx::Color::Rgb(0, 0, 0xFF), "and its colour");

    const ComputedStyle reordered = StyleOf("<p style='border: red solid 3px'>x</p>", "", "p");
    Expect(reordered.border_width.top == Length::Pixels(3.0f) &&
               reordered.border_color == gfx::Color::Rgb(0xFF, 0, 0),
           "the components may come in any order, which is what the grammar says");
  });

  AddTest(tests, "StyleResolver/AnInvalidValueLeavesThePropertyAlone", [] {
    const ComputedStyle style =
        StyleOf("<p>x</p>", "p { color: notacolour; display: block }", "p");
    Expect(style.color == gfx::Color::Rgb(0, 0, 0),
           "an unparsable value is dropped rather than turning the element transparent");
    Expect(style.display == Display::Block, "and the rest of the rule still applies");
  });

  AddTest(tests, "StyleResolver/DisplayNoneMeansNoBox", [] {
    Expect(!StyleOf("<p style='display:none'>x</p>", "", "p").GeneratesBox(),
           "display:none generates no box");
    Expect(StyleOf("<head><title>t</title></head><p>x</p>", "", "title").display == Display::None,
           "and the built-in sheet hides the head, so its contents never render");
  });

  AddTest(tests, "StyleResolver/ResolvesEveryElementInOneTreeWalk", [] {
    const std::unique_ptr<dom::Document> document =
        html::ParseDocument("<div><p>a</p><p>b</p><span>c</span></div>");
    StyleResolver resolver;
    std::size_t count = 0;
    resolver.ForEachStyledElement(*document, [&count](const Element&, const ComputedStyle&) {
      ++count;
    });
    Expect(count >= 6, "html, head, body, div and three children are all styled");
  });

  AddTest(tests, "StyleResolver/HandlesSelectorsThatMatchNothingAndSheetsThatAreEmpty", [] {
    Expect(StyleOf("<p>x</p>", "", "p").display == Display::Block, "an empty author sheet");
    Expect(StyleOf("<p>x</p>", "nosuchelement { color: red }", "p").color ==
               gfx::Color::Rgb(0, 0, 0),
           "a selector matching nothing changes nothing");
    Expect(StyleOf("<p>x</p>", "}}} garbage {{{", "p").display == Display::Block,
           "and a sheet that is entirely broken leaves the built-in styles intact");
  });
}

}  // namespace microbrowser::tests

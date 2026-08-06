#include <memory>
#include <optional>
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
using css::Direction;
using css::Display;
using css::FontStyle;
using css::Length;
using css::Origin;
using css::ParseColor;
using css::ParseLength;
using css::ParseStyleSheet;
using css::InheritInto;
using css::Position;
using css::StyleResolver;
using css::UnicodeBidi;
using css::SupportsDeclaration;
using css::TextAlign;
using css::WhiteSpace;
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
    Expect(ParseColor("rgb(100%, 50%, 0%)") == gfx::Color::Rgb(255, 128, 0),
           "rgb percentage channels scale into bytes");
    Expect(ParseColor("rgb(1,\n2,\f3)") == gfx::Color::Rgb(1, 2, 3),
           "CSS whitespace separates rgb function components");
    Expect(ParseColor("rgba(1,2,3,0.5)") == gfx::Color::Rgba(1, 2, 3, 128),
           "alpha is 0..1 rather than 0..255");
    Expect(ParseColor("rgba(255,0,0,50%)") == gfx::Color::Rgba(255, 0, 0, 128),
           "alpha may also be a percentage");
    Expect(ParseColor("transparent") == gfx::Color::Transparent(), "transparent");
    Expect(!ParseColor("notacolour").has_value(), "an unknown name is not a colour");
    Expect(!ParseColor("#12345").has_value(), "and neither is a five-digit hex");
    Expect(!ParseColor("rgb(1, 2, 3, 4, 5)").has_value(),
           "extra rgb components invalidate the colour rather than being ignored");
  });

  AddTest(tests, "Css/ParsesLengths", [] {
    Expect(ParseLength("10px") == Length::Pixels(10.0f), "pixels");
    Expect(ParseLength("\n10px\f") == Length::Pixels(10.0f), "CSS whitespace is trimmed");
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

  AddTest(tests, "Css/EvaluatesCalc", [] {
    Expect(ParseLength("calc(10px + 5px)") == Length::Pixels(15.0f), "absolute terms fold");
    Expect(ParseLength("CALC(10px)") == Length::Pixels(10.0f), "the function name is not cased");
    Expect(ParseLength("calc(2 * 10px)") == Length::Pixels(20.0f), "a number may scale a length");
    Expect(ParseLength("calc(10px * 2)") == Length::Pixels(20.0f), "on either side");
    Expect(ParseLength("calc(30px / 4)") == Length::Pixels(7.5f), "and may divide one");
    Expect(ParseLength("calc(10px + 12pt)") == Length::Pixels(26.0f),
           "a point is folded into pixels before the sum, not after");
    Expect(ParseLength("calc(1rem + 4px)") == Length::Pixels(20.0f),
           "and so is a rem, at the same root size Length::Resolve uses");

    const std::optional<Length> mixed = ParseLength("calc(100% - 20px)");
    Expect(mixed.has_value() && mixed->IsPercent() && mixed->value == 100.0f &&
               mixed->offset == -20.0f,
           "the form the feature exists for: a percentage and an absolute term, both kept");
    Expect(mixed->Used(200.0f, 16.0f) == 180.0f,
           "and the offset is applied where the percentage is finally resolved");
    Expect(ParseLength("calc(1em + 4px)")->Resolve(10.0f) == 14.0f,
           "an em term resolves against the font size and keeps its offset");
    Expect(ParseLength("calc((100% - 20px) / 2)")->offset == -10.0f,
           "division distributes over both terms");
    Expect(ParseLength("calc(100% + calc(2px * 3))")->offset == 6.0f, "a calc may nest");

    Expect(!ParseLength("calc(100% - 1em)").has_value(),
           "two relative terms in one value is a calc this Length cannot hold, and an "
           "unrepresentable value is dropped rather than rounded to whichever term was noticed");
    Expect(!ParseLength("calc(10px / 0)").has_value(), "division by zero is invalid, not infinite");
    Expect(!ParseLength("calc(10px * 2px)").has_value(), "a length times a length is an area");
    Expect(!ParseLength("calc(10px + 2)").has_value(), "and a length plus a number is neither");
    Expect(!ParseLength("calc(100%-20px)").has_value(),
           "`+` and `-` need whitespace around them: without it `-20px` is a signed number, "
           "and accepting it here would parse a value no other engine does");
    Expect(!ParseLength("calc(1px +2px)").has_value(), "same rule on the trailing side");
    Expect(!ParseLength("calc(10vw)").has_value(), "a unit this engine has no answer for");
    Expect(!ParseLength("calc(10px").has_value(), "and an unclosed one is not a value at all");
    Expect(!ParseLength("calc()").has_value(), "nor an empty one");
    Expect(!ParseLength("calc(1e400px)").has_value(), "nor a number outside a double's range");
    Expect(!ParseLength("calc(1e30px * 1e30)").has_value(),
           "nor a product outside a float's, which would otherwise reach layout as infinity");

    // Every shorthand in this module counts its components and rejects the
    // wrong number, so a splitter that cut `calc(1px + 2px)` into three would
    // not produce a slightly wrong margin -- it would drop the declaration.
    const ComputedStyle shorthand =
        StyleOf("<p>x</p>", "p { margin: calc(4px + 6px) 0 }", "p");
    Expect(shorthand.margin.top == Length::Pixels(10.0f) &&
               shorthand.margin.left == Length::Pixels(0.0f),
           "a calc is one component of a shorthand, not one per space inside it");
    Expect(StyleOf("<p>x</p>", "p { background-size: calc(1rem + 4px) }", "p")
                   .background.size_x == Length::Pixels(20.0f),
           "and the same rule holds where the shorthand takes one or two lengths");
  });

  // The half of `@supports` that has to stay true as the engine changes. A
  // wrong answer here does not produce a missing effect, it produces a page
  // that styles itself for a browser this is not -- ADR 0014 §3.
  AddTest(tests, "StyleResolver/AnswersSupportsFromWhatItActuallyApplies", [] {
    Expect(SupportsDeclaration("display", "flex"), "flexbox is implemented, and says so");
    Expect(!SupportsDeclaration("display", "grid"),
           "grid is not, and a `display` branch that shrugged at an unknown value would "
           "have claimed it was");
    Expect(SupportsDeclaration("display", "inline-block"), "and inline-block is");
    Expect(!SupportsDeclaration("float", "inline-start"), "an unknown keyword on a known "
           "property is a no");
    Expect(!SupportsDeclaration("nosuchproperty", "1px"), "and so is an unknown property");
    Expect(SupportsDeclaration("width", "calc(100% - 20px)"),
           "a value's own grammar is part of the question");
    Expect(!SupportsDeclaration("width", "calc(100% - 1em)"),
           "including the calc forms this engine cannot represent");
    Expect(!SupportsDeclaration("width", "1vw"), "and the units it does not have");
    Expect(SupportsDeclaration("aspect-ratio", "16 / 9"), "aspect-ratio, in both spellings");
    Expect(SupportsDeclaration("aspect-ratio", "16/9"), "of the same ratio");
    Expect(!SupportsDeclaration("aspect-ratio", "auto 16/9"),
           "but not the form that asks a replaced element for its own ratio, which nothing "
           "here can answer");
    Expect(!SupportsDeclaration("aspect-ratio", "16 / 9 / 4"), "nor a third term");
    Expect(SupportsDeclaration("--anything", "whatever it likes"),
           "a custom property has no grammar to fail");
    Expect(!SupportsDeclaration("background", "linear-gradient(red, blue)"),
           "a gradient is not painted, so claiming the declaration works would send a page "
           "past the flat colour it offered as a fallback");
    Expect(SupportsDeclaration("BACKGROUND-COLOR", " red "),
           "the property name is folded and the value is trimmed, as in a stylesheet");
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
    Expect(StyleOf("<button>Go</button>", "", "button").display == Display::InlineBlock,
           "a button is an inline-block control, not ordinary inline text");
    Expect(StyleOf("<textarea>x</textarea>", "", "textarea").display == Display::InlineBlock,
           "a textarea is an inline-block control, not ordinary inline text");
    Expect(StyleOf("<select><option>x</option></select>", "", "select").display ==
               Display::InlineBlock,
           "a select is an inline-block control, not ordinary inline text");
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

  AddTest(tests, "StyleResolver/WidthAndHeightAttributesArePresentationalHints", [] {
    Expect(StyleOf("<table width='85%'><tr><td>x</td></tr></table>", "", "table").width ==
               (Length{85.0f, Length::Unit::Percent}),
           "a trailing percent is a percentage");
    Expect(StyleOf("<table width='400'><tr><td>x</td></tr></table>", "", "table").width ==
               Length::Pixels(400.0f),
           "and a bare number is pixels, not a unitless length CSS would reject");
    Expect(StyleOf("<table width='100*'><tr><td>x</td></tr></table>", "", "table").width ==
               Length::Auto(),
           "the legacy `100*` column syntax is not a length this browser has, so it is "
           "ignored rather than guessed at");
    Expect(StyleOf("<p width='400'>x</p>", "", "p").width == Length::Auto(),
           "and the attribute only applies to the elements HTML says it does");
  });

  AddTest(tests, "StyleResolver/AlignAttributeIsATextAlignHintOnCellsOnly", [] {
    Expect(StyleOf("<table><tr><td align='right'>x</td></tr></table>", "", "td").text_align ==
               TextAlign::Right,
           "`align` on a cell is text alignment");
    Expect(StyleOf("<table><tr><td align='right'>x</td></tr></table>",
                   "td { text-align: center }", "td")
                   .text_align == TextAlign::Center,
           "and a stylesheet still beats it, because it is a hint and not an author rule");
    // **This assertion used to read `== TextAlign::Left` and its comment used to say the value came
    // from the div, inherited.** Both were wrong, and the test passed anyway because `Left` was also
    // the initial value -- so it could not tell "inherited from the div" from "nobody set it". Now
    // that the initial value is `Start`, it can, and the answer is that nothing set it: `align` is
    // mapped on cells only, so neither the div's nor the img's produces a declaration.
    Expect(StyleOf("<div align='right'><img align='left'>x</div>", "", "img").text_align ==
               TextAlign::Start,
           "`align` on a div and on an image are both unmapped, so this is the initial value");
  });

  AddTest(tests, "StyleResolver/DirectionComesFromTheAttributeAndFromTheContent", [] {
    Expect(StyleOf("<div dir=rtl>x</div>", "", "div").direction == Direction::Rtl, "dir=rtl");
    Expect(StyleOf("<div dir=RTL>x</div>", "", "div").direction == Direction::Rtl,
           "and it is case-insensitive, like every HTML attribute value that is a keyword");
    Expect(StyleOf("<div dir=ltr>x</div>", "", "div").direction == Direction::Ltr, "dir=ltr");
    Expect(StyleOf("<div>x</div>", "", "div").direction == Direction::Ltr,
           "and nothing at all is left-to-right");
    // `dir=auto` is P2 over the element's own text, which is what makes a field holding
    // user-supplied text lay out the way its author wrote it.
    Expect(StyleOf("<div dir=auto>שלום</div>", "", "div").direction == Direction::Rtl,
           "auto with Hebrew content");
    Expect(StyleOf("<div dir=auto>hello שלום</div>", "", "div").direction == Direction::Ltr,
           "auto is decided by the *first* strong character, not by a majority");
    Expect(StyleOf("<div dir=auto>123 שלום</div>", "", "div").direction == Direction::Rtl,
           "and a number is not strong, so it does not decide");
    // The walk skips a descendant with its own `dir`, because that subtree is not this element's
    // text for this purpose -- otherwise a nested `<span dir=rtl>` would flip its parent.
    Expect(StyleOf("<div dir=auto><span dir=rtl>שלום</span> hello</div>", "", "div").direction ==
               Direction::Ltr,
           "a descendant with its own dir does not vote");
    // `<bdi>` defaults to auto, which is the whole point of the element.
    Expect(StyleOf("<div><bdi>שלום</bdi></div>", "", "bdi").direction == Direction::Rtl,
           "bdi infers its own direction with no attribute at all");
    Expect(StyleOf("<div><bdi>שלום</bdi></div>", "", "bdi").unicode_bidi == UnicodeBidi::Isolate,
           "and the user-agent sheet isolates it");
    Expect(StyleOf("<div><bdo dir=rtl>x</bdo></div>", "", "bdo").unicode_bidi ==
               UnicodeBidi::BidiOverride,
           "and overrides a bdo");
  });

  AddTest(tests, "StyleResolver/OneListOfInheritedProperties", [] {
    // `src/layout` used to keep a second list of which properties inherit, for the anonymous box it
    // builds around a text node -- and it had drifted: `direction` and `unicode-bidi` were added to
    // the cascade and not to it, so a right-to-left `<span>` was right-to-left and its own text was
    // not. There is one list now (`css::InheritInto`), and this asserts what it copies. **A property
    // added to it belongs here too**, which is the only way a third caller stays honest.
    ComputedStyle parent;
    parent.direction = Direction::Rtl;
    parent.unicode_bidi = UnicodeBidi::Isolate;
    parent.text_align = TextAlign::Center;
    parent.font_size = 21.0f;
    parent.SetCustomProperty("--x", "1");
    ComputedStyle child;
    InheritInto(parent, child);
    Expect(child.direction == Direction::Rtl, "direction inherits");
    Expect(child.unicode_bidi == UnicodeBidi::Isolate, "unicode-bidi inherits here, and not in CSS");
    Expect(child.text_align == TextAlign::Center, "text-align inherits");
    Expect(child.font_size == 21.0f, "font-size inherits");
    Expect(child.custom_properties.Size() == 1, "and so do custom properties");
    Expect(child.position == Position::Static,
           "while a non-inherited property stays at its initial value");
    ComputedStyle text_child;
    InheritInto(parent, text_child, /*with_custom_properties=*/false);
    Expect(text_child.direction == Direction::Rtl, "the text-box form inherits the rest");
    Expect(text_child.custom_properties.Empty(),
           "and skips the custom-property table, because a text box never resolves a var()");
  });

  AddTest(tests, "StyleResolver/CellPaddingIsReadFromTheTable", [] {
    Expect(StyleOf("<table cellpadding='6'><tr><td>x</td></tr></table>", "", "td")
                   .padding.left == Length::Pixels(6.0f),
           "`cellpadding` is written on the table and means padding on every cell in it");
    Expect(StyleOf("<table cellpadding='6'><tr><td>x</td></tr></table>", "", "table")
                   .padding.left == Length::Pixels(0.0f),
           "and not padding on the table itself");
  });

  AddTest(tests, "StyleResolver/CenterIsABlock", [] {
    // A <center> holding a table is the classic 1990s page layout. Left inline,
    // the table's rows end up on one line and the page is unreadable.
    Expect(StyleOf("<center>x</center>", "", "center").display == Display::Block,
           "<center> is a block");
    Expect(StyleOf("<center>x</center>", "", "center").text_align == TextAlign::Center,
           "that centers its inline content");
  });

  AddTest(tests, "StyleResolver/LinkMatchesEveryHyperlinkAndVisitedMatchesNone", [] {
    Expect(StyleOf("<a href='/x'>x</a>", "a:link { color: red }", "a").color ==
               gfx::Color::Rgb(0xFF, 0, 0),
           "`:link` matches a hyperlink");
    Expect(StyleOf("<a>x</a>", "a:link { color: red }", "a").color != gfx::Color::Rgb(0xFF, 0, 0),
           "and an anchor without an href is not one");
    // Not a missing feature: every way a page can read back which links are
    // styled differently -- painted colour, layout size, timing -- is a way to
    // read the user's history. Matching nothing is the only answer that leaks
    // nothing.
    Expect(StyleOf("<a href='/x'>x</a>", "a:visited { color: red }", "a").color !=
               gfx::Color::Rgb(0xFF, 0, 0),
           "`:visited` matches nothing, because a page that can see it can read history");
  });

  // Custom properties. The measurement in ADR 0014: youtube.com's stylesheet
  // uses `var()` 8585 times and grid 78, which is why these came first. An
  // unresolvable reference is *invalid at computed-value time* -- a defined
  // outcome, and not the same as an unrecognized declaration.
  AddTest(tests, "StyleResolver/CustomPropertiesSubstituteAndInherit", [] {
    Expect(StyleOf("<p>x</p>", ":root { --fg: #ff0000 } p { color: var(--fg) }", "p").color ==
               gfx::Color::Rgb(0xFF, 0, 0),
           "a var() reference takes the custom property's value");
    // Inherited, which is the whole reason a stylesheet can set them once on
    // :root and use them everywhere.
    Expect(StyleOf("<div><p>x</p></div>", "div { --fg: #00ff00 } p { color: var(--fg) }", "p")
                   .color == gfx::Color::Rgb(0, 0xFF, 0),
           "a custom property inherits to a descendant");
    // The nearer declaration wins, and it wins inside a value written further
    // up as well.
    Expect(StyleOf("<div><p>x</p></div>",
                   ":root { --fg: #ff0000 } p { --fg: #0000ff; color: var(--fg) }", "p")
                   .color == gfx::Color::Rgb(0, 0, 0xFF),
           "an element's own custom property beats the inherited one");
    // A reference to an unset name uses its fallback.
    Expect(StyleOf("<p>x</p>", "p { color: var(--nope, #ff0000) }", "p").color ==
               gfx::Color::Rgb(0xFF, 0, 0),
           "an unset name falls back");
    // References nest, both in the value and in the fallback.
    Expect(StyleOf("<p>x</p>", ":root { --a: var(--b); --b: #ff0000 } p { color: var(--a) }", "p")
                   .color == gfx::Color::Rgb(0xFF, 0, 0),
           "a custom property may reference another");
    Expect(StyleOf("<p>x</p>", ":root { --b: #ff0000 } p { color: var(--a, var(--b)) }", "p")
                   .color == gfx::Color::Rgb(0xFF, 0, 0),
           "a fallback may itself be a reference");
    // Substitution is textual and lands anywhere in a value, including part of
    // one -- which is what makes `--pad` usable in a shorthand.
    Expect(StyleOf("<p>x</p>", ":root { --pad: 20px } p { padding: var(--pad) 0 }", "p")
                   .padding.top == Length::Pixels(20.0f),
           "a reference may be one component of a shorthand");

    // Invalid at computed-value time: the property is unset, and the rule it
    // would have beaten does *not* get to win instead. This is the case that
    // separates a correct implementation from one that merely skips the
    // declaration -- see ADR 0014.
    Expect(StyleOf("<p>x</p>", "p { color: #ff0000 } p { color: var(--nope) }", "p").color !=
               gfx::Color::Rgb(0xFF, 0, 0),
           "an unresolvable reference unsets the property rather than yielding to a lower rule");
    // A cycle is not a hang.
    Expect(StyleOf("<p>x</p>", ":root { --a: var(--b); --b: var(--a) } p { color: var(--a) }", "p")
                   .color == gfx::Color::Rgb(0, 0, 0),
           "a cyclic reference is invalid rather than endless");
    // A declaration with no reference in it is untouched, including one that
    // merely contains the letters.
    Expect(StyleOf("<p>x</p>", "p { font-family: varsity }", "p").font_family[0] == "varsity",
           "a value that is not a var() reference is left alone");
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

  AddTest(tests, "StyleResolver/UnitlessLineHeightMayBeFractional", [] {
    const ComputedStyle style = StyleOf("<p style='font-size: 20px; line-height: 1.5'>x</p>", "",
                                        "p");
    ExpectEqInt(static_cast<long long>(style.line_height), 30,
                "a unitless line-height number multiplies the element font size");
    const ComputedStyle percent =
        StyleOf("<p style='font-size: 20px; line-height: 150%'>x</p>", "", "p");
    ExpectEqInt(static_cast<long long>(percent.line_height), 30,
                "a percentage line-height also resolves against the element font size");
    const ComputedStyle negative =
        StyleOf("<p style='line-height: 30px; line-height: -1.5'>x</p>", "", "p");
    ExpectEqInt(static_cast<long long>(negative.line_height), 30,
                "a negative line-height is invalid and leaves the earlier value alone");
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

    const ComputedStyle margin = StyleOf("<p style='margin: -1px auto'>x</p>", "", "p");
    Expect(margin.margin.top == Length::Pixels(-1.0f) && margin.margin.right.IsAuto(),
           "margins may be negative or auto");
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

    const ComputedStyle none = StyleOf("<p style='border: 2px solid blue; border: none'>x</p>", "",
                                       "p");
    Expect(!none.has_border, "border:none turns the border off");
  });

  AddTest(tests, "StyleResolver/AnInvalidValueLeavesThePropertyAlone", [] {
    const ComputedStyle style =
        StyleOf("<p>x</p>", "p { color: notacolour; display: block }", "p");
    Expect(style.color == gfx::Color::Rgb(0, 0, 0),
           "an unparsable value is dropped rather than turning the element transparent");
    Expect(style.display == Display::Block, "and the rest of the rule still applies");

    const ComputedStyle keywords =
        StyleOf("<p style='font-style: italic; font-style: sideways; "
                "text-align: center; text-align: somewhere; "
                "white-space: pre; white-space: balanced'>x</p>",
                "", "p");
    Expect(keywords.font_style == FontStyle::Italic,
           "an invalid font-style leaves the earlier valid value alone");
    Expect(keywords.text_align == TextAlign::Center,
           "an invalid text-align leaves the earlier valid value alone");
    Expect(keywords.white_space == WhiteSpace::Pre,
           "an invalid white-space leaves the earlier valid value alone");

    const ComputedStyle font =
        StyleOf("<p style='font-size: 20px; font-size: -5px; "
                "font-weight: 700; font-weight: 1001'>x</p>",
                "", "p");
    ExpectEqInt(static_cast<long long>(font.font_size), 20,
                "an invalid font-size leaves the earlier valid value alone");
    ExpectEqInt(static_cast<long long>(font.font_weight), 700,
                "an out-of-range font-weight leaves the earlier valid value alone");

    const ComputedStyle dimensions =
        StyleOf("<p style='width: 10px; width: -1px; height: 20px; height: -5%'>x</p>", "", "p");
    Expect(dimensions.width == Length::Pixels(10.0f),
           "an invalid width leaves the earlier valid value alone");
    Expect(dimensions.height == Length::Pixels(20.0f),
           "an invalid height leaves the earlier valid value alone");
    const ComputedStyle auto_width = StyleOf("<p style='width: 10px; width: auto'>x</p>", "", "p");
    Expect(auto_width.width.IsAuto(), "auto is still a valid width");

    const ComputedStyle border =
        StyleOf("<p style='border: 2px solid blue; border: wavy; "
                "border: -1px solid red; border-width: nope; border-width: 50%'>x</p>",
                "", "p");
    Expect(border.has_border, "an invalid border shorthand does not turn a previous border off");
    Expect(border.border_width.top == Length::Pixels(2.0f),
           "and an invalid border-width does not alter its previous width");
    Expect(border.border_color == gfx::Color::Rgb(0, 0, 0xFF),
           "or its previous colour");

    const ComputedStyle padding =
        StyleOf("<p style='padding: 5px; padding: -1px; padding-left: auto'>x</p>", "", "p");
    Expect(padding.padding.top == Length::Pixels(5.0f) &&
               padding.padding.left == Length::Pixels(5.0f),
           "invalid padding edge values leave the earlier valid padding alone");
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

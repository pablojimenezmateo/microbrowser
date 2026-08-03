#include <cmath>
#include <memory>
#include <variant>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "gfx/DisplayList.h"
#include "html/TreeBuilder.h"
#include "layout/LayoutEngine.h"

namespace microbrowser::tests {

using layout::Box;
using layout::FixedTextMeasurer;
using layout::LayoutEngine;

namespace {

// A measurer with exactly ten pixels per character at a 20px font, so an
// expected width is a number a test can state rather than a number that depends
// on which version of which typeface is installed.
constexpr float kAdvanceRatio = 0.5f;

struct LaidOut {
  std::unique_ptr<dom::Document> document;
  std::unique_ptr<css::StyleResolver> resolver;
  std::unique_ptr<FixedTextMeasurer> measurer;
  std::unique_ptr<Box> root;
  float height = 0.0f;
};

LaidOut Run(std::string_view html, std::string_view css_text, float width = 400.0f) {
  LaidOut result;
  result.document = html::ParseDocument(html);
  result.resolver = std::make_unique<css::StyleResolver>();
  result.resolver->AddStyleSheet(css::ParseStyleSheet(css_text), css::Origin::Author);
  result.measurer = std::make_unique<FixedTextMeasurer>(kAdvanceRatio);

  const LayoutEngine engine(*result.resolver, *result.measurer);
  result.root = engine.BuildBoxTree(*result.document);
  result.height = engine.Layout(*result.root, width);
  return result;
}

// The first box whose origin element has this tag.
const Box* FindBox(const Box& root, std::string_view tag) {
  const Box* found = nullptr;
  root.ForEachDescendant([&](const Box& box) {
    if (found == nullptr && box.Origin() != nullptr && box.Origin()->TagName() == tag) {
      found = &box;
    }
  });
  return found;
}

std::vector<const Box*> BoxesByTag(const Box& root, std::string_view tag) {
  std::vector<const Box*> boxes;
  root.ForEachDescendant([&](const Box& box) {
    if (box.Origin() != nullptr && box.Origin()->TagName() == tag) {
      boxes.push_back(&box);
    }
  });
  return boxes;
}

std::vector<const Box*> TextBoxes(const Box& root) {
  std::vector<const Box*> boxes;
  root.ForEachDescendant([&](const Box& box) {
    if (box.GetKind() == Box::Kind::Text) {
      boxes.push_back(&box);
    }
  });
  return boxes;
}

}  // namespace

void RegisterLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Layout/StacksBlocksVertically", [] {
    const LaidOut result =
        Run("<div>a</div><div>b</div>", "body { margin: 0 } div { margin: 0; font-size: 20px }");
    const Box* first = FindBox(*result.root, "div");
    Expect(first != nullptr, "the first div has a box");
    Expect(first->Geometry().content.y == 0.0f, "the first block starts at the top");
    Expect(result.height > 0.0f, "and the document has height");
  });

  AddTest(tests, "Layout/BlockWidthFillsTheContainingBlock", [] {
    const LaidOut result = Run("<div>x</div>", "body { margin: 0 } div { margin: 0 }", 400.0f);
    const Box* div = FindBox(*result.root, "div");
    Expect(div != nullptr, "the div exists");
    Expect(div->Geometry().content.width == 400.0f,
           "a block with auto width fills its containing block");
  });

  AddTest(tests, "Layout/SubtractsMarginPaddingAndBorderFromTheContentWidth", [] {
    const LaidOut result = Run(
        "<div>x</div>",
        "body { margin: 0 } div { margin: 10px; padding: 5px; border: 2px solid red }", 400.0f);
    const Box* div = FindBox(*result.root, "div");
    Expect(div != nullptr, "the div exists");
    // 400 - 2*10 margin - 2*5 padding - 2*2 border = 366
    Expect(div->Geometry().content.width == 366.0f,
           "content width is the containing block minus every horizontal edge");
    Expect(div->Geometry().content.x == 17.0f,
           "and the content starts inside all three: 10 margin + 2 border + 5 padding");
  });

  AddTest(tests, "Layout/NestedBlocksAccumulateTheirAncestorsLeftEdges", [] {
    // Geometry is absolute: the painter walks the box tree with no ancestor
    // stack. Vertical position was threaded through the layout cursor from the
    // start; horizontal was not, so every nested block painted at its parent's
    // left margin instead of past it. A page with a padded container inside a
    // padded body drew its content in the wrong place, and nothing noticed
    // because the widths were right.
    const LaidOut result =
        Run("<div><p>x</p></div>",
            "body { margin: 0; padding: 20px } div { margin: 0; padding: 10px } p { margin: 0 }",
            400.0f);

    const Box* div = FindBox(*result.root, "div");
    const Box* paragraph = FindBox(*result.root, "p");
    Expect(div != nullptr && paragraph != nullptr, "both boxes exist");
    Expect(div->Geometry().content.x == 30.0f,
           "20px of body padding plus 10px of the div's own");
    Expect(paragraph->Geometry().content.x == 30.0f, "and the paragraph starts inside both");
    Expect(paragraph->Geometry().content.width == 340.0f,
           "while the widths, which were always right, stay right");
  });

  AddTest(tests, "Layout/TextInsideNestedBlocksStartsAtTheContentEdge", [] {
    // The same bug seen from the side that a user would: the words, not the box.
    const LaidOut result =
        Run("<div>words</div>", "body { margin: 0; padding: 25px } div { margin: 0 }", 400.0f);
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty() && !texts.at(0)->Fragments().empty(), "the text has a fragment");
    Expect(texts.at(0)->Fragments().at(0).rect.x == 25.0f,
           "a line starts at its containing block's content edge, not at the viewport's");
  });

  AddTest(tests, "Layout/ResolvesPercentageWidthAgainstTheContainingBlock", [] {
    // The cascade carried the percentage rather than guessing at a containing
    // block; this is the one place it can be resolved.
    const LaidOut result =
        Run("<div>x</div>", "body { margin: 0 } div { margin: 0; width: 50% }", 400.0f);
    const Box* div = FindBox(*result.root, "div");
    Expect(div->Geometry().content.width == 200.0f, "half of 400");
  });

  AddTest(tests, "Layout/TheBoxModelEdgesNest", [] {
    const LaidOut result = Run(
        "<div>x</div>", "body { margin: 0 } div { margin: 10px; padding: 5px; border: 2px red }",
        400.0f);
    const Box* div = FindBox(*result.root, "div");
    const gfx::FloatRect content = div->Geometry().content;
    const gfx::FloatRect padding = div->Geometry().PaddingBox();
    const gfx::FloatRect border = div->Geometry().BorderBox();
    const gfx::FloatRect margin = div->Geometry().MarginBox();

    Expect(padding.x == content.x - 5.0f, "the padding box is outside the content box");
    Expect(border.x == padding.x - 2.0f, "the border box is outside the padding box");
    Expect(margin.x == border.x - 10.0f, "and the margin box outside that");
    Expect(margin.width > border.width && border.width > padding.width &&
               padding.width > content.width,
           "each box is strictly larger than the one it contains");
  });

  // Line breaking is the single most visible thing inline layout does.
  AddTest(tests, "Layout/BreaksLinesAtSpacesWhenTheyDoNotFit", [] {
    // At 20px and a 0.5 ratio each character is 10px wide, so a 100px box fits
    // ten characters.
    const LaidOut result = Run("<div>aaaa bbbb cccc</div>",
                               "body { margin: 0 } div { margin: 0; width: 100px; font-size: 20px }",
                               400.0f);
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty(), "there is text");
    // "aaaa bbbb" is 9 characters = 90px and fits; adding " cccc" would not.
    Expect(result.height >= 48.0f,
           "the text wrapped onto at least two lines, which at 24px per line is 48px");
  });

  AddTest(tests, "Layout/AWordTooLongForTheLineIsStillPlaced", [] {
    // Otherwise the line-breaking loop has nowhere to put it and spins.
    const LaidOut result =
        Run("<div>aaaaaaaaaaaaaaaaaaaa</div>",
            "body { margin: 0 } div { margin: 0; width: 20px; font-size: 20px }", 400.0f);
    Expect(result.height > 0.0f, "an unbreakable word still occupies a line");
    Expect(!TextBoxes(*result.root).empty(), "and produces a box");
  });

  AddTest(tests, "Layout/CollapsesWhitespaceButKeepsWordSeparation", [] {
    const LaidOut result = Run("<div>a    b</div>", "div { font-size: 20px }");
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty(), "there is text");
    ExpectEqString(texts.at(0)->Text(), "a b",
                   "runs of whitespace collapse to one space, which is what white-space: "
                   "normal means");
  });

  AddTest(tests, "Layout/WhitespaceBetweenBlocksGeneratesNoBox", [] {
    const LaidOut result = Run("<div>a</div>\n   \n<div>b</div>", "div { font-size: 20px }");
    ExpectEqInt(static_cast<long long>(TextBoxes(*result.root).size()), 2,
                "the whitespace between the two divs is not a text box; keeping it would put "
                "a blank line between every pair of paragraphs");
  });

  AddTest(tests, "Layout/PreservesWhitespaceWhenAskedTo", [] {
    const LaidOut result = Run("<pre>a    b</pre>", "");
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty(), "there is text");
    ExpectEqString(texts.at(0)->Text(), "a    b",
                   "white-space: pre keeps the run, which the built-in sheet sets on <pre>");
  });

  // Whitespace across an element boundary is the case that renders as
  // "boldand italic" when it is got wrong, and it is got wrong by collapsing
  // per text node instead of per inline formatting context.
  AddTest(tests, "Layout/KeepsTheSpaceBetweenTwoInlineElements", [] {
    const LaidOut result = Run("<p><b>bold</b> and <i>italic</i> after</p>", "");
    std::string line;
    for (const Box* text : TextBoxes(*result.root)) {
      line += text->Text();
    }
    ExpectEqString(line, "bold and italic after",
                   "the space between two inlines lives at the edge of the text node between "
                   "them, and dropping it renders 'boldand italic'");
  });

  AddTest(tests, "Layout/DropsACollapsibleSpaceAtTheStartOfALine", [] {
    // The other half: a leading space that survives box building must not
    // indent the line it lands on.
    const LaidOut result =
        Run("<div> leading</div>", "body { margin: 0 } div { margin: 0; font-size: 20px }", 400.0f);
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty(), "there is text");
    Expect(!texts.at(0)->Fragments().empty(), "and a fragment");
    Expect(texts.at(0)->Fragments().at(0).rect.x == 0.0f,
           "a line never begins with a collapsible space");
    ExpectEqInt(static_cast<long long>(texts.at(0)->Fragments().at(0).begin), 1,
                "the fragment starts after the space rather than the text losing it");
  });

  AddTest(tests, "Layout/AWrappedTextBoxGetsOneFragmentPerLine", [] {
    // A single geometry per text box meant the last line overwrote every
    // earlier one, and every line but the last painted in the wrong place.
    const LaidOut result = Run("<div>aaaa bbbb cccc</div>",
                               "body { margin: 0 } div { margin: 0; width: 100px; font-size: 20px }",
                               400.0f);
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty(), "there is text");
    const std::vector<layout::TextFragment>& fragments = texts.at(0)->Fragments();
    Expect(fragments.size() >= 2, "the run wrapped onto more than one line");
    Expect(fragments.at(1).rect.y > fragments.at(0).rect.y, "and the second line is below the first");
    for (const layout::TextFragment& fragment : fragments) {
      Expect(fragment.baseline > fragment.rect.y && fragment.baseline <= fragment.rect.Bottom(),
             "the baseline sits inside the line box, below its top");
    }
    std::size_t covered = 0;
    for (const layout::TextFragment& fragment : fragments) {
      covered += fragment.length;
    }
    Expect(covered <= texts.at(0)->Text().size(),
           "fragments index the box's own text and cannot run past it");
  });

  AddTest(tests, "Layout/RelayoutDoesNotAccumulateFragments", [] {
    LaidOut result = Run("<div>aaaa bbbb cccc</div>",
                         "body { margin: 0 } div { margin: 0; font-size: 20px }", 400.0f);
    const FixedTextMeasurer measurer(kAdvanceRatio);
    const LayoutEngine engine(*result.resolver, measurer);
    engine.Layout(*result.root, 100.0f);
    const std::size_t narrow = TextBoxes(*result.root).at(0)->Fragments().size();
    engine.Layout(*result.root, 400.0f);
    const std::size_t wide = TextBoxes(*result.root).at(0)->Fragments().size();
    Expect(narrow > wide, "a narrower viewport wraps onto more lines");
    ExpectEqInt(static_cast<long long>(wide), 1,
                "and laying out again replaces the fragments rather than appending to them");
  });

  // Mixed block and inline children is the case that needs anonymous boxes.
  AddTest(tests, "Layout/WrapsInlineSiblingsOfBlocksInAnonymousBoxes", [] {
    const LaidOut result = Run("<div>text<p>para</p>more</div>", "");
    std::size_t anonymous = 0;
    result.root->ForEachDescendant([&anonymous](const Box& box) {
      if (box.GetKind() == Box::Kind::AnonymousBlock) {
        ++anonymous;
      }
    });
    ExpectEqInt(static_cast<long long>(anonymous), 2,
                "the text before and after the paragraph each get an anonymous block, which "
                "is the only way block and inline content can be siblings");
  });

  AddTest(tests, "Layout/DisplayNoneRemovesTheSubtree", [] {
    const LaidOut result = Run("<div>visible<span style='display:none'>hidden</span></div>", "");
    bool saw_hidden = false;
    result.root->ForEachDescendant([&saw_hidden](const Box& box) {
      saw_hidden = saw_hidden || box.Text() == "hidden";
    });
    Expect(!saw_hidden, "display:none generates no box, and neither do its descendants");
  });

  AddTest(tests, "Layout/HeadContentIsNotLaidOut", [] {
    const LaidOut result = Run("<head><title>T</title></head><body>body</body>", "");
    bool saw_title = false;
    result.root->ForEachDescendant([&saw_title](const Box& box) {
      saw_title = saw_title || box.Text() == "T";
    });
    Expect(!saw_title, "the built-in sheet hides the head, so its text never becomes a box");
  });

  AddTest(tests, "Layout/ProducesABoxTreeForAnyDocument", [] {
    for (const std::string_view html : {"", "<p>", "<div><div><div>", "text", "<!--c-->",
                                        "<p>a</p><p>b</p>", "<span>i</span>"}) {
      const LaidOut result = Run(html, "");
      Expect(result.root != nullptr,
             std::string("no box tree for: ") + std::string(html));
      Expect(result.height >= 0.0f, "and a non-negative height");
    }
  });

  AddTest(tests, "Layout/EverythingOnALineSharesOneBaseline", [] {
    // Found by rendering a page: short text next to a tall image sat at the
    // top of the line instead of along its bottom. A line's items hang from a
    // shared baseline, and where that baseline falls is not known until every
    // item on the line has been measured -- which is why line layout is two
    // passes rather than placement as it goes.
    const LaidOut result = Run(
        "<body style='margin:0'><img src='x' width='60' height='60'>text</body>",
        "body { margin: 0 } img { margin: 0 }", 400.0f);

    const Box* image = FindBox(*result.root, "img");
    Expect(image != nullptr, "the image has a box");
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty() && !texts.at(0)->Fragments().empty(), "and the text has a fragment");

    const layout::TextFragment& fragment = texts.at(0)->Fragments().at(0);
    // A replaced element's baseline is its bottom edge, per CSS 2.1 s10.8.1.
    Expect(std::abs(image->Geometry().content.Bottom() - fragment.baseline) < 0.01f,
           "the image's bottom edge is the text's baseline, which is what makes an image on a "
           "line of text sit on the text rather than beside it");
    Expect(image->Geometry().content.height == 60.0f, "at its declared size");
  });

  AddTest(tests, "Layout/ATallLineItemMakesTheWholeLineTall", [] {
    const LaidOut short_line =
        Run("<body style='margin:0'>text</body>", "body { margin: 0 }", 400.0f);
    const LaidOut tall_line =
        Run("<body style='margin:0'><img src='x' width='10' height='90'>text</body>",
            "body { margin: 0 } img { margin: 0 }", 400.0f);
    Expect(tall_line.height >= short_line.height + 60.0f,
           "the line box grows to hold its tallest item rather than clipping it");
  });

  AddTest(tests, "Layout/AnImageTooWideForTheLineWrapsRatherThanOverlapping", [] {
    const LaidOut result =
        Run("<body style='margin:0'>text<img src='x' width='300' height='20'></body>",
            "body { margin: 0 } img { margin: 0 }", 200.0f);
    const Box* image = FindBox(*result.root, "img");
    Expect(image != nullptr, "the image has a box");
    Expect(image->Geometry().content.x == 0.0f,
           "an atomic inline that does not fit starts a new line rather than overlapping the "
           "text before it");
  });

  AddTest(tests, "Layout/InputControlsGenerateVisibleInlineBoxes", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input size='10'><span>after</span></body>",
            "body { margin: 0 } input { margin: 0; font-size: 20px }", 400.0f);
    const Box* input = FindBox(*result.root, "input");
    Expect(input != nullptr, "the input has a box");
    Expect(input->Geometry().content.width == 132.0f,
           "the size attribute becomes a bounded text-control width");
    Expect(input->Geometry().content.height == 30.0f,
           "and the control has a line-height-derived height");
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty() && !texts.at(0)->Fragments().empty(), "the following text laid out");
    Expect(texts.at(0)->Fragments().at(0).rect.x >= input->Geometry().content.Right(),
           "the input occupies inline space before following text");
  });

  AddTest(tests, "Layout/HiddenInputsGenerateNoBox", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input type='hidden' value='secret'><span>visible</span></body>",
            "body { margin: 0 }", 400.0f);
    Expect(FindBox(*result.root, "input") == nullptr,
           "a hidden input is form state, not a rendered control");
    Expect(!TextBoxes(*result.root).empty(), "the visible content remains");
  });

  AddTest(tests, "Layout/PaintsInputControlBackgroundAndBorder", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input size='4'></body>",
            "body { margin: 0 } input { margin: 0; background-color: white; "
            "border: 1px solid gray; font-size: 20px }",
            400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_fill = false;
    bool saw_stroke = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      saw_fill = saw_fill || std::holds_alternative<gfx::FillPathCommand>(command);
      saw_stroke = saw_stroke || std::holds_alternative<gfx::StrokePathCommand>(command);
    }
    Expect(saw_fill, "the control background is painted");
    Expect(saw_stroke, "and so is its border");
  });

  AddTest(tests, "Layout/PaintsInputControlValues", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input type='submit' value='Search'></body>",
            "body { margin: 0 } input { margin: 0; font-size: 20px }", 400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_value = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        saw_value = saw_value || (run != nullptr && run->text == "Search");
      }
    }
    Expect(saw_value, "a submit input paints its value text");
  });

  AddTest(tests, "Layout/PaintsButtonTextContent", [] {
    const LaidOut result =
        Run("<body style='margin:0'><button value='form-value'>Go</button></body>",
            "body { margin: 0 } button { margin: 0; width: 80px; height: 24px; font-size: 20px }",
            400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_label = false;
    bool leaked_value = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        saw_label = saw_label || (run != nullptr && run->text == "Go");
        leaked_value = leaked_value || (run != nullptr && run->text == "form-value");
      }
    }
    Expect(saw_label, "a button paints its text content");
    Expect(!leaked_value, "a button's value is form data, not the visible label");
  });

  AddTest(tests, "Layout/PaintsTextareaDefaultText", [] {
    const LaidOut result =
        Run("<body style='margin:0'><textarea cols='4' rows='2'>Hello</textarea></body>",
            "body { margin: 0 } textarea { margin: 0; font-size: 20px }", 400.0f);
    const Box* textarea = FindBox(*result.root, "textarea");
    Expect(textarea != nullptr, "the textarea has a box");
    Expect(textarea->Geometry().content.width == 60.0f, "cols controls intrinsic width");
    Expect(textarea->Geometry().content.height == 54.0f, "rows controls intrinsic height");

    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_text = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        saw_text = saw_text || (run != nullptr && run->text == "Hello");
      }
    }
    Expect(saw_text, "a textarea paints its default text");
  });

  AddTest(tests, "Layout/PaintsSelectedOptionText", [] {
    const LaidOut result =
        Run("<body style='margin:0'><select><option value='a'>Alpha</option>"
            "<option value='b' selected>Beta</option></select></body>",
            "body { margin: 0 } select { margin: 0; width: 80px; height: 24px; font-size: 20px }",
            400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_label = false;
    bool leaked_value = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        saw_label = saw_label || (run != nullptr && run->text == "Beta");
        leaked_value = leaked_value || (run != nullptr && run->text == "b");
      }
    }
    Expect(saw_label, "a select paints the selected option text");
    Expect(!leaked_value, "the option value is form data, not the visible label");
  });

  AddTest(tests, "Layout/PaintsCheckedInputIndicators", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input type='checkbox' checked value='box'>"
            "<input type='radio' checked value='radio'></body>",
            "body { margin: 0 } input { margin: 0; width: 20px; height: 20px; "
            "background-color: white; border: 1px solid gray; color: green }",
            400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_checkbox_mark = false;
    bool saw_radio_dot = false;
    bool leaked_value_text = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* stroke = std::get_if<gfx::StrokePathCommand>(&command)) {
        saw_checkbox_mark = saw_checkbox_mark || stroke->color == gfx::Color::Rgb(0, 0x80, 0);
      } else if (const auto* fill = std::get_if<gfx::FillPathCommand>(&command)) {
        saw_radio_dot = saw_radio_dot || fill->color == gfx::Color::Rgb(0, 0x80, 0);
      } else if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        leaked_value_text =
            leaked_value_text ||
            (run != nullptr && (run->text.find("box") != std::string::npos ||
                                run->text.find("radio") != std::string::npos));
      }
    }
    Expect(saw_checkbox_mark, "a checked checkbox paints an indicator");
    Expect(saw_radio_dot, "a checked radio paints an indicator");
    Expect(!leaked_value_text, "checkbox and radio values are form data, not visible labels");
  });

  AddTest(tests, "Layout/PasswordInputValuesAreNotPaintedAsText", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input type='password' value='secret'></body>",
            "body { margin: 0 } input { margin: 0; font-size: 20px }", 400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        Expect(run == nullptr || run->text.find("secret") == std::string::npos,
               "the password value must not be emitted as text");
      }
    }
  });

  AddTest(tests, "Layout/PaintsTextInputPlaceholders", [] {
    const LaidOut result =
        Run("<body style='margin:0'><input placeholder='Search'></body>",
            "body { margin: 0 } input { margin: 0; width: 100px; height: 24px; font-size: 20px }",
            400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_placeholder = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        saw_placeholder = saw_placeholder || (run != nullptr && run->text == "Search");
      }
    }
    Expect(saw_placeholder, "an empty text input paints its placeholder");
  });

  AddTest(tests, "Layout/PaintsPasswordPlaceholdersWithoutLeakingValues", [] {
    const LaidOut result = Run(
        "<body style='margin:0'><input type='password' placeholder='Password'>"
        "<input type='password' value='secret' placeholder='Secret'></body>",
        "body { margin: 0 } input { margin: 0; width: 100px; height: 24px; font-size: 20px }",
        400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    bool saw_placeholder = false;
    bool leaked_value = false;
    bool painted_hidden_placeholder = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        saw_placeholder = saw_placeholder || (run != nullptr && run->text == "Password");
        leaked_value =
            leaked_value || (run != nullptr && run->text.find("secret") != std::string::npos);
        painted_hidden_placeholder =
            painted_hidden_placeholder || (run != nullptr && run->text == "Secret");
      }
    }
    Expect(saw_placeholder, "an empty password input paints its placeholder");
    Expect(!leaked_value, "a password input value is still not emitted as text");
    Expect(!painted_hidden_placeholder, "a non-empty password input does not paint its placeholder");
  });

  AddTest(tests, "Layout/TableCellsShareARowInsteadOfStacking", [] {
    // One character is 10px wide under the fixed measurer at this font size, so
    // each column wants exactly 10 and the table shrinks to 20 of the 200
    // available.
    const LaidOut result =
        Run("<table><tr><td>a</td><td>b</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            200.0f);
    const std::vector<const Box*> cells = BoxesByTag(*result.root, "td");
    ExpectEqInt(static_cast<long long>(cells.size()), 2, "two cell boxes");
    Expect(cells.at(0)->Geometry().content.x == 0.0f, "the first cell starts at the row edge");
    Expect(cells.at(1)->Geometry().content.x == 10.0f,
           "the second cell is placed beside the first, not below it");
    Expect(cells.at(0)->Geometry().content.y == cells.at(1)->Geometry().content.y,
           "cells in one row share a y position");
  });

  AddTest(tests, "Layout/TableColumnsAreSizedToTheirContent", [] {
    // The bug this guards: every column got the same share of the table, so a
    // rank column beside an article title took a third of the page. Hacker
    // News is one table and unreadable without this.
    const LaidOut result =
        Run("<table><tr><td>1.</td><td>a much longer headline</td></tr>"
            "<tr><td>2.</td><td>short</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            600.0f);
    const std::vector<const Box*> cells = BoxesByTag(*result.root, "td");
    ExpectEqInt(static_cast<long long>(cells.size()), 4, "four cell boxes");
    Expect(cells.at(0)->Geometry().content.width == 20.0f,
           "the narrow column is as wide as its widest cell, not a share of the table");
    Expect(cells.at(1)->Geometry().content.width == 220.0f,
           "and the wide column gets what it asks for");
    Expect(cells.at(2)->Geometry().content.width == cells.at(0)->Geometry().content.width &&
               cells.at(3)->Geometry().content.x == cells.at(1)->Geometry().content.x,
           "every row uses the same column grid");
  });

  AddTest(tests, "Layout/ATableIsNoWiderThanItsContent", [] {
    const LaidOut result =
        Run("<table><tr><td>ab</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }", 400.0f);
    const Box* table = FindBox(*result.root, "table");
    Expect(table != nullptr && table->Geometry().content.width == 20.0f,
           "a table with no stated width shrinks to fit rather than filling its container the "
           "way an ordinary block does");
  });

  AddTest(tests, "Layout/ATableTooNarrowForItsContentUsesMinimumColumns", [] {
    // 40px of content in a 30px table. Squeezing below the minimum does not
    // make the text fit, so the columns stay at their minimum and the table
    // overflows -- which is what every browser does.
    const LaidOut result =
        Run("<table width='30'><tr><td>abcd</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }", 400.0f);
    const std::vector<const Box*> cells = BoxesByTag(*result.root, "td");
    ExpectEqInt(static_cast<long long>(cells.size()), 1, "one cell");
    Expect(cells.at(0)->Geometry().content.width == 40.0f,
           "the column keeps its minimum width and the table overflows");
  });

  AddTest(tests, "Layout/SlackIsSharedByHowMuchEachColumnCouldUse", [] {
    // "a b" has a min of 10 (its longest word) and a max of 30; "xy" cannot
    // wrap, so both its bounds are 20. The table's minimum is 30 and its
    // maximum 50, so a stated 45 leaves 15 of slack -- all of which belongs to
    // the only column with room left to use it.
    const LaidOut result =
        Run("<table width='45'><tr><td>a b</td><td>xy</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }", 400.0f);
    const std::vector<const Box*> cells = BoxesByTag(*result.root, "td");
    ExpectEqInt(static_cast<long long>(cells.size()), 2, "two cells");
    Expect(cells.at(0)->Geometry().content.width == 25.0f,
           "the wrappable column takes all of the slack");
    Expect(cells.at(1)->Geometry().content.width == 20.0f,
           "and the column that cannot use it does not grow");
  });

  AddTest(tests, "Layout/TableRowsAdvanceByTheirTallestCell", [] {
    const LaidOut result = Run(
        "<table><tr><td>short</td><td><img src='x' width='10' height='60'></td></tr>"
        "<tr><td>next</td><td>row</td></tr></table>",
        "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px } img { margin: 0 }",
        200.0f);
    const std::vector<const Box*> rows = BoxesByTag(*result.root, "tr");
    ExpectEqInt(static_cast<long long>(rows.size()), 2, "two row boxes");
    Expect(rows.at(0)->Geometry().content.height >= 60.0f,
           "the first row is at least as tall as its image cell");
    Expect(rows.at(1)->Geometry().content.y >= rows.at(0)->Geometry().content.Bottom(),
           "the next row starts below the tallest cell, not below the first cell only");
  });

  AddTest(tests, "Layout/TableColspanConsumesMultipleColumns", [] {
    const LaidOut result =
        Run("<table><tr><td colspan='2'>wide</td><td>right</td></tr>"
            "<tr><td>a</td><td>b</td><td>c</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            300.0f);
    const std::vector<const Box*> cells = BoxesByTag(*result.root, "td");
    ExpectEqInt(static_cast<long long>(cells.size()), 5, "five cell boxes");
    // "wide" needs 40 across the two columns "a" and "b" would size to 10
    // each, so the shortfall is shared evenly and both become 20.
    Expect(cells.at(0)->Geometry().content.width == 40.0f,
           "a colspan=2 cell occupies two of the three columns");
    Expect(cells.at(1)->Geometry().content.x == 40.0f,
           "the following cell starts after both columns the span consumed");
    Expect(cells.at(3)->Geometry().content.x == 20.0f &&
               cells.at(4)->Geometry().content.x == 40.0f,
           "the next row still uses the same three-column grid");
  });

  AddTest(tests, "Layout/InvalidTableColspanFallsBackToOneColumn", [] {
    const LaidOut result =
        Run("<table><tr><td colspan='0'>a</td><td colspan='n'>b</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            200.0f);
    const std::vector<const Box*> cells = BoxesByTag(*result.root, "td");
    ExpectEqInt(static_cast<long long>(cells.size()), 2, "two cell boxes");
    Expect(cells.at(0)->Geometry().content.width == 10.0f,
           "zero is not accepted as a table span");
    Expect(cells.at(1)->Geometry().content.x == 10.0f,
           "and a non-numeric span does not consume extra columns");
  });

  AddTest(tests, "Layout/TableWidthAttributeCanBeAPercentage", [] {
    const LaidOut result =
        Run("<table width='50%'><tr><td>a</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            400.0f);
    const Box* table = FindBox(*result.root, "table");
    Expect(table != nullptr, "the table has a box");
    Expect(table->Geometry().content.width == 200.0f,
           "the table width attribute resolves as a percentage of the containing block");
  });

  AddTest(tests, "Layout/TableWidthAttributeCanBeUnitlessPixels", [] {
    const LaidOut result =
        Run("<table width='120'><tr><td>a</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            400.0f);
    const Box* table = FindBox(*result.root, "table");
    Expect(table != nullptr, "the table has a box");
    Expect(table->Geometry().content.width == 120.0f,
           "HTML's unitless table width attribute is pixels, unlike CSS width");
  });

  AddTest(tests, "Layout/InvalidTableWidthAttributeIsIgnored", [] {
    const LaidOut result =
        Run("<table width='wide'><tr><td>a</td></tr></table>",
            "body { margin: 0 } table, td { margin: 0; padding: 0; font-size: 20px }",
            400.0f);
    const Box* table = FindBox(*result.root, "table");
    Expect(table != nullptr, "the table has a box");
    Expect(table->Geometry().content.width == 10.0f,
           "an invalid table width attribute falls back to auto width, which for a table is "
           "shrink-to-fit rather than the full containing block");
  });

  // --- Painting -------------------------------------------------------------

  AddTest(tests, "Layout/PaintsBackgroundsAndBorders", [] {
    const LaidOut result = Run(
        "<div>x</div>",
        "body { margin: 0 } div { margin: 0; background-color: red; border: 2px solid blue }",
        400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    Expect(!list.IsEmpty(), "something was painted");

    bool saw_fill = false;
    bool saw_stroke = false;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      saw_fill = saw_fill || std::holds_alternative<gfx::FillPathCommand>(command);
      saw_stroke = saw_stroke || std::holds_alternative<gfx::StrokePathCommand>(command);
    }
    Expect(saw_fill, "the background is a fill");
    Expect(saw_stroke, "and the border is a stroke");
  });

  AddTest(tests, "Layout/PaintsTextAtItsBaselineWithTheStylesFont", [] {
    const LaidOut result =
        Run("<div>hi</div>",
            "body { margin: 0 } div { margin: 0; font-size: 20px; font-weight: bold; "
            "font-family: Fictional; color: red }",
            400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);

    const gfx::DrawTextCommand* drawn = nullptr;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        drawn = text;
      }
    }
    Expect(drawn != nullptr, "the text was recorded");
    Expect(list.TextAt(drawn->text) != nullptr, "with a run");
    ExpectEqString(list.TextAt(drawn->text)->text, "hi", "and the run is the text");
    Expect(drawn->color == gfx::Color::Rgb(0xFF, 0, 0), "in the style's color");

    const gfx::FontRequest* font = list.FontAt(drawn->font);
    Expect(font != nullptr, "and a font request");
    ExpectEqInt(static_cast<long long>(font->families.size()), 1, "naming one family");
    ExpectEqString(font->families.at(0), "Fictional",
                   "which names families rather than carrying a font handle -- a handle could "
                   "not cross a process boundary");
    Expect(font->size == 20.0f && font->weight == 700, "at the style's size and weight");

    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(drawn->origin.y == texts.at(0)->Fragments().at(0).baseline,
           "text is drawn at the baseline, not at the top of the line box");
  });

  AddTest(tests, "Layout/TextBoundsCoverTheGlyphsWithoutAFont", [] {
    // Damage is computed from a display list alone: the compositor side has no
    // font stack, and after the process split it is on the other side of the
    // sandbox from one.
    const LaidOut result =
        Run("<div>hi</div>", "body { margin: 0 } div { margin: 0; font-size: 20px }", 400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    const gfx::IntRect bounds = list.Bounds();

    const layout::TextFragment& fragment = TextBoxes(*result.root).at(0)->Fragments().at(0);
    Expect(bounds.y <= static_cast<int>(fragment.baseline) - 20,
           "the bound reaches an em above the baseline, which is above any ascent");
    Expect(bounds.Right() >= static_cast<int>(fragment.rect.Right()),
           "and past the end of the advance, where a diacritic can sit");
  });

  AddTest(tests, "Layout/PaintsParentsBeforeChildren", [] {
    // A child must draw on top of its parent's background rather than under it.
    const LaidOut result =
        Run("<div style='background-color:red'><p style='background-color:blue'>x</p></div>",
            "body { margin: 0 }", 400.0f);
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);

    std::vector<gfx::Color> fills;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* fill = std::get_if<gfx::FillPathCommand>(&command)) {
        fills.push_back(fill->color);
      }
    }
    Expect(fills.size() >= 2, "both backgrounds painted");
    Expect(fills.at(0) == gfx::Color::Rgb(0xFF, 0, 0), "the parent's is first");
    Expect(fills.at(1) == gfx::Color::Rgb(0, 0, 0xFF), "and the child's is on top of it");
  });

  AddTest(tests, "Layout/ATextBoxDoesNotInheritItsParentsBackgroundOrBorder", [] {
    // Found by rendering a page: the paragraph had two nested borders. A text
    // box was built with a copy of its parent's whole computed style, so it
    // carried the parent's background and border and the painter drew both a
    // second time, inset by however wide the line was.
    const LaidOut result = Run(
        "<p>x</p>", "body { margin: 0 } p { background-color: red; border: 2px solid blue }",
        400.0f);
    const std::vector<const Box*> texts = TextBoxes(*result.root);
    Expect(!texts.empty(), "there is text");
    Expect(texts.at(0)->Style().background_color.Alpha() == 0,
           "background-color does not inherit");
    Expect(texts.at(0)->Style().border_width.left.Resolve(0.0f) == 0.0f, "and neither does a border");

    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    std::size_t strokes = 0;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      strokes += std::holds_alternative<gfx::StrokePathCommand>(command) ? 1u : 0u;
    }
    ExpectEqInt(static_cast<long long>(strokes), 1, "exactly one border is painted, not two");
  });

  AddTest(tests, "Layout/PaintsNothingForAnEmptyDocument", [] {
    const LaidOut result = Run("", "");
    gfx::DisplayList list;
    layout::BuildDisplayList(*result.root, list);
    Expect(list.IsEmpty(),
           "a document with no backgrounds or borders paints nothing, which is different "
           "from failing to paint");
  });
}

}  // namespace microbrowser::tests

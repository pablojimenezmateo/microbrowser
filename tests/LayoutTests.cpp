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

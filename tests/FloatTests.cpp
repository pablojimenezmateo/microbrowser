#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "html/TreeBuilder.h"
#include "layout/FloatContext.h"
#include "layout/LayoutEngine.h"

namespace microbrowser::tests {

using css::Clear;
using css::Float;
using layout::Box;
using layout::FloatContext;
using layout::FixedTextMeasurer;
using layout::LayoutEngine;

namespace {

constexpr float kAdvanceRatio = 0.5f;

struct LaidOut {
  std::unique_ptr<dom::Document> document;
  std::unique_ptr<css::StyleResolver> resolver;
  std::unique_ptr<FixedTextMeasurer> measurer;
  std::unique_ptr<Box> root;
  float height = 0.0f;
};

LaidOut Run(std::string_view html, std::string_view css_text, float width = 300.0f) {
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

const Box* FindBox(const Box& root, std::string_view tag, std::size_t skip = 0) {
  const Box* found = nullptr;
  std::size_t seen = 0;
  root.ForEachDescendant([&](const Box& box) {
    if (found == nullptr && box.Origin() != nullptr && box.Origin()->TagName() == tag) {
      if (seen++ == skip) {
        found = &box;
      }
    }
  });
  return found;
}

std::vector<layout::TextFragment> AllFragments(const Box& root) {
  std::vector<layout::TextFragment> fragments;
  root.ForEachDescendant([&](const Box& box) {
    for (const layout::TextFragment& fragment : box.Fragments()) {
      fragments.push_back(fragment);
    }
  });
  return fragments;
}

}  // namespace

void RegisterFloatTests(std::vector<TestCase>& tests) {
  // --- The placement algorithm, without a document -------------------------

  AddTest(tests, "FloatContext/AnEmptyContextNarrowsNothing", [] {
    const FloatContext floats;
    const FloatContext::Band band = floats.BandAt(0.0f, 20.0f, 10.0f, 110.0f);
    Expect(band.left == 10.0f && band.right == 110.0f, "the full containing block is available");
    Expect(floats.LowestBottom() == 0.0f, "and nothing hangs below anything");
  });

  AddTest(tests, "FloatContext/ALeftFloatNarrowsFromTheLeft", [] {
    FloatContext floats;
    const gfx::FloatRect placed = floats.Place(Float::Left, 40.0f, 30.0f, 0.0f, 0.0f, 100.0f);
    Expect(placed == gfx::FloatRect{0.0f, 0.0f, 40.0f, 30.0f}, "it sits at the left edge");

    const FloatContext::Band beside = floats.BandAt(0.0f, 10.0f, 0.0f, 100.0f);
    Expect(beside.left == 40.0f && beside.right == 100.0f, "a line beside it starts past it");

    const FloatContext::Band below = floats.BandAt(30.0f, 10.0f, 0.0f, 100.0f);
    Expect(below.left == 0.0f,
           "and a line below it gets its width back -- a float that narrowed every later line "
           "would be a margin, not a float");
  });

  AddTest(tests, "FloatContext/ARightFloatNarrowsFromTheRight", [] {
    FloatContext floats;
    const gfx::FloatRect placed = floats.Place(Float::Right, 40.0f, 30.0f, 0.0f, 0.0f, 100.0f);
    Expect(placed.x == 60.0f, "it sits against the right edge");
    const FloatContext::Band band = floats.BandAt(0.0f, 10.0f, 0.0f, 100.0f);
    Expect(band.left == 0.0f && band.right == 60.0f, "and the band ends where it begins");
  });

  AddTest(tests, "FloatContext/TwoFloatsOnOneSideStackSideways", [] {
    FloatContext floats;
    floats.Place(Float::Left, 30.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    const gfx::FloatRect second = floats.Place(Float::Left, 30.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    Expect(second.x == 30.0f && second.y == 0.0f,
           "the second goes beside the first while there is room");
  });

  AddTest(tests, "FloatContext/AFloatThatDoesNotFitDropsBelow", [] {
    FloatContext floats;
    floats.Place(Float::Left, 60.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    const gfx::FloatRect second = floats.Place(Float::Left, 60.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    Expect(second.y == 20.0f, "past the bottom of the one it could not fit beside");
    Expect(second.x == 0.0f, "and back at the edge, where there is room again");
  });

  AddTest(tests, "FloatContext/AFloatIsNeverHigherThanAnEarlierOne", [] {
    // CSS 2.1 s9.5.1: a float's top may not be above the top of any float
    // placed before it. Without the rule, a short float after a tall one would
    // float up past it and the two would swap order on the page.
    FloatContext floats;
    floats.Place(Float::Left, 20.0f, 20.0f, 50.0f, 0.0f, 100.0f);
    const gfx::FloatRect second = floats.Place(Float::Left, 20.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    Expect(second.y >= 50.0f, "the later float cannot rise above the earlier one");
  });

  AddTest(tests, "FloatContext/AFloatWiderThanTheContainerStillLands", [] {
    FloatContext floats;
    const gfx::FloatRect placed = floats.Place(Float::Left, 500.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    Expect(placed.width <= 100.0f, "clamped to the containing block");
    Expect(floats.Count() == 1, "and placed rather than dropped: it overflows, it does not vanish");
  });

  AddTest(tests, "FloatContext/ClearSkipsToTheBottomOfTheNamedSide", [] {
    FloatContext floats;
    floats.Place(Float::Left, 20.0f, 40.0f, 0.0f, 0.0f, 100.0f);
    floats.Place(Float::Right, 20.0f, 80.0f, 0.0f, 0.0f, 100.0f);

    ExpectEqInt(static_cast<long long>(floats.ClearanceBelow(Clear::Left, 0.0f)), 40,
                "clear:left skips the left float only");
    ExpectEqInt(static_cast<long long>(floats.ClearanceBelow(Clear::Right, 0.0f)), 80,
                "clear:right the right one");
    ExpectEqInt(static_cast<long long>(floats.ClearanceBelow(Clear::Both, 0.0f)), 80,
                "and clear:both the lower of them -- one decision, not two");
    ExpectEqInt(static_cast<long long>(floats.ClearanceBelow(Clear::None, 5.0f)), 5,
                "clear:none moves nothing");
    ExpectEqInt(static_cast<long long>(floats.ClearanceBelow(Clear::Both, 200.0f)), 200,
                "and clearing never moves a box back up");
  });

  AddTest(tests, "FloatContext/AFloatEndingWhereALineBeginsDoesNotNarrowIt", [] {
    // Half-open vertical overlap. Closed at both ends, a float and the line
    // below it fight over one row of pixels.
    FloatContext floats;
    floats.Place(Float::Left, 40.0f, 20.0f, 0.0f, 0.0f, 100.0f);
    const FloatContext::Band band = floats.BandAt(20.0f, 10.0f, 0.0f, 100.0f);
    Expect(band.left == 0.0f, "the line starting exactly at the float's bottom is full width");
  });

  AddTest(tests, "FloatContext/ATallLineIsNarrowedByAFloatItOnlyPartlyOverlaps", [] {
    // A line box is not a line: asking about its top alone would let a tall
    // line clear a float at its top and still overlap it lower down.
    FloatContext floats;
    floats.Place(Float::Left, 40.0f, 20.0f, 10.0f, 0.0f, 100.0f);
    const FloatContext::Band band = floats.BandAt(0.0f, 40.0f, 0.0f, 100.0f);
    Expect(band.left == 40.0f, "the float narrows a line that spans across it");
  });

  // --- Floats in a document -------------------------------------------------

  AddTest(tests, "Layout/TextFlowsBesideAFloat", [] {
    const LaidOut result = Run(
        "<div>F</div><p>aaaa bbbb cccc dddd</p>",
        "body { margin: 0 } div { float: left; width: 100px; height: 50px; margin: 0 } "
        "p { margin: 0; font-size: 20px }",
        300.0f);

    const Box* floated = FindBox(*result.root, "div");
    Expect(floated != nullptr, "the float has a box");
    Expect(floated->Geometry().content.x == 0.0f && floated->Geometry().content.y == 0.0f,
           "and sits at the top left");

    const std::vector<layout::TextFragment> fragments = AllFragments(*result.root);
    bool saw_beside = false;
    for (const layout::TextFragment& fragment : fragments) {
      if (fragment.rect.y < 50.0f && fragment.rect.width > 0.0f && fragment.rect.x >= 100.0f) {
        saw_beside = true;
      }
    }
    Expect(saw_beside, "the paragraph's lines start past the float rather than under it");
  });

  AddTest(tests, "Layout/TheBlockBoxIsNotNarrowedByAFloatButItsLinesAre", [] {
    // The distinction that surprises everyone, and it is the correct one: a
    // float overlaps the block box and shortens the line boxes inside it, which
    // is why a background on the paragraph runs behind the float.
    const LaidOut result =
        Run("<div>F</div><p>text</p>",
            "body { margin: 0 } div { float: left; width: 80px; height: 40px; margin: 0 } "
            "p { margin: 0 }",
            300.0f);
    const Box* paragraph = FindBox(*result.root, "p");
    Expect(paragraph != nullptr, "the paragraph has a box");
    Expect(paragraph->Geometry().content.x == 0.0f,
           "the block box still starts at the containing block's edge");
    Expect(paragraph->Geometry().content.width == 300.0f, "and keeps its full width");
    Expect(!paragraph->Fragments().empty() || !AllFragments(*result.root).empty(),
           "while the text inside it moved");
  });

  AddTest(tests, "Layout/ClearMovesABlockBelowTheFloat", [] {
    const LaidOut result = Run(
        "<div id=f>F</div><div id=c>after</div>",
        "body { margin: 0 } #f { float: left; width: 100px; height: 50px; margin: 0 } "
        "#c { clear: both; margin: 0 }",
        300.0f);
    const Box* cleared = FindBox(*result.root, "div", 1);
    Expect(cleared != nullptr, "the cleared block has a box");
    Expect(cleared->Geometry().content.y >= 50.0f,
           "it starts below the float rather than beside it");
  });

  AddTest(tests, "Layout/AFloatWithNoWidthShrinksToFitItsContent", [] {
    // Otherwise a float fills its containing block and there is nothing left to
    // flow beside it, which is the one thing a float is for.
    const LaidOut result =
        Run("<div>abcd</div><p>text</p>",
            "body { margin: 0 } div { float: left; margin: 0; font-size: 20px } p { margin: 0 }",
            300.0f);
    const Box* floated = FindBox(*result.root, "div");
    Expect(floated != nullptr, "the float has a box");
    Expect(floated->Geometry().content.width > 0.0f, "it is not empty");
    Expect(floated->Geometry().content.width < 300.0f,
           "and not the whole containing block: four characters at ten pixels each is forty");
  });

  AddTest(tests, "Layout/TheDocumentIsTallEnoughToHoldATallFloat", [] {
    const LaidOut result =
        Run("<div>F</div>",
            "body { margin: 0 } div { float: left; width: 50px; height: 400px; margin: 0 }",
            300.0f);
    Expect(result.height >= 400.0f,
           "a page that is nothing but a tall float still scrolls to the bottom of it");
  });

  AddTest(tests, "Layout/AFloatInsideAFloatDoesNotEscapeIt", [] {
    // A float establishes a formatting context. Without that, a float in a
    // sidebar would shorten the lines of the article beside it.
    const LaidOut result = Run(
        "<div id=outer><div id=inner>x</div></div><p>aaaa bbbb</p>",
        "body { margin: 0 } #outer { float: left; width: 100px; height: 30px; margin: 0 } "
        "#inner { float: left; width: 40px; height: 200px; margin: 0 } p { margin: 0 }",
        300.0f);

    const std::vector<layout::TextFragment> fragments = AllFragments(*result.root);
    bool paragraph_clears_outer_only = false;
    for (const layout::TextFragment& fragment : fragments) {
      // The paragraph runs beside the 100px outer float, not beside the inner
      // one, so it starts at 100 rather than being pushed further.
      if (fragment.rect.x == 100.0f) {
        paragraph_clears_outer_only = true;
      }
    }
    Expect(paragraph_clears_outer_only,
           "the inner float belongs to the outer float's formatting context and does not "
           "narrow lines outside it");
  });

  AddTest(tests, "Layout/AFloatedImageIsAFloatAndNotAnInlineAtomic", [] {
    // Found by rendering a page: a floated <img> stacked above the text
    // instead of beside it. A float is out of flow whatever kind of box it is,
    // and a replaced box is the one kind that is otherwise placed on a line --
    // so every classification that asked "is this block-level?" had to start
    // asking "is this in the line flow?" instead.
    const LaidOut result = Run(
        "<img src='x' width='100' height='50'><p>aaaa bbbb cccc</p>",
        "body { margin: 0 } img { float: left; margin: 0 } p { margin: 0; font-size: 20px }",
        300.0f);

    const Box* image = FindBox(*result.root, "img");
    Expect(image != nullptr, "the image has a box");
    Expect(image->Geometry().content.y == 0.0f, "it floats at the top");

    bool text_beside = false;
    for (const layout::TextFragment& fragment : AllFragments(*result.root)) {
      text_beside = text_beside || (fragment.rect.y < 50.0f && fragment.rect.x >= 100.0f);
    }
    Expect(text_beside, "and the text runs beside it rather than under it");
  });

  AddTest(tests, "Layout/AFloatAmongInlineSiblingsDoesNotBecomeInlineContent", [] {
    // The float and the text are siblings inside one paragraph. The text needs
    // an anonymous block; the float must not be inside it.
    const LaidOut result =
        Run("<p><span>F</span>aaaa bbbb</p>",
            "body { margin: 0 } p { margin: 0; font-size: 20px } "
            "span { float: left; width: 80px; height: 30px }",
            300.0f);
    const Box* floated = FindBox(*result.root, "span");
    Expect(floated != nullptr, "the span has a box");
    Expect(floated->Geometry().content.width == 80.0f,
           "float:left made a block of the span, per CSS 2.1 s9.7");

    bool text_beside = false;
    for (const layout::TextFragment& fragment : AllFragments(*result.root)) {
      text_beside = text_beside || (fragment.rect.y < 30.0f && fragment.rect.x >= 80.0f);
    }
    Expect(text_beside, "and its sibling text flows beside it");
  });

  AddTest(tests, "Layout/LinesBelowAFloatRegainTheFullWidth", [] {
    const LaidOut result = Run(
        "<div>F</div><p>aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj</p>",
        "body { margin: 0 } div { float: left; width: 150px; height: 20px; margin: 0 } "
        "p { margin: 0; font-size: 20px }",
        300.0f);

    const std::vector<layout::TextFragment> fragments = AllFragments(*result.root);
    bool narrowed = false;
    bool full = false;
    for (const layout::TextFragment& fragment : fragments) {
      narrowed = narrowed || (fragment.rect.y < 20.0f && fragment.rect.x >= 150.0f);
      full = full || (fragment.rect.y >= 20.0f && fragment.rect.x == 0.0f);
    }
    Expect(narrowed, "the first line runs beside the float");
    Expect(full, "and a line below it starts back at the edge");
  });
}

}  // namespace microbrowser::tests

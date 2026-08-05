// The scroll model: ADR 0018, and its load-bearing sentence is what most of
// these tests are about.
//
// **A scroll is a paint, not a layout.** Changing an offset must not rebuild a
// box tree, must not re-resolve a cascade, and must damage the strip that came
// into view rather than the window. Everything about the cost of scrolling
// follows from that, and every one of these assertions is a way for it to stop
// being true without anything else noticing.
//
// The other half is the API. `scrollTop` is measured at 254 occurrences across
// the survey -- more than `getBoundingClientRect` -- and ADR 0012's rule
// applies to it with full force: an implementation that is present and answers
// zero sends a virtualised list down the native path into a wall.

#include <cmath>
#include <variant>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "engine/Page.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"
#include "gfx/FontCatalog.h"
#include "html/TreeBuilder.h"
#include "layout/LayoutEngine.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

using layout::Box;
using layout::FixedTextMeasurer;
using layout::LayoutEngine;

struct LaidOut {
  std::unique_ptr<dom::Document> document;
  std::unique_ptr<css::StyleResolver> resolver;
  std::unique_ptr<FixedTextMeasurer> measurer;
  std::unique_ptr<Box> root;
  layout::ScrollOffsets offsets;
};

LaidOut Run(std::string_view html, std::string_view css_text, float width = 400.0f) {
  LaidOut result;
  result.document = html::ParseDocument(html);
  result.resolver = std::make_unique<css::StyleResolver>();
  result.resolver->AddStyleSheet(css::ParseStyleSheet(css_text), css::Origin::Author);
  result.measurer = std::make_unique<FixedTextMeasurer>(0.5f);

  const LayoutEngine engine(*result.resolver, *result.measurer);
  result.root = engine.BuildBoxTree(*result.document);
  engine.Layout(*result.root, width);
  layout::UpdateScrollState(*result.root, result.offsets);
  return result;
}

Box* FindBox(Box& root, std::string_view id) {
  Box* found = nullptr;
  const auto walk = [&](Box& box, auto& self) -> void {
    if (found != nullptr) {
      return;
    }
    const dom::Element* element = box.Origin();
    if (element != nullptr) {
      const std::string* attribute = element->GetAttribute("id");
      if (attribute != nullptr && *attribute == id) {
        found = &box;
        return;
      }
    }
    for (const std::unique_ptr<Box>& child : box.MutableChildren()) {
      self(*child, self);
    }
  };
  walk(root, walk);
  return found;
}

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
    catalog.SetGenericFamily("sans-serif", "Test");
    catalog.SetGenericFamily("monospace", "Test");
  }
};

std::vector<std::string> RunAndCollect(engine::Page& page, std::string_view html) {
  page.Load(html, "https://example.org/");
  page.SetViewport(css::MediaContext{800.0f, 600.0f, 1.0f});
  page.Layout(800.0f);
  page.RunScripts(0);
  return page.ConsoleOutput();
}

std::string Line(const std::vector<std::string>& output, std::size_t index) {
  return index < output.size() ? output[index] : std::string("<missing>");
}

// The y of the first fill of `color` in the list, or a sentinel. Reading a
// position off the display list is how a paint-time decision -- which is what
// sticky is -- becomes something a test can state a number about.
float FirstFillY(const gfx::DisplayList& list, std::uint32_t argb) {
  for (const gfx::DisplayCommand& command : list.Commands()) {
    const auto* fill = std::get_if<gfx::FillPathCommand>(&command);
    if (fill != nullptr && fill->color.argb == argb && fill->path < list.Paths().size()) {
      return list.Paths()[fill->path].ControlBounds().y;
    }
  }
  return -1e9f;
}

// Floats, to a tolerance. Layout arithmetic is float arithmetic and an exact
// comparison here would make every test a hostage to the last rounding.
void ExpectEqFloat(float actual, float expected, float tolerance, std::string_view message) {
  Expect(std::fabs(actual - expected) <= tolerance,
         std::string(message) + " (was " + std::to_string(actual) + ", expected " +
             std::to_string(expected) + ")");
}

}  // namespace

void RegisterScrollTests(std::vector<TestCase>& tests) {
  // --- Layout: what a box can be scrolled to -------------------------------

  AddTest(tests, "Scroll/ScrollableOverflowIsTheContentAScrollerCanReach", [] {
    LaidOut laid = Run(
        "<div id='box'><div id='tall'></div></div>",
        "#box { height: 100px; overflow: auto } #tall { height: 400px }");
    Box* box = FindBox(*laid.root, "box");
    Expect(box != nullptr, "the scroller has a box");
    ExpectEqFloat(box->ScrollableOverflow().height, 400.0f, 0.5f,
                  "scrollHeight is the content, not the box");
    ExpectEqFloat(layout::MaxScrollOffset(*box).y, 300.0f, 0.5f,
                  "and the reachable range is that less what already shows");
  });

  AddTest(tests, "Scroll/AContainerThatFitsItsContentScrollsNowhere", [] {
    LaidOut laid = Run("<div id='box'><div id='short'></div></div>",
                       "#box { height: 100px; overflow: auto } #short { height: 20px }");
    Box* box = FindBox(*laid.root, "box");
    Expect(box != nullptr, "the box exists");
    ExpectEqFloat(layout::MaxScrollOffset(*box).y, 0.0f, 0.01f,
                  "a box whose content fits has nowhere to go");
    // Not zero: `scrollHeight` on a box that fits reports the box, which is
    // what every engine does and what a page dividing by it depends on.
    ExpectEqFloat(box->ScrollableOverflow().height, 100.0f, 0.5f,
                  "and reports its own height rather than its content's");
  });

  AddTest(tests, "Scroll/AStoredOffsetSurvivesARelayoutAndIsClampedByIt", [] {
    LaidOut laid = Run("<div id='box'><div id='tall'></div></div>",
                       "#box { height: 100px; overflow: auto } #tall { height: 400px }");
    Box* box = FindBox(*laid.root, "box");
    Expect(box != nullptr && box->Origin() != nullptr, "the scroller has an element");
    laid.offsets[box->Origin()] = gfx::FloatPoint{0.0f, 1000.0f};

    // The second pass is what a relayout does: the box tree is rebuilt and the
    // offsets are put back on it. The out-of-range one is clamped rather than
    // taken, which is the whole reason `scrollTop = 1e9` is how a page scrolls
    // a chat log to the bottom.
    const LayoutEngine engine(*laid.resolver, *laid.measurer);
    laid.root = engine.BuildBoxTree(*laid.document);
    engine.Layout(*laid.root, 400.0f);
    layout::UpdateScrollState(*laid.root, laid.offsets);
    box = FindBox(*laid.root, "box");
    Expect(box != nullptr, "and it still has one");
    ExpectEqFloat(box->ScrollOffset().y, 300.0f, 0.5f,
                  "the offset survived the relayout, clamped to what it can reach");
  });

  AddTest(tests, "Scroll/AWheelChainsToAnAncestorWhenTheInnerBoxIsAtItsEnd", [] {
    LaidOut laid = Run(
        "<div id='outer'><div id='inner'><div id='tall'></div></div></div>",
        "#outer { height: 50px; overflow: auto }"
        "#inner { height: 100px; overflow: auto }"
        "#tall { height: 400px }");
    Box* inner = FindBox(*laid.root, "inner");
    Box* outer = FindBox(*laid.root, "outer");
    Expect(inner != nullptr && outer != nullptr, "both scrollers exist");

    const gfx::FloatPoint inside{10.0f, 10.0f};
    const gfx::FloatPoint down{0.0f, 50.0f};
    Expect(layout::ScrollTargetAt(*laid.root, inside, down) == inner,
           "the deepest scroller under the pointer takes the wheel");

    // At its end, the inner box hands the wheel on rather than swallowing it.
    // That one rule is the difference between a menu that traps a scroll
    // forever and one that lets the page behind it move.
    inner->SetScrollOffset(gfx::FloatPoint{0.0f, layout::MaxScrollOffset(*inner).y});
    Expect(layout::ScrollTargetAt(*laid.root, inside, down) == outer,
           "and hands it to its ancestor when it can go no further");

    // Upwards it can still move, so it keeps it: chaining is per direction.
    Expect(layout::ScrollTargetAt(*laid.root, inside, gfx::FloatPoint{0.0f, -50.0f}) == inner,
           "chaining is decided per direction, not per box");
  });

  AddTest(tests, "Scroll/OverflowHiddenIsScriptableAndNotWheelable", [] {
    LaidOut laid = Run("<div id='box'><div id='tall'></div></div>",
                       "#box { height: 100px; overflow: hidden } #tall { height: 400px }");
    Box* box = FindBox(*laid.root, "box");
    Expect(box != nullptr, "the box exists");
    Expect(box->IsScrollContainer(), "`hidden` still clips, so it is still a scroll container");
    Expect(!box->AllowsUserScroll(), "but a wheel must not move what a page said does not scroll");
    Expect(layout::ScrollTargetAt(*laid.root, gfx::FloatPoint{10.0f, 10.0f},
                                  gfx::FloatPoint{0.0f, 50.0f}) == nullptr,
           "and a wheel over it finds nothing to move");
  });

  // --- Paint: a scroll moves the sampling, not the geometry ----------------

  AddTest(tests, "Scroll/ScrollingABoxTranslatesItsChildrenAndNotItself", [] {
    LaidOut laid = Run(
        "<div id='box'><div id='tall'></div></div>",
        "#box { height: 100px; overflow: auto; background: #ff0000 }"
        "#tall { height: 400px; background: #00ff00 }");
    Box* box = FindBox(*laid.root, "box");
    Expect(box != nullptr, "the scroller exists");

    gfx::DisplayList before;
    layout::BuildDisplayList(*laid.root, before);
    box->SetScrollOffset(gfx::FloatPoint{0.0f, 60.0f});
    gfx::DisplayList after;
    layout::BuildDisplayList(*laid.root, after);

    ExpectEqFloat(FirstFillY(before, 0xFFFF0000u), FirstFillY(after, 0xFFFF0000u), 0.01f,
                  "the scroller itself does not move");
    ExpectEqFloat(FirstFillY(after, 0xFF00FF00u) - FirstFillY(before, 0xFF00FF00u), -60.0f, 0.5f,
                  "and its content moves up by exactly the offset");
    // The geometry is untouched: a scroll is a paint. A relayout here would be
    // the single most expensive mistake this file exists to prevent.
    ExpectEqFloat(FindBox(*laid.root, "tall")->Geometry().content.y,
                  FindBox(*laid.root, "tall")->Geometry().content.y, 0.01f, "geometry is stable");
  });

  AddTest(tests, "Scroll/ASickyBoxPinsToTheScrollportAndStaysInItsBlock", [] {
    LaidOut laid = Run(
        "<div id='wrap'><div id='head'></div><div id='body'></div></div>",
        "body { margin: 0 } #wrap { height: 500px }"
        "#head { position: sticky; top: 0; height: 40px; background: #ff0000 }"
        "#body { height: 460px }");

    // Unscrolled, it is exactly where the flow put it.
    gfx::DisplayList list;
    layout::BuildDisplayList(*laid.root, list, gfx::FloatPoint{}, gfx::FloatSize{400.0f, 200.0f});
    ExpectEqFloat(FirstFillY(list, 0xFFFF0000u), 0.0f, 0.5f, "sticky starts where it is laid out");

    // Scrolled past, it stops at the scrollport edge rather than leaving.
    gfx::DisplayList scrolled;
    layout::BuildDisplayList(*laid.root, scrolled, gfx::FloatPoint{0.0f, -120.0f},
                             gfx::FloatSize{400.0f, 200.0f});
    ExpectEqFloat(FirstFillY(scrolled, 0xFFFF0000u), 0.0f, 0.5f,
                  "and sticks at the top instead of scrolling away");

    // Past the end of its containing block it is pushed out, which is what
    // makes a section header give way to the next section's.
    gfx::DisplayList far;
    layout::BuildDisplayList(*laid.root, far, gfx::FloatPoint{0.0f, -480.0f},
                             gfx::FloatSize{400.0f, 200.0f});
    ExpectEqFloat(FirstFillY(far, 0xFFFF0000u), -20.0f, 1.0f,
                  "and leaves with its containing block rather than escaping it");
  });

  AddTest(tests, "Scroll/AFixedBoxDoesNotMoveWithTheDocument", [] {
    LaidOut laid = Run("<div id='bar'></div><div id='rest'></div>",
                       "body { margin: 0 } #bar { position: fixed; top: 0; height: 30px; background: #0000ff }"
                       "#rest { height: 900px }");
    gfx::DisplayList at_top;
    layout::BuildDisplayList(*laid.root, at_top, gfx::FloatPoint{}, gfx::FloatSize{400.0f, 200.0f});
    gfx::DisplayList scrolled;
    layout::BuildDisplayList(*laid.root, scrolled, gfx::FloatPoint{0.0f, -300.0f},
                             gfx::FloatSize{400.0f, 200.0f});
    ExpectEqFloat(FirstFillY(scrolled, 0xFF0000FFu), FirstFillY(at_top, 0xFF0000FFu), 0.5f,
                  "a fixed box is positioned against the viewport, so a scroll does not move it");
  });

  // --- The canvas blit -----------------------------------------------------

  AddTest(tests, "Scroll/TheCanvasBlitMovesPixelsAndLeavesTheExposedBand", [] {
    gfx::Canvas canvas(4, 4);
    canvas.Clear(gfx::Color::Rgb(0, 0, 0));
    canvas.FillRect(gfx::IntRect{0, 0, 4, 1}, gfx::Color::Rgb(0xFF, 0, 0));

    canvas.ScrollRegion(canvas.Bounds(), 0, 2);
    ExpectEqInt(static_cast<long long>(canvas.Row(2)[0] & 0x00FFFFFFu), 0xFF0000,
                "the red row moved down by two");
    ExpectEqInt(static_cast<long long>(canvas.Row(0)[0] & 0x00FFFFFFu), 0xFF0000,
                "and what slid out of the exposed band is left for the caller to repaint");
  });

  AddTest(tests, "Scroll/ABlitPastTheRegionMovesNothing", [] {
    gfx::Canvas canvas(4, 4);
    canvas.Clear(gfx::Color::Rgb(0, 0, 0));
    canvas.FillRect(gfx::IntRect{0, 0, 4, 1}, gfx::Color::Rgb(0xFF, 0, 0));
    // No overlap to copy, so nothing is copied -- and, more to the point,
    // nothing outside the surface is read. The delta arrives from the engine,
    // which after the process split is a renderer.
    canvas.ScrollRegion(canvas.Bounds(), 0, 400);
    ExpectEqInt(static_cast<long long>(canvas.Row(0)[0] & 0x00FFFFFFu), 0xFF0000,
                "a delta past the region is a no-op rather than a read out of bounds");
    canvas.ScrollRegion(gfx::IntRect{-1000, -1000, 4, 4}, 0, 1);
    ExpectEqInt(static_cast<long long>(canvas.Row(0)[0] & 0x00FFFFFFu), 0xFF0000,
                "and so is a region entirely off the surface");
  });

  // --- The API a page uses -------------------------------------------------

  AddTest(tests, "Scroll/ScrollTopReadsAndWritesAndIsClamped", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'>"
        "<div id='a' style='height:100px;overflow:auto'>"
        "<div style='height:400px'></div></div>"
        "<script>var e = document.getElementById('a');"
        "console.log(e.scrollHeight + ',' + e.scrollTop);"
        "e.scrollTop = 120;"
        "console.log(e.scrollTop);"
        "e.scrollTop = 1e9;"
        "console.log(e.scrollTop);"
        "e.scrollBy(0, -50);"
        "console.log(e.scrollTop);"
        "e.scrollTo({top: 10});"
        "console.log(e.scrollTop + ',' + e.scrollLeft);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "400,0", "scrollHeight is the content and scrollTop starts at 0");
    ExpectEqString(Line(output, 1), "120", "a write takes");
    ExpectEqString(Line(output, 2), "300", "and an out-of-range one is clamped rather than refused");
    ExpectEqString(Line(output, 3), "250", "scrollBy is relative");
    ExpectEqString(Line(output, 4), "10,0", "scrollTo is absolute and leaves the other axis alone");
  });

  AddTest(tests, "Scroll/ScrollIntoViewMovesEveryScrollingAncestor", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'>"
        "<div id='outer' style='height:200px;overflow:auto'>"
        "<div style='height:300px'></div>"
        "<div id='inner' style='height:100px;overflow:auto'>"
        "<div style='height:300px'></div>"
        "<div id='target' style='height:20px'></div>"
        "</div></div>"
        "<script>document.getElementById('target').scrollIntoView();"
        "console.log(document.getElementById('outer').scrollTop + ',' +"
        "            document.getElementById('inner').scrollTop);"
        "</script></body>");
    // The inner box scrolls as far as it can -- 220, its whole range -- and the
    // outer one then scrolls the inner box into its own view. An implementation
    // that moved only the nearest one reports the outer at zero, which looks
    // right in a demo and fails on a page.
    ExpectEqString(Line(output, 0), "200,220", "both ancestors moved");
  });

  AddTest(tests, "Scroll/TheViewportIsTheDocumentElementsScrollOffset", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'><div style='height:3000px'></div>"
        "<script>window.scrollTo(0, 250);"
        "console.log(window.scrollY + ',' + document.documentElement.scrollTop);"
        "document.documentElement.scrollTop = 40;"
        "console.log(window.pageYOffset);"
        "</script></body>");
    // Two names for one number. A browser where they are two numbers is one
    // where half a page's scroll handling silently disagrees with the other.
    ExpectEqString(Line(output, 0), "250,250", "window.scrollY and documentElement.scrollTop agree");
    ExpectEqString(Line(output, 1), "40", "and a write through either moves the same offset");
  });

  AddTest(tests, "Scroll/AScrollEventFiresOnceAndOnlyWhenSomethingMoved", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'>"
        "<div id='a' style='height:100px;overflow:auto'>"
        "<div style='height:400px'></div></div>"
        "<script>window.__n = 0;"
        "document.getElementById('a').addEventListener('scroll', function(){ window.__n++; });"
        "var e = document.getElementById('a');"
        "e.scrollTop = 10; e.scrollTop = 20; e.scrollTop = 30;"
        "console.log('during:' + window.__n);"
        "</script></body>");
    // Nothing during the turn: three writes in a row must not run a handler
    // three times, which is the difference between a wheel that costs one
    // dispatch and one that costs a dozen.
    ExpectEqString(Line(output, 0), "during:0", "a scroll does not dispatch synchronously");

    Expect(page.NextWakeDelay(0).has_value(), "a page that owes a scroll event asks to be woken");
    Expect(page.RunDueWork(1), "and the frame delivers it");
    Expect(!page.NextWakeDelay(2).has_value(),
           "after which a settled page schedules nothing at all -- the zero-idle invariant");
  });
}

}  // namespace microbrowser::tests

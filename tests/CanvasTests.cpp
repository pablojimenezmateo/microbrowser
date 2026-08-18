#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "bindings/Canvas.h"
#include "engine/CanvasSurfaces.h"
#include "html/TreeBuilder.h"
#include "support/SyntheticFont.h"
#include "gfx/FontCatalog.h"
#include "gfx/TextRenderer.h"
#include "dom/Node.h"

namespace microbrowser::tests {

using bindings::CanvasOp;
using engine::CanvasSurfaces;

namespace {

// A font catalog with the synthetic test face, which is what every other test that needs one uses.
// Canvas needs one because `fillText` and `measureText` go through the real shaper.
struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
    catalog.SetGenericFamily("sans-serif", "Test");
  }
};

// A `<canvas>` element from markup, so its attributes are the ones a page would write.
struct CanvasFixture {
  explicit CanvasFixture(const std::string& markup) {
    document = html::ParseDocument("<body>" + markup + "</body>");
    document->ForEachDescendant([this](const dom::Node& node) {
      if (element == nullptr && node.IsElement() &&
          static_cast<const dom::Element&>(node).TagName() == "canvas") {
        element = const_cast<dom::Element*>(static_cast<const dom::Element*>(&node));
      }
    });
  }
  std::unique_ptr<dom::Document> document;
  dom::Element* element = nullptr;
};

CanvasOp Op(CanvasOp::Kind kind, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
  CanvasOp op;
  op.kind = kind;
  op.a = a;
  op.b = b;
  op.c = c;
  op.d = d;
  return op;
}

CanvasOp ColorOp(CanvasOp::Kind kind, const char* text) {
  CanvasOp op;
  op.kind = kind;
  op.text = text;
  return op;
}

// One pixel, as "r,g,b,a", which is what an assertion about drawing can be read as.
std::string PixelAt(const CanvasSurfaces& surfaces, const dom::Element& element, int x, int y) {
  const std::vector<std::uint8_t> pixels = surfaces.ReadPixels(element, x, y, 1, 1);
  if (pixels.size() < 4) {
    return "unreadable";
  }
  return std::to_string(pixels[0]) + "," + std::to_string(pixels[1]) + "," +
         std::to_string(pixels[2]) + "," + std::to_string(pixels[3]);
}

}  // namespace

void RegisterCanvasTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Canvas2D/TheBackingStoreComesFromTheAttributes", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture sized("<canvas width=320 height=200></canvas>");
    Expect(sized.element != nullptr, "the element parsed");
    const CanvasSurfaces::Surface* surface = surfaces.For(*sized.element);
    Expect(surface != nullptr, "and it has a surface");
    // **The bug this asserts against was silent.** A store created at the default 300x150 regardless of
    // the attributes means every page that writes `<canvas width=320 height=200>` -- which is nearly all
    // of them -- draws into a smaller canvas than it asked for, and the drawing is merely clipped rather
    // than visibly wrong. Found by probing a real page, which reported `canvas 300x150`.
    ExpectEqInt(surface->canvas.Width(), 320, "the width attribute sizes the store");
    ExpectEqInt(surface->canvas.Height(), 200, "and the height attribute too");

    CanvasFixture bare("<canvas></canvas>");
    const CanvasSurfaces::Surface* defaulted = surfaces.For(*bare.element);
    ExpectEqInt(defaulted->canvas.Width(), CanvasSurfaces::kDefaultWidth,
                "and with no attributes it is the specification's 300x150");
    ExpectEqInt(defaulted->canvas.Height(), CanvasSurfaces::kDefaultHeight, "150 tall");
  });

  AddTest(tests, "Canvas2D/FillAndClearAreDifferentOperations", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture fixture("<canvas width=50 height=50></canvas>");
    dom::Element& canvas = *fixture.element;
    surfaces.Execute(canvas, ColorOp(CanvasOp::Kind::SetFillColor, "rgb(10,20,30)"));
    surfaces.Execute(canvas, Op(CanvasOp::Kind::FillRect, 0, 0, 40, 40));
    ExpectEqString(PixelAt(surfaces, canvas, 5, 5), "10,20,30,255", "the fill landed");
    // **`clearRect` writes, it does not fill.** Every fill path in `gfx` blends, and `Canvas::FillRect`
    // returns immediately for a fully transparent colour -- so a `clearRect` written as a fill of
    // transparent black silently does nothing. Which is what the first version did: a probe read
    // `16,16,16,255` out of a region the page had just cleared.
    surfaces.Execute(canvas, Op(CanvasOp::Kind::ClearRect, 10, 10, 10, 10));
    ExpectEqString(PixelAt(surfaces, canvas, 15, 15), "0,0,0,0", "and the clear cleared");
    ExpectEqString(PixelAt(surfaces, canvas, 5, 5), "10,20,30,255", "without touching its neighbour");
  });

  AddTest(tests, "Canvas2D/SaveAndRestoreMoveTheWholeStateTogether", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture fixture("<canvas width=60 height=60></canvas>");
    dom::Element& canvas = *fixture.element;
    surfaces.Execute(canvas, ColorOp(CanvasOp::Kind::SetFillColor, "rgb(255,0,0)"));
    surfaces.Execute(canvas, Op(CanvasOp::Kind::Save));
    // Inside the save: a different colour *and* a translation. The two have to come back together --
    // parallel stacks would be the bug where a `restore()` puts back the colour and not the transform.
    surfaces.Execute(canvas, ColorOp(CanvasOp::Kind::SetFillColor, "rgb(0,0,255)"));
    CanvasOp translate = Op(CanvasOp::Kind::Transform, 1, 0, 0, 1);
    translate.e = 30.0;
    translate.f = 30.0;
    surfaces.Execute(canvas, translate);
    surfaces.Execute(canvas, Op(CanvasOp::Kind::FillRect, 0, 0, 10, 10));
    ExpectEqString(PixelAt(surfaces, canvas, 35, 35), "0,0,255,255",
                   "blue, at the translated origin");
    surfaces.Execute(canvas, Op(CanvasOp::Kind::Restore));
    surfaces.Execute(canvas, Op(CanvasOp::Kind::FillRect, 0, 0, 10, 10));
    ExpectEqString(PixelAt(surfaces, canvas, 5, 5), "255,0,0,255",
                   "and after the restore, red at the untranslated origin");
    // An unbalanced `restore()` is a no-op rather than an error, which is the specification's rule.
    for (int i = 0; i < 5; ++i) {
      surfaces.Execute(canvas, Op(CanvasOp::Kind::Restore));
    }
    surfaces.Execute(canvas, Op(CanvasOp::Kind::FillRect, 20, 20, 5, 5));
    ExpectEqString(PixelAt(surfaces, canvas, 22, 22), "255,0,0,255", "and changes nothing");
  });

  AddTest(tests, "Canvas2D/AnUnparseableColourLeavesThePreviousOne", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture fixture("<canvas width=20 height=20></canvas>");
    dom::Element& canvas = *fixture.element;
    surfaces.Execute(canvas, ColorOp(CanvasOp::Kind::SetFillColor, "rgb(1,2,3)"));
    // The specification's rule: an unparseable `fillStyle` assignment is *ignored*, so the previous
    // colour survives. Falling back to black would repaint everything after the typo in black.
    surfaces.Execute(canvas, ColorOp(CanvasOp::Kind::SetFillColor, "not-a-colour"));
    surfaces.Execute(canvas, Op(CanvasOp::Kind::FillRect, 0, 0, 10, 10));
    ExpectEqString(PixelAt(surfaces, canvas, 2, 2), "1,2,3,255", "the previous colour is still in use");
  });

  AddTest(tests, "Canvas2D/ResizingResetsTheStateAndNotTheTaint", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture fixture("<canvas width=40 height=40></canvas>");
    dom::Element& canvas = *fixture.element;
    surfaces.Execute(canvas, ColorOp(CanvasOp::Kind::SetFillColor, "rgb(9,9,9)"));
    surfaces.Execute(canvas, Op(CanvasOp::Kind::FillRect, 0, 0, 40, 40));
    // A cross-origin draw, which is the only thing that taints. Set at the *draw*: a flag computed at
    // read time would have to re-derive what had been drawn, and getting that wrong means a page
    // reading pixels of an image it was never allowed to see.
    //
    // The taint decision itself is `Page`'s -- it is the object that knows what an `<img>` fetched
    // and from which origin -- so it arrives here as an argument rather than on the command. What
    // this test pins is the half that lives here: once set, it is never cleared.
    auto source = std::make_shared<gfx::Image>();
    source->Adopt(1, 1, std::vector<std::uint32_t>{0xFF112233u});
    CanvasOp drawing = Op(CanvasOp::Kind::DrawImage);
    drawing.c = 1;
    drawing.d = 1;
    drawing.g = 1;
    drawing.h = 1;
    surfaces.DrawImage(canvas, drawing, source, /*taints=*/true);
    Expect(surfaces.ReadPixels(canvas, 0, 0, 1, 1).empty(),
           "a tainted canvas reads as nothing, which the binding turns into a SecurityError");
    // `canvas.width = canvas.width` is the idiomatic clear, so a resize resets the state and the pixels.
    surfaces.SetSize(canvas, 40, 40);
    const CanvasSurfaces::Surface* surface = surfaces.Find(canvas);
    Expect(surface->state.fill == gfx::Color::Rgb(0, 0, 0), "the state went back to its initial value");
    // **And the taint did not.** A resize does not un-see the pixels that were drawn, and clearing it
    // here would be a one-line bypass of the whole check.
    Expect(surface->tainted, "the taint survives a resize");
    Expect(surfaces.ReadPixels(canvas, 0, 0, 1, 1).empty(), "so the canvas is still unreadable");
  });

  AddTest(tests, "Canvas2D/SizesAPageChoosesAreBounded", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture fixture("<canvas width=10 height=10></canvas>");
    dom::Element& canvas = *fixture.element;
    // `canvas.width = 1e9` is one line, and a page controls both the size and how many canvases it
    // makes. Refused, and the canvas keeps the size it had -- which is what the specification says for
    // a size the implementation cannot support, and is better than throwing: a page that catches
    // nothing would stop.
    surfaces.SetSize(canvas, 100000, 100000);
    const CanvasSurfaces::Surface* surface = surfaces.Find(canvas);
    ExpectEqInt(surface->canvas.Width(), 10, "the refused size left the old one");
    surfaces.SetSize(canvas, 64, 32);
    ExpectEqInt(surfaces.Find(canvas)->canvas.Width(), 64, "and a reasonable one is taken");
  });

  AddTest(tests, "Canvas2D/PixelsRoundTripThroughGetAndPutImageData", [] {
    TestFonts fonts;
    gfx::TextRenderer text(fonts.catalog);
    CanvasSurfaces surfaces(text);
    CanvasFixture fixture("<canvas width=8 height=8></canvas>");
    dom::Element& canvas = *fixture.element;
    const std::vector<std::uint8_t> green{0, 200, 0, 255, 0, 200, 0, 255};
    surfaces.WritePixels(canvas, 2, 3, 2, 1, green);
    ExpectEqString(PixelAt(surfaces, canvas, 2, 3), "0,200,0,255", "what was written is what is read");
    ExpectEqString(PixelAt(surfaces, canvas, 3, 3), "0,200,0,255", "both pixels");
    ExpectEqString(PixelAt(surfaces, canvas, 4, 3), "0,0,0,0", "and not the one after them");
    // A short buffer is refused rather than partially applied: half a filter's output written and half
    // left is worse than none, because the page cannot tell.
    surfaces.WritePixels(canvas, 0, 0, 4, 4, green);
    ExpectEqString(PixelAt(surfaces, canvas, 0, 0), "0,0,0,0", "a short buffer wrote nothing");
    // Reading outside the canvas is transparent black rather than a refusal, which is what the
    // specification says and what lets a page read a region that straddles the edge.
    const std::vector<std::uint8_t> outside = surfaces.ReadPixels(canvas, 100, 100, 2, 2);
    ExpectEqInt(static_cast<long long>(outside.size()), 16, "the read succeeds");
    Expect(outside[0] == 0 && outside[3] == 0, "and answers transparent black");
  });
}

}  // namespace microbrowser::tests

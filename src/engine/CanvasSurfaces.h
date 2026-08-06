#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bindings/Canvas.h"
#include "gfx/Canvas.h"
#include "gfx/Image.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "gfx/TextRenderer.h"

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::engine {

// One backing store per `<canvas>`, and the graphics state a context keeps.
//
// ADR 0029 §2, session 36. This is the far side of `bindings::CanvasSurface`: the binding layer sends
// commands and this executes them against a real `gfx::Painter`, which is the machinery this project
// already built for painting pages.
//
// **The state lives here rather than in the binding layer**, and that is the load-bearing choice. A
// canvas context's state -- fill colour, line width, transform, clip, and the save stack -- is what
// every command reads, and two copies of it is how a `restore()` ends up restoring something the
// painter never had. So `save()` and `fillStyle =` are commands like any other, and there is one
// source of truth.
//
// The other thing here is **tainting**, which is a security boundary rather than a privacy one. Drawing
// a cross-origin image without CORS taints the canvas, and a tainted canvas refuses every read for the
// rest of its life. Set at the *draw*, never at the read: a flag computed at read time would have to
// re-derive what had been drawn, and getting that wrong means a page reading pixels of an image it was
// never allowed to see.
class CanvasSurfaces {
 public:
  // The default size the specification gives a `<canvas>` with no attributes. Not zero: a page that
  // draws into an unsized canvas expects a 300x150 one, and the number is in the specification
  // precisely because it is arbitrary.
  static constexpr int kDefaultWidth = 300;
  static constexpr int kDefaultHeight = 150;

  // The largest backing store one canvas may have, and the total across a document.
  //
  // **A page controls both**, which is why there are two: `canvas.width = 1e9` is one line, and so is a
  // loop creating a thousand canvases. Sixteen megapixels is a 4096x4096 canvas, past anything a real
  // page uses and still 64MB of pixels; the document total is four of those. Exceeding either leaves
  // the canvas at its previous size rather than throwing, which is what the specification says for a
  // size the implementation cannot support.
  static constexpr std::int64_t kMaxCanvasPixels = 16 * 1024 * 1024;
  static constexpr std::int64_t kMaxDocumentPixels = 64 * 1024 * 1024;

  // The clip, the transform and the paint state, as one thing a `save()` copies.
  //
  // A struct rather than parallel stacks, because the specification's `save()` pushes *all* of it and
  // nothing else: parallel stacks would be several things that have to be pushed and popped together,
  // and the bug that produces is a `restore()` that puts back the colour and not the transform.
  struct State {
    gfx::Color fill = gfx::Color::Rgb(0, 0, 0);
    gfx::Color stroke = gfx::Color::Rgb(0, 0, 0);
    gfx::StrokeStyle line;
    gfx::AffineTransform transform;
    // The clip as a rectangle, because that is what `gfx::Canvas` supports. A non-rectangular
    // `clip()` -- a path -- is intersected with its *bounding box* instead, which is a stated
    // approximation: it clips less than asked, never more, so nothing is hidden that should be visible
    // and something may be visible that should be hidden. The alternative is a coverage mask per clip,
    // which is a change to `gfx::Canvas`.
    gfx::IntRect clip;
    float alpha = 1.0f;
    std::string font = "10px sans-serif";
    // `textAlign` and `textBaseline`, as the offsets they amount to. Stored as the enumeration the
    // specification uses rather than as a number, because `start` and `end` depend on direction.
    enum class Align : std::uint8_t { Start, End, Left, Right, Center };
    enum class Baseline : std::uint8_t { Alphabetic, Top, Middle, Bottom, Hanging, Ideographic };
    Align align = Align::Start;
    Baseline baseline = Baseline::Alphabetic;
  };

  explicit CanvasSurfaces(gfx::TextRenderer& text) : text_(&text) {}

  // The surface for an element, created at the default size on first use. Null only when the size is
  // refused, which a caller treats as "nothing to draw on".
  struct Surface {
    gfx::Canvas canvas;
    std::vector<State> stack;
    State state;
    gfx::Path path;
    bool tainted = false;
    // Whether anything has been drawn since the last time the page's bitmap was taken. A canvas nobody
    // draws into costs no image copy per frame, which matters: the bitmap crosses to paint.
    bool dirty = true;
    std::shared_ptr<const gfx::Image> snapshot;
  };

  Surface* For(const dom::Element& element);
  const Surface* Find(const dom::Element& element) const;

  void SetSize(dom::Element& element, int width, int height);
  void Execute(dom::Element& element, const bindings::CanvasOp& op);

  // The pixels, for painting. A shared image rather than a copy per frame -- and rebuilt only when
  // something was drawn, which is what `dirty` is for.
  std::shared_ptr<const gfx::Image> Snapshot(const dom::Element& element);

  std::vector<std::uint8_t> ReadPixels(const dom::Element& element, int x, int y, int width,
                                       int height) const;
  void WritePixels(dom::Element& element, int x, int y, int width, int height,
                   const std::vector<std::uint8_t>& rgba);
  double MeasureText(const dom::Element& element, const std::string& text) const;

  // A navigation. Every backing store goes -- a canvas is the largest thing a document can hold and
  // one that outlived its page would be 64MB leaked per navigation.
  void Clear() { surfaces_.clear(); }

 private:
  std::int64_t PixelsHeld() const;

  std::map<const dom::Element*, Surface> surfaces_;
  gfx::TextRenderer* text_;
};

}  // namespace microbrowser::engine

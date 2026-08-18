#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::bindings {

// `<canvas>` and its 2D context, as what this module can say about them.
//
// ADR 0029 §2, session 36. The ADR's argument for building this is that **`src/gfx` already is a 2D
// canvas** -- an analytic-AA rasterizer, a stroker, paths, affine transforms, image scaling and a text
// shaper, all built for the browser's own painting. Canvas 2D is a binding over that.
//
// Which is exactly why this module cannot do it directly: `src/bindings` may see `util`, `js`, `dom`
// and `html`, and **not `gfx`**. So the drawing crosses as *commands* and `src/engine` executes them
// against a real `gfx::Painter`.
//
// **One command type rather than forty virtuals**, and that is the design decision worth stating. The
// alternative -- a virtual per method -- would put the whole Canvas API on one interface and make the
// seam as wide as the feature. A command is data: it can be counted, bounded, logged, and (later) sent
// to another process, which is the same reasoning `gfx::DisplayList` is built on. The state -- the fill
// colour, the transform, the save stack -- lives with the `Painter` on the far side, because two copies
// of a graphics state is how a `restore()` ends up restoring something else.
struct CanvasOp {
  enum class Kind : std::uint8_t {
    // State
    Save,
    Restore,
    SetFillColor,
    SetStrokeColor,
    SetLineWidth,
    SetLineCap,
    SetLineJoin,
    SetMiterLimit,
    SetGlobalAlpha,
    SetFont,
    SetTextAlign,
    SetTextBaseline,
    SetLineDash,
    SetLineDashOffset,
    SetGlobalCompositeOperation,
    SetShadowColor,
    SetShadowBlur,
    SetShadowOffsetX,
    SetShadowOffsetY,
    SetImageSmoothing,
    SetDirection,
    // The whole state at once, back to its defaults, plus the path and the save stack. The
    // specification's `reset()`, and the same algorithm a `canvas.width = w` write runs.
    Reset,
    // Transforms
    Transform,      // a..f: the matrix, multiplied into the current one
    SetTransform,   // a..f: replaces it
    ResetTransform,
    // Paths
    BeginPath,
    MoveTo,
    LineTo,
    QuadraticCurveTo,
    BezierCurveTo,
    Arc,
    ArcTo,
    Rect,
    RoundRect,
    ClosePath,
    Fill,
    Stroke,
    Clip,
    // Rectangles, which are their own commands rather than paths because the specification says they
    // ignore the current path entirely -- `fillRect` between a `moveTo` and a `fill` must not join it.
    FillRect,
    StrokeRect,
    ClearRect,
    // Paint sources. A gradient or a pattern is an *object* a page holds and assigns to `fillStyle`
    // later, so it cannot be a colour string: the binding layer mints a handle, these commands tell
    // the far side what it names, and `SetFillPaint` selects one. The handle is chosen on this side
    // deliberately -- a command that had to return a value would make every other command's return
    // type a question, and the seam stays "send data, get nothing back".
    CreateLinearGradient,
    CreateRadialGradient,
    CreateConicGradient,
    AddColorStop,
    CreatePattern,
    SetFillPaint,
    SetStrokePaint,
    // Text and images
    FillText,
    StrokeText,
    DrawImage,
  };

  Kind kind = Kind::Save;
  // Up to eight numbers, which is what the widest command needs (`drawImage` with both rectangles).
  // Named `a`..`h` rather than `x1`, `y1` because their meaning changes per command and a name that is
  // right for one would be wrong for the next.
  double a = 0.0;
  double b = 0.0;
  double c = 0.0;
  double d = 0.0;
  double e = 0.0;
  double f = 0.0;
  double g = 0.0;
  double h = 0.0;
  // A colour as the page wrote it, parsed on the far side by the one CSS colour parser. Not parsed
  // here: `src/bindings` may not see `gfx::Color`, and a second colour parser is a second answer about
  // what `rgb(300, -1, 50%)` means.
  std::string text;
  bool flag = false;
  // The variable-length arguments: `setLineDash`'s array and `roundRect`'s radii. A vector rather
  // than more scalars because both are page-chosen in length, and a fixed eight would be a silent
  // truncation of exactly the input a page controls.
  std::vector<double> numbers;
  // A paint source, named rather than held: `src/bindings` cannot hold a `gfx::Color` let alone a
  // gradient, so what crosses is the number the binding layer minted for it. Zero is "none".
  std::uint32_t handle = 0;
  // The source of a `drawImage` or a `createPattern` -- an `<img>`, a `<canvas>` or a `<video>`.
  // An element rather than pixels, because the pixels live on the far side already and copying them
  // through this seam would make every draw a bitmap copy.
  const dom::Element* source = nullptr;
};

// The canvases a document owns, and the one thing a page can do with them that is a security decision.
class CanvasSurface {
 public:
  virtual ~CanvasSurface() = default;

  // Whether this element is a `<canvas>` at all, asked before anything else -- so `div.getContext` is
  // undefined rather than a function that answers about nothing.
  virtual bool IsCanvas(const dom::Element& element) const = 0;

  // The backing store's size in device pixels, which is the `width`/`height` *attributes* rather than
  // the CSS box. Setting either resets the canvas to transparent black, which is the specification's
  // rule and a real one: pages use `canvas.width = canvas.width` as the idiomatic clear.
  virtual void SetCanvasSize(dom::Element& element, int width, int height) = 0;
  virtual int CanvasWidth(const dom::Element& element) const = 0;
  virtual int CanvasHeight(const dom::Element& element) const = 0;

  // One drawing command. Executed immediately, because that is what the API is: `ctx.fillRect()` has
  // drawn by the time it returns, and a page that draws and then reads back depends on it.
  virtual void ExecuteCanvasOp(dom::Element& element, const CanvasOp& op) = 0;

  // `getImageData`, as premultiplied-alpha-undone RGBA bytes in row order.
  //
  // **Empty when the canvas is tainted**, which the caller turns into a `SecurityError`. Tainting is
  // a security boundary rather than a privacy one and is set at the *draw*: once a cross-origin image
  // without CORS has been drawn, every later read fails, because the alternative is a page reading
  // pixels of an image it was never allowed to see.
  virtual std::vector<std::uint8_t> ReadCanvasPixels(const dom::Element& element, int x, int y,
                                                     int width, int height) const = 0;
  virtual bool CanvasIsTainted(const dom::Element& element) const = 0;
  virtual void WriteCanvasPixels(dom::Element& element, int x, int y, int width, int height,
                                 const std::vector<std::uint8_t>& rgba) = 0;

  // `measureText`'s width, which needs the shaper and therefore the far side.
  virtual double MeasureCanvasText(const dom::Element& element, const std::string& text) const = 0;

  // The graphics state, read back.
  //
  // **There is no shadow copy on this side, and that is the whole point.** The state lives with the
  // painter (see `ExecuteCanvasOp`), so `ctx.fillStyle` has to ask it -- otherwise `save()`,
  // `restore()` and `reset()` would each have to remember to update a second copy, and the one that
  // forgot would be a getter that disagrees with what the next `fill()` draws. `Kind` is the key
  // because the setter already names it: `SetFillColor` reads back what `SetFillColor` wrote.
  //
  // Text-valued state comes back *serialized* -- `#ff0000` rather than the `#f00` the page wrote --
  // because the specification says the getter returns the value's canonical form, which means the
  // one colour parser has to have seen it.
  virtual std::string CanvasStateText(const dom::Element& element, CanvasOp::Kind which) const = 0;
  virtual double CanvasStateNumber(const dom::Element& element, CanvasOp::Kind which) const = 0;

  // The current transformation matrix, as `a, b, c, d, e, f`. Six numbers rather than a matrix type
  // because there is no matrix type this module may name.
  virtual std::vector<double> CanvasTransform(const dom::Element& element) const = 0;

  // `isPointInPath` and `isPointInStroke`, which are questions rather than commands: the answer
  // depends on the path the far side is holding, and a copy of it on this side would be a second
  // path to keep in step.
  virtual bool CanvasHitTest(const dom::Element& element, double x, double y, bool stroke,
                             bool even_odd) const = 0;

  // Whether the one CSS colour parser accepts this text.
  //
  // Asked rather than answered here for the reason `CanvasOp::text` carries a colour unparsed: a
  // second colour parser in this module would be a second answer about what `rgb(300, -1, 50%)`
  // means. It exists because `addColorStop` has to *throw* on a colour it cannot parse, and that
  // decision has to be made on this side of the seam -- a command that silently dropped the stop
  // would leave a page believing it had drawn a gradient it never described.
  virtual bool CanvasParsesColor(const std::string& text) const = 0;
};

}  // namespace microbrowser::bindings

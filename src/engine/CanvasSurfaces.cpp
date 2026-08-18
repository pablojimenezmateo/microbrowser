#include "engine/CanvasSurfaces.h"

#include "engine/CanvasComposite.h"
#include "engine/CanvasGeometry.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "css/ComputedStyle.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "layout/FontTextMeasurer.h"
#include "dom/Node.h"
#include "gfx/ColorText.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The canvas backing store holds **premultiplied** ARGB, and these two are the only places that
// matters.
//
// It is not a choice so much as a consequence: `gfx::BlendSrcOver` computes
// `out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)`, which is the premultiplied formula. On a page
// the destination is always opaque, so premultiplied and not are the same number and nobody ever had
// to say which this was. A canvas starts *transparent*, and there it is the difference between
// `fillStyle = 'rgba(0,255,0,0.5)'` reading back as green at half alpha and reading back as a dark
// green -- which is what every `2d.fillStyle.parse` test with an alpha was measuring.
std::uint32_t Unpremultiply(std::uint32_t argb) {
  const std::uint32_t alpha = (argb >> 24) & 0xFFu;
  if (alpha == 0 || alpha == 255) {
    return alpha == 0 ? 0u : argb;
  }
  const auto channel = [alpha](std::uint32_t value) {
    return std::min(255u, (value * 255u + alpha / 2) / alpha);
  };
  return (alpha << 24) | (channel((argb >> 16) & 0xFFu) << 16) |
         (channel((argb >> 8) & 0xFFu) << 8) | channel(argb & 0xFFu);
}

std::uint32_t Premultiply(std::uint32_t argb) {
  const std::uint32_t alpha = (argb >> 24) & 0xFFu;
  if (alpha == 255) {
    return argb;
  }
  if (alpha == 0) {
    return 0;
  }
  return (alpha << 24) | (gfx::MulDiv255((argb >> 16) & 0xFFu, alpha) << 16) |
         (gfx::MulDiv255((argb >> 8) & 0xFFu, alpha) << 8) |
         gfx::MulDiv255(argb & 0xFFu, alpha);
}

// A colour as the page wrote it, through the one CSS colour parser.
//
// Falling back to opaque black is the specification's rule for an unparseable `fillStyle`: the
// assignment is *ignored*, so the previous value survives. Which is why this returns whether it parsed
// rather than a colour -- a caller that took a fallback would overwrite a colour the page set earlier.
bool ParseCanvasColor(const std::string& text, gfx::Color& out) {
  const std::optional<gfx::Color> parsed = css::ParseColor(text);
  if (!parsed.has_value()) {
    return false;
  }
  out = *parsed;
  return true;
}

gfx::FloatPoint Apply(const gfx::AffineTransform& transform, double x, double y) {
  return transform.MapPoint(gfx::FloatPoint{static_cast<float>(x), static_cast<float>(y)});
}

bool Finite(double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
}

}  // namespace

std::int64_t CanvasSurfaces::PixelsHeld() const {
  std::int64_t total = 0;
  for (const auto& [element, surface] : surfaces_) {
    total += static_cast<std::int64_t>(surface.canvas.Width()) * surface.canvas.Height();
  }
  return total;
}

CanvasSurfaces::Surface* CanvasSurfaces::For(const dom::Element& element) {
  const auto found = surfaces_.find(&element);
  if (found != surfaces_.end()) {
    return &found->second;
  }
  // **The size comes from the element's attributes**, not from the default. `<canvas width=320
  // height=200>` is how nearly every page sizes one, and a backing store created at 300x150 regardless
  // means every such page draws into a smaller canvas than it asked for -- silently, because the CSS
  // box is the attribute size and the drawing is just clipped. The probe that found this read
  // `canvas 300x150` for an element that said 320x200.
  const auto attribute = [&element](const char* name, int fallback) {
    const std::string* value = element.GetAttribute(name);
    if (value == nullptr) {
      return fallback;
    }
    const std::optional<float> parsed = util::ParseFloat(*value);
    if (!parsed.has_value() || *parsed < 0.0f ||
        static_cast<std::int64_t>(*parsed) > kMaxCanvasPixels) {
      return fallback;
    }
    return static_cast<int>(*parsed);
  };
  const int width = attribute("width", kDefaultWidth);
  const int height = attribute("height", kDefaultHeight);
  if (static_cast<std::int64_t>(width) * height > kMaxCanvasPixels ||
      PixelsHeld() + static_cast<std::int64_t>(width) * height > kMaxDocumentPixels) {
    return nullptr;
  }
  Surface surface;
  surface.canvas = gfx::Canvas(width, height);
  surface.state.clip = surface.canvas.Bounds();
  AddPerformanceCounter(PerfCounterId::CanvasesCreated);
  return &surfaces_.emplace(&element, std::move(surface)).first->second;
}

const CanvasSurfaces::Surface* CanvasSurfaces::Find(const dom::Element& element) const {
  const auto found = surfaces_.find(&element);
  return found == surfaces_.end() ? nullptr : &found->second;
}

void CanvasSurfaces::SetSize(dom::Element& element, int width, int height) {
  Surface* surface = For(element);
  if (surface == nullptr) {
    return;
  }
  const int clamped_width = std::max(0, width);
  const int clamped_height = std::max(0, height);
  const std::int64_t wanted = static_cast<std::int64_t>(clamped_width) * clamped_height;
  const std::int64_t current =
      static_cast<std::int64_t>(surface->canvas.Width()) * surface->canvas.Height();
  if (wanted > kMaxCanvasPixels || PixelsHeld() - current + wanted > kMaxDocumentPixels) {
    // Refused, and the canvas keeps the size it had. The specification's answer for a size the
    // implementation cannot support, and the alternative is a page allocating the machine's memory one
    // assignment at a time.
    AddPerformanceCounter(PerfCounterId::CanvasSizeRefusals);
    return;
  }
  surface->canvas.Resize(clamped_width, clamped_height);
  // **Resizing resets everything**, which is the specification's rule and a load-bearing one: pages use
  // `canvas.width = canvas.width` as the idiomatic clear, and a resize that kept the state would make
  // that line a no-op that looks like a clear. It is literally `reset()`, which is why they share a
  // function: the specification defines the width setter as running the reset algorithm.
  ResetState(*surface);
  // The taint is *not* cleared. A resize does not un-see the cross-origin pixels that were drawn, and
  // clearing it here would be a one-line bypass of the whole check.
}

void CanvasSurfaces::ResetState(Surface& surface) {
  surface.state = State{};
  surface.state.clip = surface.canvas.Bounds();
  surface.stack.clear();
  surface.path = gfx::Path{};
  surface.dirty = true;
  surface.snapshot.reset();
  // The pixels too: `reset()` clears the bitmap to transparent black. Written rather than filled, for
  // the reason `clearRect` is -- every fill in `gfx` blends, and blending transparent black changes
  // nothing.
  for (int row = 0; row < surface.canvas.Height(); ++row) {
    if (std::uint32_t* pixels = surface.canvas.Row(row)) {
      std::fill(pixels, pixels + surface.canvas.Width(), 0u);
    }
  }
}

namespace {

// A `gfx::Color` as the CSS Color serialization a canvas getter must return.
//
// The specification is exact and pages compare against it: opaque is six lowercase hex digits, and
// anything else is `rgba(r, g, b, a)` with the alpha as a shortest-round-tripping decimal. Not
// `gfx::ColorText`'s job, because that one serializes for CSS output where the rules differ.
std::string SerializeCanvasColor(gfx::Color color) {
  static constexpr char kHex[] = "0123456789abcdef";
  if (color.Alpha() == 255) {
    std::string out = "#";
    for (const std::uint8_t component : {color.Red(), color.Green(), color.Blue()}) {
      out.push_back(kHex[component >> 4]);
      out.push_back(kHex[component & 0x0Fu]);
    }
    return out;
  }
  // The alpha to at most three decimals with trailing zeroes trimmed, which is what
  // `2d.fillStyle.get` and every `save`/`restore` test compares against. Built by hand rather than
  // with a stream, because a locale with a comma for a decimal separator would produce
  // `rgba(0, 0, 0, 0,5)` and no test would say which one was wrong.
  const int thousandths = static_cast<int>(std::lround(static_cast<double>(color.Alpha()) / 255.0 * 1000.0));
  std::string alpha;
  if (thousandths % 1000 == 0) {
    alpha = std::to_string(thousandths / 1000);
  } else {
    std::string fraction = std::to_string(thousandths % 1000);
    fraction.insert(0, static_cast<std::size_t>(3 - fraction.size()), '0');
    while (!fraction.empty() && fraction.back() == '0') {
      fraction.pop_back();
    }
    alpha = std::to_string(thousandths / 1000) + "." + fraction;
  }
  return "rgba(" + std::to_string(color.Red()) + ", " + std::to_string(color.Green()) + ", " +
         std::to_string(color.Blue()) + ", " + alpha + ")";
}

}  // namespace

std::string CanvasSurfaces::StateText(const dom::Element& element,
                                      bindings::CanvasOp::Kind which) const {
  const Surface* surface = Find(element);
  const State state = surface == nullptr ? State{} : surface->state;
  using Kind = bindings::CanvasOp::Kind;
  switch (which) {
    case Kind::SetFillColor:
      return SerializeCanvasColor(state.fill);
    case Kind::SetStrokeColor:
      return SerializeCanvasColor(state.stroke);
    case Kind::SetLineCap:
      return state.line.cap == gfx::LineCap::Round    ? "round"
             : state.line.cap == gfx::LineCap::Square ? "square"
                                                      : "butt";
    case Kind::SetLineJoin:
      return state.line.join == gfx::LineJoin::Round   ? "round"
             : state.line.join == gfx::LineJoin::Bevel ? "bevel"
                                                       : "miter";
    case Kind::SetFont:
      return state.font;
    case Kind::SetGlobalCompositeOperation:
      return std::string(CompositeOpName(state.composite));
    case Kind::SetTextAlign:
      switch (state.align) {
        case State::Align::End:
          return "end";
        case State::Align::Left:
          return "left";
        case State::Align::Right:
          return "right";
        case State::Align::Center:
          return "center";
        case State::Align::Start:
          break;
      }
      return "start";
    case Kind::SetTextBaseline:
      switch (state.baseline) {
        case State::Baseline::Top:
          return "top";
        case State::Baseline::Middle:
          return "middle";
        case State::Baseline::Bottom:
          return "bottom";
        case State::Baseline::Hanging:
          return "hanging";
        case State::Baseline::Ideographic:
          return "ideographic";
        case State::Baseline::Alphabetic:
          break;
      }
      return "alphabetic";
    default:
      break;
  }
  return {};
}

double CanvasSurfaces::StateNumber(const dom::Element& element,
                                   bindings::CanvasOp::Kind which) const {
  const Surface* surface = Find(element);
  const State state = surface == nullptr ? State{} : surface->state;
  using Kind = bindings::CanvasOp::Kind;
  switch (which) {
    case Kind::SetLineWidth:
      return static_cast<double>(state.line.width);
    case Kind::SetMiterLimit:
      return static_cast<double>(state.line.miter_limit);
    case Kind::SetGlobalAlpha:
      return static_cast<double>(state.alpha);
    case Kind::SetFillPaint:
      return static_cast<double>(state.fill_paint);
    case Kind::SetStrokePaint:
      return static_cast<double>(state.stroke_paint);
    default:
      break;
  }
  return 0.0;
}

std::vector<double> CanvasSurfaces::Transform(const dom::Element& element) const {
  const Surface* surface = Find(element);
  const gfx::AffineTransform matrix =
      surface == nullptr ? gfx::AffineTransform{} : surface->state.transform;
  return {static_cast<double>(matrix.A()), static_cast<double>(matrix.B()),
          static_cast<double>(matrix.C()), static_cast<double>(matrix.D()),
          static_cast<double>(matrix.E()), static_cast<double>(matrix.F())};
}

void CanvasSurfaces::Execute(dom::Element& element, const bindings::CanvasOp& op) {
  Surface* surface = For(element);
  if (surface == nullptr || surface->canvas.IsEmpty()) {
    return;
  }
  using Kind = bindings::CanvasOp::Kind;
  State& state = surface->state;
  gfx::Painter painter(surface->canvas);
  // The painter draws in canvas space and the transform is applied to *points* rather than handed to
  // it, because `gfx::Painter::SetTransform` transforms the path and the stroke width together and the
  // specification wants the stroke width in user space. Applying it here keeps the two separable.
  painter.SetTransform(gfx::AffineTransform{});
  surface->canvas.PushClip(state.clip);

  const auto fill_color = [&state]() {
    // `globalAlpha` multiplied into the colour, which is what it amounts to for a solid fill. It is not
    // the same as a real compositing pass -- overlapping shapes in one path would double-count -- and
    // that is recorded rather than hidden: this browser has no layer to composite through.
    const auto scaled = static_cast<std::uint8_t>(std::lround(std::clamp(
        static_cast<double>(state.fill.Alpha()) * static_cast<double>(state.alpha), 0.0, 255.0)));
    return gfx::Color::Rgba(state.fill.Red(), state.fill.Green(), state.fill.Blue(), scaled);
  };
  const auto stroke_color = [&state]() {
    const auto scaled = static_cast<std::uint8_t>(std::lround(std::clamp(
        static_cast<double>(state.stroke.Alpha()) * static_cast<double>(state.alpha), 0.0, 255.0)));
    return gfx::Color::Rgba(state.stroke.Red(), state.stroke.Green(), state.stroke.Blue(), scaled);
  };
  bool drew = false;
  // The selected paint source, or null for a plain colour. Looked up once per command rather than at
  // each of the five places that draw, and handed the transform in force *now* -- the specification
  // says a gradient's coordinates are in the space at the time of filling, so a page that translates
  // between `createLinearGradient` and `fill` gets the gradient where it drew, not where it built it.
  const auto paint_for = [&](std::uint32_t handle) -> const gfx::Paint* {
    if (handle == 0) {
      return nullptr;
    }
    const auto found = surface->paints.find(handle);
    if (found == surface->paints.end()) {
      return nullptr;
    }
    found->second.SetTransform(state.transform);
    return &found->second;
  };
  // `source-over` is every page's operator and stays the span fill it was; the other eleven go
  // through the compositor, which is a different loop -- it visits pixels the shape never covered,
  // because six of them erase exactly those.
  const auto composite_shape = [&](const gfx::Path& path, gfx::FillRule rule,
                                   const gfx::Paint* paint, gfx::Color colour) {
    const gfx::IntRect clip = state.clip.Intersected(surface->canvas.Bounds());
    gfx::PathRasterizer rasterizer;
    const std::vector<gfx::CoverageSpan> spans =
        rasterizer.Rasterize(path, rule, clip, gfx::AffineTransform{});
    const std::function<gfx::Color(int, int)> source =
        paint == nullptr
            ? std::function<gfx::Color(int, int)>([colour](int, int) { return colour; })
            : std::function<gfx::Color(int, int)>([paint](int x, int y) {
                return paint->At(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
              });
    CompositeShape(surface->canvas, clip, spans, source, state.alpha, state.composite);
  };
  const auto fill_path = [&](const gfx::Path& path, gfx::FillRule rule) {
    const gfx::Paint* paint = paint_for(state.fill_paint);
    if (state.composite != CompositeOp::SourceOver) {
      // The colour goes in without `globalAlpha` folded into it, because the compositor folds it in
      // itself -- doing both would square it.
      composite_shape(path, rule, paint, state.fill);
      return;
    }
    if (paint != nullptr) {
      painter.FillPath(path, *paint, state.alpha, rule);
    } else {
      painter.FillPath(path, fill_color(), rule);
    }
  };
  const auto stroke_path = [&](const gfx::Path& path) {
    const gfx::Paint* paint = paint_for(state.stroke_paint);
    if (state.composite != CompositeOp::SourceOver) {
      // Stroked to an outline first, which is what `StrokePath` does anyway: the compositor takes a
      // shape, and a stroke is a shape.
      const float scale = std::max(state.transform.MaximumScale(), 1e-4f);
      gfx::Path outline;
      gfx::StrokeToPath(path, state.line, gfx::kFlattenTolerance / scale, outline);
      composite_shape(outline, gfx::FillRule::NonZero, paint, state.stroke);
      return;
    }
    if (paint != nullptr) {
      painter.StrokePath(path, state.line, *paint, state.alpha);
    } else {
      painter.StrokePath(path, state.line, stroke_color());
    }
  };

  switch (op.kind) {
    case Kind::Save:
      // A bound, because `save()` in a loop is one line. A thousand nested states is far past any real
      // drawing and the refusal is silent, which is what the specification says for an unbalanced stack.
      if (surface->stack.size() < 1000) {
        surface->stack.push_back(state);
      }
      break;
    case Kind::Restore:
      if (!surface->stack.empty()) {
        state = surface->stack.back();
        surface->stack.pop_back();
      }
      break;
    case Kind::SetFillColor:
      // A colour that parses *deselects* the gradient. Both live in the state and only one of them
      // is the fill, so leaving the handle set would make `ctx.fillStyle = 'red'` a no-op for any
      // page that had ever assigned a gradient.
      if (ParseCanvasColor(op.text, state.fill)) {
        state.fill_paint = 0;
      }
      break;
    case Kind::SetStrokeColor:
      if (ParseCanvasColor(op.text, state.stroke)) {
        state.stroke_paint = 0;
      }
      break;
    case Kind::SetLineWidth:
      // Zero and negative are *ignored*, per the specification -- not clamped to a hairline. A page
      // that computed a zero width meant nothing visible, and drawing a one-pixel line instead is a
      // rendering nobody asked for.
      if (op.a > 0.0 && std::isfinite(op.a)) {
        state.line.width = static_cast<float>(op.a);
      }
      break;
    case Kind::SetLineCap:
      state.line.cap = op.text == "round"  ? gfx::LineCap::Round
                       : op.text == "square" ? gfx::LineCap::Square
                                             : gfx::LineCap::Butt;
      break;
    case Kind::SetLineJoin:
      state.line.join = op.text == "round" ? gfx::LineJoin::Round
                        : op.text == "bevel" ? gfx::LineJoin::Bevel
                                             : gfx::LineJoin::Miter;
      break;
    case Kind::SetMiterLimit:
      if (op.a > 0.0 && std::isfinite(op.a)) {
        state.line.miter_limit = static_cast<float>(op.a);
      }
      break;
    case Kind::SetGlobalAlpha:
      if (op.a >= 0.0 && op.a <= 1.0) {
        state.alpha = static_cast<float>(op.a);
      }
      break;
    case Kind::SetFont:
      state.font = op.text;
      break;
    case Kind::SetTextAlign:
      state.align = op.text == "end"      ? State::Align::End
                    : op.text == "left"   ? State::Align::Left
                    : op.text == "right"  ? State::Align::Right
                    : op.text == "center" ? State::Align::Center
                                          : State::Align::Start;
      break;
    case Kind::SetTextBaseline:
      state.baseline = op.text == "top"        ? State::Baseline::Top
                       : op.text == "middle"   ? State::Baseline::Middle
                       : op.text == "bottom"   ? State::Baseline::Bottom
                       : op.text == "hanging"  ? State::Baseline::Hanging
                       : op.text == "ideographic" ? State::Baseline::Ideographic
                                                 : State::Baseline::Alphabetic;
      break;
    case Kind::Transform: {
      if (!Finite(op.a, op.b, op.c, op.d) || !Finite(op.e, op.f)) {
        break;  // a non-finite matrix is ignored, which keeps a NaN out of every later point
      }
      const gfx::AffineTransform incoming(static_cast<float>(op.a), static_cast<float>(op.b),
                                          static_cast<float>(op.c), static_cast<float>(op.d),
                                          static_cast<float>(op.e), static_cast<float>(op.f));
      // `incoming.Then(current)` rather than the other way round: the canvas API's `transform()`
      // *pre*-multiplies, so a `translate` then a `rotate` rotates about the translated origin -- which
      // is what every page drawing a rotated sprite depends on.
      state.transform = incoming.Then(state.transform);
      break;
    }
    case Kind::SetTransform:
      if (Finite(op.a, op.b, op.c, op.d) && Finite(op.e, op.f)) {
        state.transform = gfx::AffineTransform(static_cast<float>(op.a), static_cast<float>(op.b),
                                               static_cast<float>(op.c), static_cast<float>(op.d),
                                               static_cast<float>(op.e), static_cast<float>(op.f));
      }
      break;
    case Kind::ResetTransform:
      state.transform = gfx::AffineTransform{};
      break;
    case Kind::BeginPath:
      surface->path = gfx::Path{};
      break;
    case Kind::MoveTo:
      if (Finite(op.a, op.b)) {
        surface->path.MoveTo(Apply(state.transform, op.a, op.b));
        state.pen_x = op.a;
        state.pen_y = op.b;
      }
      break;
    case Kind::LineTo:
      if (Finite(op.a, op.b)) {
        // `lineTo` with no current point is a `moveTo`, which is the specification's rule and is why
        // this asks the path rather than tracking a flag of its own.
        if (surface->path.IsEmpty()) {
          surface->path.MoveTo(Apply(state.transform, op.a, op.b));
        } else {
          surface->path.LineTo(Apply(state.transform, op.a, op.b));
        }
        state.pen_x = op.a;
        state.pen_y = op.b;
      }
      break;
    case Kind::QuadraticCurveTo:
      if (Finite(op.a, op.b, op.c, op.d) && !surface->path.IsEmpty()) {
        surface->path.QuadTo(Apply(state.transform, op.a, op.b),
                             Apply(state.transform, op.c, op.d));
        state.pen_x = op.c;
        state.pen_y = op.d;
      }
      break;
    case Kind::BezierCurveTo:
      if (Finite(op.a, op.b, op.c, op.d) && Finite(op.e, op.f) && !surface->path.IsEmpty()) {
        surface->path.CubicTo(Apply(state.transform, op.a, op.b), Apply(state.transform, op.c, op.d),
                              Apply(state.transform, op.e, op.f));
        state.pen_x = op.e;
        state.pen_y = op.f;
      }
      break;
    case Kind::Arc: {
      // Negative radius is an `IndexSizeError`, thrown by the caller; here it is simply refused, so
      // that a command that got through cannot draw a mirrored arc.
      if (op.c < 0.0 || op.f < 0.0) {
        break;
      }
      bool have_current = !surface->path.IsEmpty();
      // `ellipse()` is this command with a second radius in `f` and a rotation in `g`; `arc()` sends
      // the same radius twice. One arc construction rather than two, because two would be two places
      // for the counter-clockwise sweep rule to be wrong.
      AppendEllipseArc(surface->path, state.transform, op.a, op.b, op.c, op.f, op.g, op.d, op.e,
                       op.flag, have_current);
      // The pen ends where the arc ends, which is what a following `lineTo` starts from.
      state.pen_x = op.a + op.c * std::cos(op.e);
      state.pen_y = op.b + op.f * std::sin(op.e);
      break;
    }
    case Kind::ArcTo:
      // The real tangent construction now (`engine::AppendArcTo`). It used to be a `lineTo`, on the
      // argument that a close-but-wrong curve is worse than a straight line; that argument was right
      // about the approximation and wrong about the alternative, which is to do the construction.
      // It needs the *user-space* pen position, which is why the op carries it: the path holds
      // device-space points and inverting the transform to recover one is a division by a determinant
      // a page can make zero.
      if (surface->path.IsEmpty()) {
        // No subpath: the specification says the first control point is added as a `moveTo`.
        if (Finite(op.a, op.b)) {
          surface->path.MoveTo(Apply(state.transform, op.a, op.b));
          state.pen_x = op.a;
          state.pen_y = op.b;
        }
        break;
      }
      AppendArcTo(surface->path, state.transform, state.pen_x, state.pen_y, op.a, op.b, op.c, op.d,
                  op.e);
      state.pen_x = op.c;
      state.pen_y = op.d;
      break;
    case Kind::RoundRect:
      AppendRoundRect(surface->path, state.transform, op.a, op.b, op.c, op.d, op.numbers);
      state.pen_x = op.a;
      state.pen_y = op.b;
      break;
    case Kind::Reset:
      ResetState(*surface);
      drew = true;
      break;
    case Kind::Rect:
      if (Finite(op.a, op.b, op.c, op.d)) {
        // Four transformed corners rather than `AddRect`, because a rotated `rect()` is a rotated
        // quadrilateral and `AddRect` would produce an axis-aligned one.
        surface->path.MoveTo(Apply(state.transform, op.a, op.b));
        surface->path.LineTo(Apply(state.transform, op.a + op.c, op.b));
        surface->path.LineTo(Apply(state.transform, op.a + op.c, op.b + op.d));
        surface->path.LineTo(Apply(state.transform, op.a, op.b + op.d));
        surface->path.Close();
        state.pen_x = op.a;
        state.pen_y = op.b;
      }
      break;
    case Kind::ClosePath:
      if (!surface->path.IsEmpty()) {
        surface->path.Close();
      }
      break;
    case Kind::Fill:
      fill_path(surface->path, op.flag ? gfx::FillRule::EvenOdd : gfx::FillRule::NonZero);
      drew = true;
      break;
    case Kind::Stroke:
      stroke_path(surface->path);
      drew = true;
      break;
    case Kind::Clip: {
      // The bounding box of the path, intersected with the current clip. See the note on `State::clip`:
      // this clips *less* than asked, never more, so nothing is hidden that should be visible.
      const gfx::FloatRect bounds = surface->path.ControlBounds();
      const gfx::IntRect box{static_cast<int>(std::floor(bounds.x)),
                             static_cast<int>(std::floor(bounds.y)),
                             static_cast<int>(std::ceil(bounds.width)),
                             static_cast<int>(std::ceil(bounds.height))};
      state.clip = state.clip.Intersected(box);
      break;
    }
    case Kind::FillRect:
    case Kind::StrokeRect:
    case Kind::ClearRect: {
      if (!Finite(op.a, op.b, op.c, op.d)) {
        break;
      }
      // Its own path rather than the current one: the specification says these ignore the current path
      // entirely, so a `fillRect` between a `moveTo` and a `fill` must not join it.
      gfx::Path rect;
      rect.MoveTo(Apply(state.transform, op.a, op.b));
      rect.LineTo(Apply(state.transform, op.a + op.c, op.b));
      rect.LineTo(Apply(state.transform, op.a + op.c, op.b + op.d));
      rect.LineTo(Apply(state.transform, op.a, op.b + op.d));
      rect.Close();
      if (op.kind == Kind::FillRect) {
        fill_path(rect, gfx::FillRule::NonZero);
      } else if (op.kind == Kind::StrokeRect) {
        stroke_path(rect);
      } else {
        // `clearRect` writes transparent black, which is a *replacement* and not a blend -- so it goes
        // through the canvas rather than the painter. A painter fill of transparent black would blend
        // nothing and leave the pixels alone, which is the bug this comment exists to prevent.
        const gfx::FloatRect bounds = rect.ControlBounds();
        // **Written directly, not filled.** `clearRect` sets pixels to transparent black, which is a
        // *replacement*; every fill path in `gfx` blends, and `Canvas::FillRect` goes further and
        // returns immediately for a fully transparent colour -- so a `clearRect` written as a fill is
        // a `clearRect` that silently does nothing. Which is exactly what the first version did, and
        // the probe that found it read 16,16,16,255 out of a region the page had just cleared.
        const gfx::IntRect box =
            gfx::IntRect{static_cast<int>(std::floor(bounds.x)), static_cast<int>(std::floor(bounds.y)),
                         static_cast<int>(std::ceil(bounds.width)),
                         static_cast<int>(std::ceil(bounds.height))}
                .Intersected(state.clip)
                .Intersected(surface->canvas.Bounds());
        for (int row = box.Top(); row < box.Bottom(); ++row) {
          std::uint32_t* pixels = surface->canvas.Row(row);
          if (pixels != nullptr) {
            std::fill(pixels + box.Left(), pixels + box.Right(), 0u);
          }
        }
      }
      drew = true;
      break;
    }
    case Kind::FillText:
    case Kind::StrokeText: {
      if (!Finite(op.a, op.b) || text_ == nullptr) {
        break;
      }
      // The font, through the CSS shorthand parser -- so `ctx.font = 'bold 24px serif'` means what it
      // means in a stylesheet. One parser, for the reason the colour has one.
      css::ComputedStyle style;
      css::ApplyDeclaration(css::Declaration{"font", state.font, false}, css::ComputedStyle{}, style);
      const gfx::FontRequest request = layout::FontRequestFor(style);
      const double width = text_->MeasureRun(op.text, request);
      double x = op.a;
      switch (state.align) {
        case State::Align::End:
        case State::Align::Right:
          x -= width;
          break;
        case State::Align::Center:
          x -= width * 0.5;
          break;
        case State::Align::Start:
        case State::Align::Left:
          break;
      }
      const gfx::FontMetrics metrics = text_->MetricsFor(request);
      double y = op.b;
      switch (state.baseline) {
        case State::Baseline::Top:
        case State::Baseline::Hanging:
          y += static_cast<double>(metrics.ascent);
          break;
        case State::Baseline::Middle:
          y += static_cast<double>(metrics.ascent) * 0.5;
          break;
        case State::Baseline::Bottom:
        case State::Baseline::Ideographic:
          y -= static_cast<double>(metrics.descent);
          break;
        case State::Baseline::Alphabetic:
          break;
      }
      // Painted at the transformed origin. The glyphs themselves are *not* transformed -- a rotated
      // `fillText` draws upright text at a rotated position -- which is a stated approximation: text
      // under a transform needs the shaper's outlines transformed, and `gfx::TextRenderer` draws
      // rasterised glyphs at a point.
      text_->DrawRun(painter, op.text, request, Apply(state.transform, x, y),
                     op.kind == Kind::FillText ? fill_color() : stroke_color());
      drew = true;
      break;
    }
    case Kind::SetGlobalCompositeOperation:
      if (const std::optional<CompositeOp> parsed = ParseCompositeOp(op.text)) {
        state.composite = *parsed;
      }
      break;
    case Kind::SetLineDash:
    case Kind::SetLineDashOffset:
    case Kind::SetShadowColor:
    case Kind::SetShadowBlur:
    case Kind::SetShadowOffsetX:
    case Kind::SetShadowOffsetY:
    case Kind::SetImageSmoothing:
    case Kind::SetDirection:
      // Reserved, and deliberately not stored. A dash array or a composite operator kept here and
      // ignored by the painter would make `ctx.getLineDash()` answer about a dash nothing draws --
      // which is the stub ADR 0012 forbids, wearing the shape of state rather than of a method. The
      // binding layer does not install these names, so a page feature-detects an absence.
      break;
    case Kind::CreateLinearGradient:
      surface->paints.insert_or_assign(
          op.handle, gfx::Paint::Linear(static_cast<float>(op.a), static_cast<float>(op.b),
                                        static_cast<float>(op.c), static_cast<float>(op.d)));
      break;
    case Kind::CreateRadialGradient:
      surface->paints.insert_or_assign(
          op.handle, gfx::Paint::Radial(static_cast<float>(op.a), static_cast<float>(op.b),
                                        static_cast<float>(op.c), static_cast<float>(op.d),
                                        static_cast<float>(op.e), static_cast<float>(op.f)));
      break;
    case Kind::CreateConicGradient:
      surface->paints.insert_or_assign(
          op.handle, gfx::Paint::Conic(static_cast<float>(op.a), static_cast<float>(op.b),
                                       static_cast<float>(op.c)));
      break;
    case Kind::AddColorStop: {
      const auto found = surface->paints.find(op.handle);
      gfx::Color stop;
      // An unparseable colour is a `SyntaxError` thrown by the caller, so reaching here with one
      // means the caller let it through; refusing is better than adding black.
      if (found != surface->paints.end() && ParseCanvasColor(op.text, stop)) {
        found->second.AddStop(static_cast<float>(op.a), stop);
      }
      break;
    }
    case Kind::SetFillPaint:
      state.fill_paint = op.handle;
      break;
    case Kind::SetStrokePaint:
      state.stroke_paint = op.handle;
      break;
    case Kind::CreatePattern:
    case Kind::DrawImage:
      // Not here: these two need pixels, and `src/bindings` cannot hold a `gfx::Image`. `Page`
      // resolves the source element and calls `DrawImage`/`SetPattern` below. Reaching this case
      // means the caller had no image, which is a no-op rather than a failure -- an `<img>` that has
      // not loaded draws nothing, per the specification.
      break;
  }
  surface->canvas.PopClip();
  if (drew) {
    surface->dirty = true;
    surface->snapshot.reset();
    AddPerformanceCounter(PerfCounterId::CanvasDraws);
  }
}

void CanvasSurfaces::DrawImage(dom::Element& element, const bindings::CanvasOp& op,
                               const std::shared_ptr<const gfx::Image>& image, bool taints) {
  Surface* surface = For(element);
  if (surface == nullptr || surface->canvas.IsEmpty() || image == nullptr || !image->IsValid()) {
    return;
  }
  if (taints) {
    // Set before the draw, so a refusal further down cannot leave a canvas readable that has seen
    // cross-origin pixels.
    surface->tainted = true;
    AddPerformanceCounter(PerfCounterId::CanvasesTainted);
  }
  // `a`..`d` are the source rectangle and `e`..`h` the destination. The three-argument and
  // five-argument forms arrive with the source rectangle as the whole image, filled in by the
  // binding layer's own natural-size query rather than guessed here.
  const double sx = op.a;
  const double sy = op.b;
  const double sw = op.c;
  const double sh = op.d;
  const double dx = op.e;
  const double dy = op.f;
  const double dw = op.g;
  const double dh = op.h;
  if (!Finite(sx, sy, sw, sh) || !Finite(dx, dy, dw, dh) || sw == 0.0 || sh == 0.0 ||
      dw == 0.0 || dh == 0.0) {
    return;
  }
  State& state = surface->state;
  gfx::Painter painter(surface->canvas);
  painter.SetTransform(gfx::AffineTransform{});
  surface->canvas.PushClip(state.clip);
  // The image is painted as a *pattern* clipped to the destination rectangle, which is how one
  // machine covers every case: the pattern already knows how to sample through an inverse transform,
  // so a rotated or scaled `drawImage` costs nothing extra and cannot disagree with `createPattern`.
  gfx::Paint paint = gfx::Paint::Pattern(image, gfx::Paint::Repeat::None);
  // The source rectangle becomes part of the pattern's transform: scale the destination onto the
  // source and translate so that (sx, sy) lands on (dx, dy).
  const auto scale_x = static_cast<float>(dw / sw);
  const auto scale_y = static_cast<float>(dh / sh);
  const gfx::AffineTransform placement =
      gfx::AffineTransform(scale_x, 0.0f, 0.0f, scale_y,
                           static_cast<float>(dx) - static_cast<float>(sx) * scale_x,
                           static_cast<float>(dy) - static_cast<float>(sy) * scale_y);
  paint.SetTransform(placement.Then(state.transform));
  gfx::Path rect;
  rect.MoveTo(Apply(state.transform, dx, dy));
  rect.LineTo(Apply(state.transform, dx + dw, dy));
  rect.LineTo(Apply(state.transform, dx + dw, dy + dh));
  rect.LineTo(Apply(state.transform, dx, dy + dh));
  rect.Close();
  if (state.composite == CompositeOp::SourceOver) {
    painter.FillPath(rect, paint, state.alpha, gfx::FillRule::NonZero);
  } else {
    const gfx::IntRect clip = state.clip.Intersected(surface->canvas.Bounds());
    gfx::PathRasterizer rasterizer;
    CompositeShape(surface->canvas, clip,
                   rasterizer.Rasterize(rect, gfx::FillRule::NonZero, clip, gfx::AffineTransform{}),
                   [&paint](int x, int y) {
                     return paint.At(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                   },
                   state.alpha, state.composite);
  }
  surface->canvas.PopClip();
  surface->dirty = true;
  surface->snapshot.reset();
  AddPerformanceCounter(PerfCounterId::CanvasDraws);
}

void CanvasSurfaces::SetPattern(dom::Element& element, const bindings::CanvasOp& op,
                                const std::shared_ptr<const gfx::Image>& image, bool taints) {
  Surface* surface = For(element);
  if (surface == nullptr || image == nullptr || !image->IsValid()) {
    return;
  }
  if (taints) {
    surface->tainted = true;
    AddPerformanceCounter(PerfCounterId::CanvasesTainted);
  }
  const gfx::Paint::Repeat repeat = op.text == "repeat-x"  ? gfx::Paint::Repeat::X
                                    : op.text == "repeat-y" ? gfx::Paint::Repeat::Y
                                    : op.text == "no-repeat" ? gfx::Paint::Repeat::None
                                                             : gfx::Paint::Repeat::Both;
  surface->paints.insert_or_assign(op.handle, gfx::Paint::Pattern(image, repeat));
}

std::shared_ptr<const gfx::Image> CanvasSurfaces::Snapshot(const dom::Element& element) {
  Surface* surface = For(element);
  if (surface == nullptr || surface->canvas.IsEmpty()) {
    return nullptr;
  }
  if (surface->snapshot != nullptr) {
    return surface->snapshot;  // nothing drawn since the last frame, so no copy
  }
  // Built from the canvas's pixels through the image's own constructor rather than by writing rows,
  // because `gfx::Image` exposes its pixels read-only -- which is right: an image is a decoded resource
  // and something that could be written into after the fact would be a second way to change one.
  std::vector<std::uint32_t> pixels(
      static_cast<std::size_t>(surface->canvas.Width()) *
      static_cast<std::size_t>(surface->canvas.Height()), 0);
  for (int y = 0; y < surface->canvas.Height(); ++y) {
    const std::uint32_t* row = surface->canvas.Row(y);
    if (row == nullptr) {
      continue;
    }
    // Un-premultiplied on the way out, because a `gfx::Image` is what a decoder produces and those
    // are not premultiplied -- so a canvas snapshot that kept the backing store's form would be the
    // one image in the process that meant something different from all the others.
    std::transform(row, row + surface->canvas.Width(),
                   pixels.begin() + static_cast<std::ptrdiff_t>(y) * surface->canvas.Width(),
                   Unpremultiply);
  }
  auto image = std::make_shared<gfx::Image>();
  if (!image->Adopt(surface->canvas.Width(), surface->canvas.Height(), std::move(pixels))) {
    return nullptr;
  }
  surface->snapshot = image;
  surface->dirty = false;
  return surface->snapshot;
}

std::vector<std::uint8_t> CanvasSurfaces::ReadPixels(const dom::Element& element, int x, int y,
                                                     int width, int height) const {
  const Surface* surface = Find(element);
  if (surface == nullptr || surface->tainted || width <= 0 || height <= 0) {
    // A tainted canvas reads as nothing, and the caller turns that into a `SecurityError`. Checked
    // here rather than at the call site so that every path -- `getImageData`, `toDataURL`, whatever
    // comes next -- goes through one refusal.
    return {};
  }
  const std::int64_t needed = static_cast<std::int64_t>(width) * height * 4;
  if (needed > kMaxCanvasPixels * 4) {
    return {};
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(needed), 0);
  for (int row = 0; row < height; ++row) {
    const int source_y = y + row;
    if (source_y < 0 || source_y >= surface->canvas.Height()) {
      continue;  // outside the canvas reads as transparent black, per the specification
    }
    const std::uint32_t* pixels = surface->canvas.Row(source_y);
    if (pixels == nullptr) {
      continue;
    }
    for (int column = 0; column < width; ++column) {
      const int source_x = x + column;
      if (source_x < 0 || source_x >= surface->canvas.Width()) {
        continue;
      }
      const std::uint32_t argb = Unpremultiply(pixels[source_x]);
      const std::size_t at =
          (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(column)) * 4;
      out[at + 0] = static_cast<std::uint8_t>((argb >> 16) & 0xFFu);
      out[at + 1] = static_cast<std::uint8_t>((argb >> 8) & 0xFFu);
      out[at + 2] = static_cast<std::uint8_t>(argb & 0xFFu);
      out[at + 3] = static_cast<std::uint8_t>((argb >> 24) & 0xFFu);
    }
  }
  AddPerformanceCounter(PerfCounterId::CanvasReadbacks);
  return out;
}

void CanvasSurfaces::WritePixels(dom::Element& element, int x, int y, int width, int height,
                                 const std::vector<std::uint8_t>& rgba) {
  Surface* surface = For(element);
  if (surface == nullptr || width <= 0 || height <= 0) {
    return;
  }
  const std::size_t needed =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  if (rgba.size() < needed) {
    return;  // a short buffer is refused rather than partially applied
  }
  for (int row = 0; row < height; ++row) {
    const int target_y = y + row;
    if (target_y < 0 || target_y >= surface->canvas.Height()) {
      continue;
    }
    std::uint32_t* pixels = surface->canvas.Row(target_y);
    if (pixels == nullptr) {
      continue;
    }
    for (int column = 0; column < width; ++column) {
      const int target_x = x + column;
      if (target_x < 0 || target_x >= surface->canvas.Width()) {
        continue;
      }
      const std::size_t at =
          (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(column)) * 4;
      // `putImageData` *replaces* rather than blends, which is the specification's rule and is what
      // makes it usable for a pixel filter: a blend would compose the filtered pixels over the ones
      // they were computed from.
      pixels[target_x] = Premultiply((static_cast<std::uint32_t>(rgba[at + 3]) << 24) |
                                     (static_cast<std::uint32_t>(rgba[at + 0]) << 16) |
                                     (static_cast<std::uint32_t>(rgba[at + 1]) << 8) |
                                     static_cast<std::uint32_t>(rgba[at + 2]));
    }
  }
  surface->dirty = true;
  surface->snapshot.reset();
}

double CanvasSurfaces::MeasureText(const dom::Element& element, const std::string& text) const {
  const Surface* surface = Find(element);
  if (surface == nullptr || text_ == nullptr) {
    return 0.0;
  }
  css::ComputedStyle style;
  css::ApplyDeclaration(css::Declaration{"font", surface->state.font, false}, css::ComputedStyle{},
                        style);
  return text_->MeasureRun(text, layout::FontRequestFor(style));
}

bool CanvasSurfaces::HitTest(const dom::Element& element, double x, double y, bool stroke,
                             bool even_odd) const {
  const Surface* surface = Find(element);
  if (surface == nullptr) {
    return false;
  }
  if (!stroke) {
    return PathContainsPoint(surface->path, x, y, even_odd);
  }
  // `isPointInStroke` is `isPointInPath` of the *stroked outline*, which is what the stroker already
  // produces: converting the stroke to a fill is how it is drawn, so asking the same question of the
  // same shape is the only construction that cannot disagree with what is on the screen.
  gfx::Path outline;
  gfx::StrokeToPath(surface->path, surface->state.line, outline);
  return PathContainsPoint(outline, x, y, false);
}

}  // namespace microbrowser::engine

#include "engine/CanvasSurfaces.h"

#include <algorithm>
#include <cmath>

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

// An arc, flattened into cubics.
//
// `gfx::Path` has no arc verb -- deliberately, per its own header: "exactly the set every 2D path
// format reduces to". So the conversion happens here, in the one place that needs it, rather than
// widening the path type for one caller. Each cubic covers at most a quarter turn, which is where the
// 0.5522847 magic constant comes from and is the standard bound for a quarter-arc's error.
void AppendArc(gfx::Path& path, const gfx::AffineTransform& transform, double cx, double cy,
               double radius, double start, double end, bool counter_clockwise, bool& have_current) {
  if (!(radius > 0.0) || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(start) ||
      !std::isfinite(end)) {
    return;
  }
  constexpr double kTwoPi = 6.283185307179586;
  double sweep = end - start;
  if (counter_clockwise) {
    // The specification's rule, and it is not just a sign: `arc(…, 0, 3π, true)` sweeps *backwards* by
    // more than a full turn, which draws a full circle. Normalising the sweep into (-2π, 0] and then
    // clamping to a full turn is what produces that.
    if (sweep > 0.0) {
      sweep = std::fmod(sweep, kTwoPi) - kTwoPi;
    }
    sweep = std::max(sweep, -kTwoPi);
  } else {
    if (sweep < 0.0) {
      sweep = std::fmod(sweep, kTwoPi) + kTwoPi;
    }
    sweep = std::min(sweep, kTwoPi);
  }
  const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (kTwoPi / 8.0))));
  const double step = sweep / segments;
  const gfx::FloatPoint first = Apply(transform, cx + radius * std::cos(start),
                                     cy + radius * std::sin(start));
  if (!have_current) {
    path.MoveTo(first);
    have_current = true;
  } else {
    // A `lineTo` to the arc's start, which is what the specification says when a subpath is already
    // open -- and is why `arc` after a `moveTo` elsewhere draws a connecting line rather than jumping.
    path.LineTo(first);
  }
  double angle = start;
  for (int i = 0; i < segments; ++i) {
    const double next = angle + step;
    // The tangent-length factor for a cubic approximation of a circular arc of this angle.
    const double alpha = std::sin(step) * (std::sqrt(4.0 + 3.0 * std::tan(step * 0.5) *
                                                              std::tan(step * 0.5)) -
                                           1.0) / 3.0;
    const double x0 = cx + radius * std::cos(angle);
    const double y0 = cy + radius * std::sin(angle);
    const double x1 = cx + radius * std::cos(next);
    const double y1 = cy + radius * std::sin(next);
    const gfx::FloatPoint control1 = Apply(transform, x0 - alpha * radius * std::sin(angle),
                                          y0 + alpha * radius * std::cos(angle));
    const gfx::FloatPoint control2 = Apply(transform, x1 + alpha * radius * std::sin(next),
                                          y1 - alpha * radius * std::cos(next));
    path.CubicTo(control1, control2, Apply(transform, x1, y1));
    angle = next;
  }
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
  // that line a no-op that looks like a clear.
  surface->state = State{};
  surface->state.clip = surface->canvas.Bounds();
  surface->stack.clear();
  surface->path = gfx::Path{};
  surface->dirty = true;
  surface->snapshot.reset();
  // The taint is *not* cleared. A resize does not un-see the cross-origin pixels that were drawn, and
  // clearing it here would be a one-line bypass of the whole check.
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
      (void)ParseCanvasColor(op.text, state.fill);
      break;
    case Kind::SetStrokeColor:
      (void)ParseCanvasColor(op.text, state.stroke);
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
      }
      break;
    case Kind::QuadraticCurveTo:
      if (Finite(op.a, op.b, op.c, op.d) && !surface->path.IsEmpty()) {
        surface->path.QuadTo(Apply(state.transform, op.a, op.b),
                             Apply(state.transform, op.c, op.d));
      }
      break;
    case Kind::BezierCurveTo:
      if (Finite(op.a, op.b, op.c, op.d) && Finite(op.e, op.f) && !surface->path.IsEmpty()) {
        surface->path.CubicTo(Apply(state.transform, op.a, op.b), Apply(state.transform, op.c, op.d),
                              Apply(state.transform, op.e, op.f));
      }
      break;
    case Kind::Arc: {
      bool have_current = !surface->path.IsEmpty();
      AppendArc(surface->path, state.transform, op.a, op.b, op.c, op.d, op.e, op.flag,
                have_current);
      break;
    }
    case Kind::ArcTo:
      // Deliberately absent rather than approximated. `arcTo` is a tangent construction whose result
      // depends on the current point, and one written as "a line then an arc" produces a shape that is
      // close and wrong -- which is worse than a straight line, because it looks intentional. A page
      // gets a `lineTo` to the second point, which is the specification's own answer for the degenerate
      // case and is visibly not a curve.
      if (Finite(op.c, op.d) && !surface->path.IsEmpty()) {
        surface->path.LineTo(Apply(state.transform, op.c, op.d));
      }
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
      }
      break;
    case Kind::ClosePath:
      if (!surface->path.IsEmpty()) {
        surface->path.Close();
      }
      break;
    case Kind::Fill:
      painter.FillPath(surface->path, fill_color(),
                       op.flag ? gfx::FillRule::EvenOdd : gfx::FillRule::NonZero);
      drew = true;
      break;
    case Kind::Stroke:
      painter.StrokePath(surface->path, state.line, stroke_color());
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
        painter.FillPath(rect, fill_color());
      } else if (op.kind == Kind::StrokeRect) {
        painter.StrokePath(rect, state.line, stroke_color());
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
    case Kind::DrawImage:
      // The image itself arrives through `WritePixels`/the caller, because `src/bindings` cannot hold a
      // `gfx::Image`. What reaches here is the *taint decision* the caller made: `flag` set means the
      // source was cross-origin without CORS, and once that is drawn the canvas can never be read
      // again. Set before the draw, so a throw in the draw cannot leave it unset.
      if (op.flag) {
        surface->tainted = true;
        AddPerformanceCounter(PerfCounterId::CanvasesTainted);
      }
      break;
  }
  surface->canvas.PopClip();
  if (drew) {
    surface->dirty = true;
    surface->snapshot.reset();
    AddPerformanceCounter(PerfCounterId::CanvasDraws);
  }
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
    std::copy(row, row + surface->canvas.Width(),
              pixels.begin() + static_cast<std::ptrdiff_t>(y) * surface->canvas.Width());
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
      const std::uint32_t argb = pixels[source_x];
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
      pixels[target_x] = (static_cast<std::uint32_t>(rgba[at + 3]) << 24) |
                         (static_cast<std::uint32_t>(rgba[at + 0]) << 16) |
                         (static_cast<std::uint32_t>(rgba[at + 1]) << 8) |
                         static_cast<std::uint32_t>(rgba[at + 2]);
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

}  // namespace microbrowser::engine

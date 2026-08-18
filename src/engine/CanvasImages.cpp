// `drawImage` and `createPattern`: the two canvas commands that need somebody else's pixels.
//
// ADR 0029 §2. Its own translation unit because it is the only part of canvas drawing whose input
// does not come from the page's script -- everything else is a number or a colour, and these two are
// a decoded image -- and because that is also where the security boundary is: a cross-origin source
// taints the canvas for the rest of its life, and the flag is set *before* the draw so that a
// refusal further down cannot leave a canvas readable that has seen pixels it may not show.
//
// `Page` decides the taint, because it is the object that knows what an `<img>` fetched and from
// what origin. What arrives here is that decision, already made.

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "engine/CanvasComposite.h"
#include "engine/CanvasSurfaces.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

gfx::FloatPoint Apply(const gfx::AffineTransform& transform, double x, double y) {
  return transform.MapPoint(gfx::FloatPoint{static_cast<float>(x), static_cast<float>(y)});
}

bool Finite(double a, double b, double c, double d) {
  return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
}

}  // namespace

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
  if (state.shadow_color.Alpha() != 0 &&
      (state.shadow_blur > 0.0f || state.shadow_offset_x != 0.0 || state.shadow_offset_y != 0.0)) {
    const gfx::IntRect shadow_clip = state.clip.Intersected(surface->canvas.Bounds());
    const int blur = static_cast<int>(std::ceil(state.shadow_blur)) + 1;
    const gfx::IntRect shadow_source =
        shadow_clip.Translated(-static_cast<int>(std::lround(state.shadow_offset_x)),
                               -static_cast<int>(std::lround(state.shadow_offset_y)))
            .Inflated(blur * 3);
    gfx::PathRasterizer shadow_rasterizer;
    // The image's *rectangle* rather than its alpha channel, which is a stated approximation: a PNG
    // with transparent corners casts a rectangular shadow here where a browser would cast the shape
    // of its opaque pixels. Doing it properly means rasterizing the image's alpha into the mask,
    // which is the same off-screen layer the grouped-shadow deviation in CanvasComposite.h wants.
    PaintShadow(surface->canvas, shadow_clip,
                shadow_rasterizer.Rasterize(rect, gfx::FillRule::NonZero, shadow_source,
                                            gfx::AffineTransform{}),
                state.shadow_offset_x, state.shadow_offset_y, state.shadow_blur * 0.5f,
                state.shadow_color, state.alpha, state.composite);
  }
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

}  // namespace microbrowser::engine

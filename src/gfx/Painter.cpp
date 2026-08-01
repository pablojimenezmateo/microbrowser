#include "gfx/Painter.h"

#include <algorithm>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

void Painter::FillPath(const Path& path, Color color, FillRule rule) {
  if (color.IsFullyTransparent()) {
    return;
  }
  const IntRect clip = canvas_->Clip().Intersected(canvas_->Bounds());
  if (clip.IsEmpty()) {
    return;
  }
  FillSpans(rasterizer_.Rasterize(path, rule, clip, transform_), color);
}

void Painter::FillRect(const FloatRect& rect, Color color) {
  if (rect.IsEmpty()) {
    return;
  }
  Path path;
  path.AddRect(rect);
  FillPath(path, color, FillRule::NonZero);
}

void Painter::StrokePath(const Path& path, const StrokeStyle& style, Color color) {
  if (color.IsFullyTransparent() || path.IsEmpty()) {
    return;
  }
  // The stroke is built in layout space and transformed on the way to pixels,
  // which is the only way a non-uniform scale produces the right shape: a
  // stroke transformed as an outline stays elliptical where a stroke widened
  // in device space would not. Its own flattening tolerance therefore has to be
  // tightened by the scale it is about to be multiplied by.
  const float scale = std::max(transform_.MaximumScale(), 1e-4f);
  StrokeToPath(path, style, kFlattenTolerance / scale, stroke_scratch_);
  // Always nonzero: the stroke is a union of overlapping pieces, and even-odd
  // would punch every overlap back out.
  FillPath(stroke_scratch_, color, FillRule::NonZero);
}

void Painter::StrokeLine(FloatPoint from, FloatPoint to, const StrokeStyle& style, Color color) {
  Path path;
  path.MoveTo(from);
  path.LineTo(to);
  StrokePath(path, style, color);
}

void Painter::FillSpans(const std::vector<CoverageSpan>& spans, Color color) {
  const std::uint32_t source_alpha = color.Alpha();
  for (const CoverageSpan& span : spans) {
    std::uint32_t* row = canvas_->Row(span.y);
    if (row == nullptr) {
      continue;
    }
    // The rasterizer clips, so this is a bug signal rather than a routine
    // case — but a wrong span must not become a heap write.
    if (span.x < 0 || span.length <= 0 || span.x + span.length > canvas_->Width()) {
      continue;
    }

    const Color blended =
        color.WithAlpha(static_cast<std::uint8_t>(MulDiv255(source_alpha, span.coverage)));
    if (blended.IsFullyTransparent()) {
      continue;
    }

    std::uint32_t* pixels = row + span.x;
    if (blended.IsOpaque()) {
      AddPerformanceCounter(PerfCounterId::GfxOpaqueFills);
      std::fill(pixels, pixels + span.length, blended.argb);
      continue;
    }
    AddPerformanceCounter(PerfCounterId::GfxBlendedFills);
    for (std::int32_t i = 0; i < span.length; ++i) {
      pixels[i] = BlendSrcOver(pixels[i], blended);
    }
  }
}

}  // namespace microbrowser::gfx

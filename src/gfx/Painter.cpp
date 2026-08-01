#include "gfx/Painter.h"

#include <algorithm>
#include <cstddef>

#include <cmath>

#include "gfx/Blitter.h"

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

void Painter::BlitGlyph(const GlyphImage& image, int x, int y, Color color) {
  const IntRect clip = canvas_->Clip().Intersected(canvas_->Bounds());
  // x and y are already clamped by the caller, so the corners of this rect
  // cannot overflow. Constructing it from unclamped values would put a signed
  // overflow one Intersected() away.
  const IntRect placed{x, y, image.width, image.height};
  const IntRect target = placed.Intersected(clip);
  if (target.IsEmpty()) {
    return;
  }

  for (int row = target.Top(); row < target.Bottom(); ++row) {
    std::uint32_t* destination = canvas_->Row(row);
    if (destination == nullptr) {
      continue;
    }
    const std::size_t mask_row = static_cast<std::size_t>(row - y) *
                                 static_cast<std::size_t>(image.width) +
                                 static_cast<std::size_t>(target.Left() - x);
    BlendMaskSrcOver(destination + target.Left(), image.coverage.data() + mask_row,
                     static_cast<std::size_t>(target.width), color);
  }
}

void Painter::DrawGlyphs(const Font& font, const ShapedRun& run, FloatPoint origin, Color color) {
  if (color.IsFullyTransparent() || run.glyphs.empty()) {
    return;
  }
  const AffineTransform saved = transform_;
  // A cached mask is pixels, so it is only reusable when the transform is a
  // pure translation. Under a scale or a rotation the glyph has to go back
  // through the rasterizer — correctness first, and a rotated line of text is
  // rare enough that the slow path is the right answer rather than a
  // per-transform cache.
  const bool translation_only = saved.A() == 1.0f && saved.B() == 0.0f && saved.C() == 0.0f &&
                                saved.D() == 1.0f;

  float pen_x = origin.x;
  float pen_y = origin.y;
  for (const PositionedGlyph& glyph : run.glyphs) {
    const float x = pen_x + glyph.x_offset;
    const float y = pen_y + glyph.y_offset;

    if (translation_only) {
      const float device_x = x + saved.E();
      const float device_y = y + saved.F();
      if (const GlyphImage* image = glyphs_.Acquire(font, glyph.glyph, device_x)) {
        // Clamped well inside the device range so that the placed rect's
        // corners stay representable however far off-surface the text is.
        constexpr float kPlacementLimit = 1.0f * 1048576.0f;
        const int left = SaturateFloatToInt(
            std::floor(std::clamp(device_x, -kPlacementLimit, kPlacementLimit)));
        const int top = SaturateFloatToInt(
            std::nearbyint(std::clamp(device_y, -kPlacementLimit, kPlacementLimit)));
        BlitGlyph(*image, left + image->origin.x, top + image->origin.y, color);
        AddPerformanceCounter(PerfCounterId::GfxGlyphsDrawn);
      }
    } else if (font.GlyphOutline(glyph.glyph, glyph_scratch_)) {
      // The outline comes back relative to the glyph origin, so the pen
      // position is applied as a transform rather than by rewriting the path.
      // That also composes with the caller's transform correctly: a rotated
      // line of text is the glyph transform *then* the painter's.
      transform_ = AffineTransform::Translation(x, y).Then(saved);
      FillPath(glyph_scratch_, color, FillRule::NonZero);
      AddPerformanceCounter(PerfCounterId::GfxGlyphsDrawn);
    }

    pen_x += glyph.x_advance;
    pen_y += glyph.y_advance;
  }
  transform_ = saved;
}

void Painter::DrawImage(const Image& image, IntPoint at) {
  if (!image.IsValid()) {
    return;
  }
  const IntRect clip = canvas_->Clip().Intersected(canvas_->Bounds());
  // Only the translation is honored, so it is folded in here rather than
  // pretending the rest of the transform was applied.
  const int left = at.x + SaturateFloatToInt(transform_.E());
  const int top = at.y + SaturateFloatToInt(transform_.F());
  const IntRect placed{std::clamp(left, -kMaxDeviceCoordinate / 2, kMaxDeviceCoordinate / 2),
                       std::clamp(top, -kMaxDeviceCoordinate / 2, kMaxDeviceCoordinate / 2),
                       image.Width(), image.Height()};
  const IntRect target = placed.Intersected(clip);
  if (target.IsEmpty()) {
    return;
  }

  AddPerformanceCounter(PerfCounterId::GfxImagesDrawn);
  for (int y = target.Top(); y < target.Bottom(); ++y) {
    std::uint32_t* destination = canvas_->Row(y);
    const std::uint32_t* source = image.Row(y - placed.y);
    if (destination == nullptr || source == nullptr) {
      continue;
    }
    destination += target.Left();
    source += target.Left() - placed.x;

    if (image.IsOpaque()) {
      // An opaque image is a copy. Knowing that once at decode time is worth
      // about as much as the vector blitter was.
      std::copy(source, source + target.width, destination);
      continue;
    }
    for (int i = 0; i < target.width; ++i) {
      destination[i] = BlendSrcOver(destination[i], Color{source[i]});
    }
  }
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
    BlendSpanSrcOver(pixels, static_cast<std::size_t>(span.length), blended);
  }
}

}  // namespace microbrowser::gfx

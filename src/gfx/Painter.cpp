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

void Painter::FillPath(const Path& path, const Paint& paint, float alpha, FillRule rule) {
  if (!(alpha > 0.0f)) {
    return;
  }
  const IntRect clip = canvas_->Clip().Intersected(canvas_->Bounds());
  if (clip.IsEmpty()) {
    return;
  }
  FillSpans(rasterizer_.Rasterize(path, rule, clip, transform_), paint, alpha);
}

void Painter::StrokePath(const Path& path, const StrokeStyle& style, const Paint& paint,
                         float alpha) {
  if (path.IsEmpty() || !(alpha > 0.0f)) {
    return;
  }
  const float scale = std::max(transform_.MaximumScale(), 1e-4f);
  StrokeToPath(path, style, kFlattenTolerance / scale, stroke_scratch_);
  FillPath(stroke_scratch_, paint, alpha, FillRule::NonZero);
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

namespace {

// One bilinear sample from four neighbours.
//
// Interpolated per channel in *non-premultiplied* space, which is wrong at a
// hard edge between an opaque pixel and a fully transparent one -- the
// transparent pixel's colour bleeds in. Premultiplying before interpolating is
// the fix, and it is the same argument Color.h makes for the opposite
// direction; doing it here would mean premultiplying and unpremultiplying per
// sample. Left as it is until an image with a hard alpha edge is scaled and
// someone sees the fringe, which is a measurement rather than a guess.
Color BilinearSample(Color c00, Color c10, Color c01, Color c11, float fx, float fy) {
  const auto channel = [&](auto get) {
    const float top = static_cast<float>(get(c00)) * (1.0f - fx) +
                      static_cast<float>(get(c10)) * fx;
    const float bottom = static_cast<float>(get(c01)) * (1.0f - fx) +
                         static_cast<float>(get(c11)) * fx;
    const float value = top * (1.0f - fy) + bottom * fy;
    return static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
  };
  return Color::Rgba(channel([](Color c) { return c.Red(); }),
                     channel([](Color c) { return c.Green(); }),
                     channel([](Color c) { return c.Blue(); }),
                     channel([](Color c) { return c.Alpha(); }));
}

}  // namespace

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

// An image under a transform that is more than a translation: every device pixel in
// the mapped quad's bounds is taken back through the *inverse* matrix into the image
// and sampled there.
//
// Backwards, from destination to source, because the forward direction leaves holes:
// a scaled-up image walked source-first writes one pixel per source sample and skips
// the gaps between them. This is also why a degenerate matrix draws nothing rather
// than something -- a transform with no inverse has collapsed the image to a line,
// and a line has no interior to fill.
void Painter::DrawImageTransformed(const Image& image, const IntRect& destination) {
  const std::optional<AffineTransform> inverse = transform_.Inverted();
  if (!inverse.has_value()) {
    return;
  }
  const FloatRect box{static_cast<float>(destination.x), static_cast<float>(destination.y),
                      static_cast<float>(destination.width),
                      static_cast<float>(destination.height)};
  const IntRect target =
      EnclosingIntRect(transform_.MapRect(box)).Intersected(canvas_->Clip().Intersected(canvas_->Bounds()));
  if (target.IsEmpty()) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::GfxImagesTransformed);

  const float scale_x = static_cast<float>(image.Width()) / box.width;
  const float scale_y = static_cast<float>(image.Height()) / box.height;
  const int max_x = image.Width() - 1;
  const int max_y = image.Height() - 1;
  for (int y = target.Top(); y < target.Bottom(); ++y) {
    std::uint32_t* row = canvas_->Row(y);
    if (row == nullptr) {
      continue;
    }
    for (int x = target.Left(); x < target.Right(); ++x) {
      // The pixel *centre*, un-mapped. Sampling by corner is the classic
      // half-pixel shift, and under a rotation it is a half-pixel shift in a
      // direction that changes with the angle.
      const FloatPoint local = inverse->MapPoint(
          FloatPoint{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
      // Outside the destination rectangle is outside the image: the bounding box of
      // a rotated quad contains four corners that are not in it, and drawing those
      // would put the edge pixels of the image in them.
      if (local.x < box.x || local.x >= box.x + box.width || local.y < box.y ||
          local.y >= box.y + box.height) {
        continue;
      }
      const float source_x = (local.x - box.x) * scale_x - 0.5f;
      const float source_y = (local.y - box.y) * scale_y - 0.5f;
      const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, max_x);
      const int x1 = std::clamp(x0 + 1, 0, max_x);
      const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0, max_y);
      const int y1 = std::clamp(y0 + 1, 0, max_y);
      const std::uint32_t* top_row = image.Row(y0);
      const std::uint32_t* bottom_row = image.Row(y1);
      if (top_row == nullptr || bottom_row == nullptr) {
        continue;
      }
      const float fx = std::clamp(source_x - static_cast<float>(x0), 0.0f, 1.0f);
      const float fy = std::clamp(source_y - static_cast<float>(y0), 0.0f, 1.0f);
      const Color sample = BilinearSample(Color{top_row[x0]}, Color{top_row[x1]},
                                          Color{bottom_row[x0]}, Color{bottom_row[x1]}, fx, fy);
      row[x] = image.IsOpaque() ? sample.argb : BlendSrcOver(row[x], sample);
    }
  }
}

void Painter::DrawImage(const Image& image, const IntRect& destination) {
  if (!image.IsValid() || destination.IsEmpty()) {
    return;
  }
  if (!transform_.IsTranslationOnly()) {
    DrawImageTransformed(image, destination);
    return;
  }
  if (destination.width == image.Width() && destination.height == image.Height()) {
    // Nothing to resample. Worth checking rather than letting the general path
    // reproduce the identity at four times the cost and with rounding.
    DrawImage(image, IntPoint{destination.x, destination.y});
    return;
  }

  const IntRect clip = canvas_->Clip().Intersected(canvas_->Bounds());
  const int left = destination.x + SaturateFloatToInt(transform_.E());
  const int top = destination.y + SaturateFloatToInt(transform_.F());
  const IntRect placed{std::clamp(left, -kMaxDeviceCoordinate / 2, kMaxDeviceCoordinate / 2),
                       std::clamp(top, -kMaxDeviceCoordinate / 2, kMaxDeviceCoordinate / 2),
                       destination.width, destination.height};
  const IntRect target = placed.Intersected(clip);
  if (target.IsEmpty()) {
    return;
  }

  AddPerformanceCounter(PerfCounterId::GfxImagesScaled);

  // Source pixel centres, so that scaling by one is the identity and scaling a
  // 2x2 up does not sample outside the image at the far edge. Sampling by
  // corner instead is the classic half-pixel shift.
  const float scale_x = static_cast<float>(image.Width()) / static_cast<float>(placed.width);
  const float scale_y = static_cast<float>(image.Height()) / static_cast<float>(placed.height);
  const int max_x = image.Width() - 1;
  const int max_y = image.Height() - 1;

  for (int y = target.Top(); y < target.Bottom(); ++y) {
    std::uint32_t* row = canvas_->Row(y);
    if (row == nullptr) {
      continue;
    }
    const float source_y =
        (static_cast<float>(y - placed.y) + 0.5f) * scale_y - 0.5f;
    const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0, max_y);
    const int y1 = std::clamp(y0 + 1, 0, max_y);
    const float fy = std::clamp(source_y - static_cast<float>(y0), 0.0f, 1.0f);

    const std::uint32_t* top_row = image.Row(y0);
    const std::uint32_t* bottom_row = image.Row(y1);
    if (top_row == nullptr || bottom_row == nullptr) {
      continue;
    }

    for (int x = target.Left(); x < target.Right(); ++x) {
      const float source_x =
          (static_cast<float>(x - placed.x) + 0.5f) * scale_x - 0.5f;
      const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, max_x);
      const int x1 = std::clamp(x0 + 1, 0, max_x);
      const float fx = std::clamp(source_x - static_cast<float>(x0), 0.0f, 1.0f);

      const Color sample = BilinearSample(Color{top_row[x0]}, Color{top_row[x1]},
                                          Color{bottom_row[x0]}, Color{bottom_row[x1]}, fx, fy);
      row[x] = image.IsOpaque() ? sample.argb : BlendSrcOver(row[x], sample);
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

void Painter::FillSpans(const std::vector<CoverageSpan>& spans, const Paint& paint, float alpha) {
  for (const CoverageSpan& span : spans) {
    std::uint32_t* row = canvas_->Row(span.y);
    if (row == nullptr) {
      continue;
    }
    if (span.x < 0 || span.length <= 0 || span.x + span.length > canvas_->Width()) {
      continue;
    }
    // Per pixel, because that is what a paint source is. The pixel *centre* is sampled rather than
    // its corner: a two-stop gradient across a 100px box otherwise reads half a pixel early
    // everywhere, which is invisible on a photograph and exactly what a canvas test measures.
    for (std::int32_t i = 0; i < span.length; ++i) {
      const std::int32_t x = span.x + i;
      const Color source = paint.At(static_cast<float>(x) + 0.5f, static_cast<float>(span.y) + 0.5f);
      const auto combined = static_cast<std::uint8_t>(std::lround(
          std::clamp(static_cast<float>(source.Alpha()) * alpha *
                         (static_cast<float>(span.coverage) / 255.0f),
                     0.0f, 255.0f)));
      if (combined == 0) {
        continue;
      }
      row[x] = BlendSrcOver(row[x], source.WithAlpha(combined));
    }
  }
  AddPerformanceCounter(PerfCounterId::GfxBlendedFills);
}

}  // namespace microbrowser::gfx

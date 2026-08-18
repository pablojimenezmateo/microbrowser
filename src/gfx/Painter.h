#pragma once

#include "gfx/AffineTransform.h"
#include "gfx/Canvas.h"
#include "gfx/Color.h"
#include "gfx/Geometry.h"
#include "gfx/GlyphCache.h"
#include "gfx/Gradient.h"
#include "gfx/Image.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"
#include "gfx/Stroker.h"
#include "gfx/TextShaper.h"

namespace microbrowser::gfx {

// Draws shapes into a Canvas.
//
// Separate from Canvas on purpose, and the separation is the reason Canvas has
// stayed four members: a canvas is a surface plus a clip, and every drawing
// verb that ever gets invented — paths, glyphs, images, gradients, shadows —
// would otherwise land on it. Painter is where they land instead.
//
// A Painter also owns the rasterizer arena, so painting a frame reuses one set
// of cell and span buffers rather than allocating per shape. That is why it is
// a class holding a canvas reference rather than a set of free functions.
class Painter {
 public:
  explicit Painter(Canvas& canvas) : canvas_(&canvas) {}

  Canvas& Target() const { return *canvas_; }

  // Maps layout space to device space for everything drawn afterwards. One
  // current transform rather than a stack: nesting belongs to whatever is
  // building the drawing (a stacking context, a display list), and a stack here
  // would be a second place to get save/restore balance wrong.
  void SetTransform(const AffineTransform& transform) { transform_ = transform; }
  const AffineTransform& Transform() const { return transform_; }

  // Fills the interior of `path` under `rule`, honoring the canvas clip.
  void FillPath(const Path& path, Color color, FillRule rule = FillRule::NonZero);

  // The same, through a paint source that is not one colour -- a gradient or a repeated image.
  //
  // A second entry point rather than a `Paint` that can also be a solid colour, because the solid
  // path is the one every page takes and it must stay a span fill: this one evaluates per *pixel*,
  // which is the cost of asking a question whose answer changes across the span.
  void FillPath(const Path& path, const Paint& paint, float alpha,
                FillRule rule = FillRule::NonZero);
  void StrokePath(const Path& path, const StrokeStyle& style, const Paint& paint, float alpha);

  // Antialiased rectangle fill in layout space. Distinct from Canvas::FillRect,
  // which snaps to whole device pixels: a border or a table rule lands on a
  // fractional boundary, and rounding it there is what produces the classic
  // one-pixel-heavier-on-one-side artifact.
  void FillRect(const FloatRect& rect, Color color);

  // Strokes `path`. The stroke is converted to a fill and rasterized in one
  // pass, so a translucent stroke does not darken where it overlaps itself.
  void StrokePath(const Path& path, const StrokeStyle& style, Color color);
  void StrokeLine(FloatPoint from, FloatPoint to, const StrokeStyle& style, Color color);

  // An image into `destination`, resampled. Honours the whole current transform:
  // a translation or a scale takes the axis-aligned path, and anything else --
  // a rotation, a skew -- is sampled through the inverse matrix.
  //
  // ADR 0014 §4. This was translation-only before `transform` existed, and a
  // rotated image drawn axis-aligned is the failure this file's own comment called
  // out: the rotation would be silently dropped rather than refused.
  void DrawImageTransformed(const Image& image, const IntRect& destination);

  // Draws a shaped run with its baseline origin at `origin`.
  //
  // Takes a run rather than a string: shaping is a separate, cacheable step,
  // and a DrawText(string) convenience would invite re-shaping the same text
  // every frame — which is the single most expensive mistake a text stack can
  // make. TextShaper produces the run; this consumes it.
  void DrawGlyphs(const Font& font, const ShapedRun& run, FloatPoint origin, Color color);

  // The glyph cache this painter draws through. Exposed so a test can assert on
  // hit rates and a caller can bound its memory, not so anyone can reach past
  // DrawGlyphs.
  GlyphCache& Glyphs() { return glyphs_; }

  // Draws `image` with its top-left corner at `at`, honoring the canvas clip.
  //
  // Unscaled and unrotated: the transform's translation is applied and anything
  // else is ignored. Scaled image drawing needs a resampler, and a resampler
  // chosen without a use case is a resampler chosen wrong — CSS
  // `image-rendering` and device-pixel-ratio scaling both have opinions that
  // arrive with layout.
  void DrawImage(const Image& image, IntPoint at);

  // Scales `image` into `destination`, honoring the canvas clip.
  //
  // Bilinear, and the choice is a use case rather than a taste: `<img
  // width=40>` on a 400px image is the common case on the web, and nearest
  // neighbour on a photograph downscaled tenfold is visibly wrong in a way
  // that no amount of speed makes up for. A destination the same size as the
  // image takes the unscaled path, so the resampler costs nothing when nothing
  // is being resampled.
  //
  // Still unrotated: the transform's translation is applied and the rest is
  // ignored. A rotated image needs the same resampler run over an inverse
  // transform, which arrives with CSS transforms.
  void DrawImage(const Image& image, const IntRect& destination);

  // Blends a rasterized coverage span set. Public because a glyph mask and an
  // image alpha channel produce the same thing and must not each grow their own
  // blitter.
  void FillSpans(const std::vector<CoverageSpan>& spans, Color color);
  void FillSpans(const std::vector<CoverageSpan>& spans, const Paint& paint, float alpha);

  const PathRasterizer& Rasterizer() const { return rasterizer_; }

 private:
  void BlitGlyph(const GlyphImage& image, int x, int y, Color color);

  Canvas* canvas_;
  PathRasterizer rasterizer_;
  // Reused across strokes for the same reason the rasterizer arena is: a stroke
  // allocates a path several times the size of its input, and a frame contains
  // many.
  Path stroke_scratch_;
  // Glyph outlines land here on their way to the rasterizer. One buffer for the
  // whole run: a page of text is tens of thousands of glyphs, and a fresh Path
  // per glyph would be tens of thousands of allocations per frame.
  Path glyph_scratch_;
  GlyphCache glyphs_;
  AffineTransform transform_;
};

}  // namespace microbrowser::gfx

#pragma once

#include "gfx/Canvas.h"
#include "gfx/Color.h"
#include "gfx/Geometry.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"
#include "gfx/Stroker.h"

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

  // Fills the interior of `path` under `rule`, honoring the canvas clip.
  void FillPath(const Path& path, Color color, FillRule rule = FillRule::NonZero);

  // Antialiased rectangle fill in layout space. Distinct from Canvas::FillRect,
  // which snaps to whole device pixels: a border or a table rule lands on a
  // fractional boundary, and rounding it there is what produces the classic
  // one-pixel-heavier-on-one-side artifact.
  void FillRect(const FloatRect& rect, Color color);

  // Strokes `path`. The stroke is converted to a fill and rasterized in one
  // pass, so a translucent stroke does not darken where it overlaps itself.
  void StrokePath(const Path& path, const StrokeStyle& style, Color color);
  void StrokeLine(FloatPoint from, FloatPoint to, const StrokeStyle& style, Color color);

  // Blends a rasterized coverage span set. Public because a glyph mask and an
  // image alpha channel produce the same thing and must not each grow their own
  // blitter.
  void FillSpans(const std::vector<CoverageSpan>& spans, Color color);

  const PathRasterizer& Rasterizer() const { return rasterizer_; }

 private:
  Canvas* canvas_;
  PathRasterizer rasterizer_;
  // Reused across strokes for the same reason the rasterizer arena is: a stroke
  // allocates a path several times the size of its input, and a frame contains
  // many.
  Path stroke_scratch_;
};

}  // namespace microbrowser::gfx

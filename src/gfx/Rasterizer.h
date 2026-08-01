#pragma once

#include <cstdint>
#include <vector>

#include "gfx/Geometry.h"
#include "gfx/Path.h"
#include "gfx/PathFlattener.h"

namespace microbrowser::gfx {

// Which interior a path encloses.
//
// NonZero is what fonts, CSS backgrounds, and borders use; EvenOdd is what SVG
// `fill-rule: evenodd` and `clip-rule` ask for. They differ only where a path
// overlaps itself, which is exactly where getting it wrong is invisible in a
// test that draws a rectangle.
enum class FillRule : std::uint8_t {
  NonZero,
  EvenOdd,
};

// A run of horizontal pixels sharing one coverage value.
//
// Coverage is 0..255, where 255 means the pixel is entirely inside the path.
// It is *not* premultiplied into a color: the same span set is reused for a
// solid fill, a gradient, and a glyph mask, and folding a color in here would
// force a re-rasterization for each.
struct CoverageSpan {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t length = 0;
  std::uint8_t coverage = 0;

  friend bool operator==(const CoverageSpan&, const CoverageSpan&) = default;
};

// One accumulation cell: a pixel that at least one edge passes through.
//
// Public only because it is a `std::vector` member below. `cover` is the summed
// signed subpixel height of the edge portions crossing the pixel; `area` is
// twice the summed area to their left. Coverage falls out as
// `cover * 2 * 256 - area`, which is why both are kept rather than a single
// blended number — the two combine differently for the interior span that
// follows the cell.
struct RasterCell {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t cover = 0;
  std::int32_t area = 0;
};

// Scanline rasterizer with analytic anti-aliasing.
//
// The algorithm is FreeType's `ftgrays` cell accumulation, which is also what
// libart, stb_truetype and every serious CPU rasterizer converged on. It
// computes the *exact* area of each pixel covered by the path rather than
// sampling it N times, so a near-horizontal edge — the case that makes
// supersampling look banded — costs the same as any other and is exactly right.
//
// Memory is proportional to the path's perimeter, not to the surface: only
// pixels an edge actually touches get a cell, and the interior between cells is
// emitted as a span with no per-pixel state. A full-page rectangle costs a few
// hundred cells; a coverage-buffer rasterizer would cost eight megabytes.
//
// Instances are reusable and are meant to be: the cell and span arenas are kept
// across calls, so steady-state painting allocates nothing.
class PathRasterizer {
 public:
  // Rasterizes `path` under `rule`, restricted to `clip`, and returns the
  // resulting spans in top-to-bottom, left-to-right order.
  //
  // Geometry outside `clip` is not merely skipped, it is projected: a contour
  // running off the left edge still contributes its winding to the pixels that
  // remain visible, which is what keeps a scrolled-in shape from developing a
  // hole. Returned spans are always inside `clip`.
  const std::vector<CoverageSpan>& Rasterize(const Path& path, FillRule rule, const IntRect& clip,
                                             float tolerance = kFlattenTolerance);

  const std::vector<CoverageSpan>& Spans() const { return spans_; }

  // Retained arena capacity, in bytes. Exposed so a memory regression in the
  // paint path is a number a test can assert on rather than a thing somebody
  // notices in `top`.
  std::size_t ArenaBytes() const;

 private:
  std::vector<RasterCell> cells_;
  std::vector<CoverageSpan> spans_;
};

static_assert(sizeof(CoverageSpan) <= 16, "spans are produced per scanline; keep them small");
static_assert(sizeof(RasterCell) <= 16, "one cell per edge-touched pixel; keep them small");

}  // namespace microbrowser::gfx

#pragma once

#include <cstdint>

#include "gfx/Path.h"
#include "gfx/PathFlattener.h"

namespace microbrowser::gfx {

// SVG and CSS name these identically, and so do we.
enum class LineCap : std::uint8_t { Butt, Round, Square };
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };

struct StrokeStyle {
  float width = 1.0f;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  // SVG's `stroke-miterlimit` default: the ratio of miter length to stroke
  // width past which a miter degenerates into a bevel. Without it, two nearly
  // parallel segments produce a spike thousands of pixels long.
  float miter_limit = 4.0f;

  friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

// Converts a stroke into a fill.
//
// `out` is the *union of convex pieces* — one quad per segment, one wedge or
// disc per join, one shape per cap — not a traced outline. Filling that union
// with FillRule::NonZero gives the same pixels as an outline would, and gets
// there without the case analysis that makes offset-curve strokers fragile:
// self-intersecting contours, cusps, and segments shorter than the stroke width
// all just work, because overlapping pieces of equal winding are still winding
// 1 or more.
//
// Two properties make the trick sound rather than merely convenient. Every
// piece is emitted with the same orientation, so an overlap never cancels to a
// hole; and the whole union is rasterized in one pass, so a translucent stroke
// is not double-blended and the seams between abutting pieces cancel exactly
// in the coverage accumulator instead of showing as hairlines.
//
// `out` is cleared first and reused, so a caller painting many strokes keeps
// one buffer.
void StrokeToPath(const Path& path, const StrokeStyle& style, float tolerance, Path& out);

inline void StrokeToPath(const Path& path, const StrokeStyle& style, Path& out) {
  StrokeToPath(path, style, kFlattenTolerance, out);
}

}  // namespace microbrowser::gfx

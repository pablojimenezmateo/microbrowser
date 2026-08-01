#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gfx/Geometry.h"

namespace microbrowser::gfx {

// The shape vocabulary of the rasterizer: move, line, quadratic, cubic, close.
//
// Exactly the set every 2D path format reduces to. Arcs and ellipses are not
// verbs — they are built from cubics by the code that wants them, so the
// rasterizer never grows a second curve evaluator.
enum class PathVerb : std::uint8_t {
  Move,   // 1 point: the start of a new contour
  Line,   // 1 point
  Quad,   // 2 points: control, end
  Cubic,  // 3 points: control 1, control 2, end
  Close,  // 0 points: a line back to the contour start
};

constexpr std::size_t PointsForVerb(PathVerb verb) {
  switch (verb) {
    case PathVerb::Move:
    case PathVerb::Line:
      return 1;
    case PathVerb::Quad:
      return 2;
    case PathVerb::Cubic:
      return 3;
    case PathVerb::Close:
      return 0;
  }
  return 0;
}

// A sequence of contours in layout (float) space.
//
// Two structural decisions, both load-bearing:
//
//   * Verbs and points live in separate flat vectors rather than a vector of
//     variant commands. A path is walked far more often than it is built, and
//     the walk wants both arrays streaming linearly. It also keeps the per-verb
//     cost at one byte.
//   * **Non-finite coordinates never enter.** A command carrying a NaN or an
//     infinity is dropped at the builder. Everything downstream — flattening,
//     fixed-point conversion, the rasterizer's integer math — may therefore
//     assume finite input, which is the difference between a clipped shape and
//     undefined behavior in a conversion. Layout will produce NaN eventually
//     (0/0 in a percentage resolve is one line of CSS away), and the guarantee
//     belongs where it can be stated once.
//
// Coordinates are *not* clamped here. Magnitude is a rasterizer concern, and
// the rasterizer clips to the target before converting to fixed point.
class Path {
 public:
  void MoveTo(FloatPoint p);
  void LineTo(FloatPoint p);
  void QuadTo(FloatPoint control, FloatPoint end);
  void CubicTo(FloatPoint control1, FloatPoint control2, FloatPoint end);
  void Close();

  void AddRect(const FloatRect& rect);
  // Per-corner radii, in the CSS order: top-left, top-right, bottom-right,
  // bottom-left. Radii are clamped so that adjacent corners cannot overlap,
  // which is the CSS `border-radius` overlap rule and also the only way to keep
  // the generated contour non-self-intersecting.
  void AddRoundedRect(const FloatRect& rect, float top_left, float top_right, float bottom_right,
                      float bottom_left);
  void AddEllipse(const FloatRect& bounds);

  void Clear();

  bool IsEmpty() const { return verbs_.empty(); }
  std::size_t VerbCount() const { return verbs_.size(); }
  std::span<const PathVerb> Verbs() const { return verbs_; }
  std::span<const FloatPoint> Points() const { return points_; }

  // The point a subsequent Line/Quad/Cubic starts from. Origin when there is no
  // open contour.
  FloatPoint CurrentPoint() const;

  // Bounds of the control polygon: contains the path, but is not tight for
  // curves. Tight bounds need root finding, and every caller so far wants a
  // conservative box for clipping, where "conservative" is the requirement.
  FloatRect ControlBounds() const;

  friend bool operator==(const Path&, const Path&) = default;

 private:
  // A Line/Quad/Cubic with no contour open starts one instead of drawing an
  // edge from the origin. An invented edge from (0,0) is a wedge across the
  // whole surface; a degenerate zero-length contour paints nothing.
  bool EnsureContour(FloatPoint start);

  std::vector<PathVerb> verbs_;
  std::vector<FloatPoint> points_;
  FloatPoint contour_start_;
  bool has_contour_ = false;
};

}  // namespace microbrowser::gfx

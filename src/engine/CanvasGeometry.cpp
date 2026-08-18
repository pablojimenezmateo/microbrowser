#include "engine/CanvasGeometry.h"

#include <algorithm>
#include <cmath>

#include <span>

#include "gfx/PathFlattener.h"

namespace microbrowser::engine {

namespace {

constexpr double kTwoPi = 6.283185307179586;

gfx::FloatPoint Apply(const gfx::AffineTransform& transform, double x, double y) {
  return transform.MapPoint(gfx::FloatPoint{static_cast<float>(x), static_cast<float>(y)});
}

bool Finite(double a, double b) { return std::isfinite(a) && std::isfinite(b); }

// The transform's linear part applied to an offset. `MapPoint` would add the translation, which for a
// control-point offset means adding it twice.
gfx::FloatPoint MapVector(const gfx::AffineTransform& transform, float x, float y) {
  return gfx::FloatPoint{transform.A() * x + transform.C() * y,
                         transform.B() * x + transform.D() * y};
}

// The sink `PathContainsPoint` flattens through: it counts how many times the polyline crosses the
// ray going right from the query point, and with what winding.
//
// Every contour is treated as closed, which is the specification's rule for `isPointInPath` -- an
// unclosed subpath is filled as if it were closed, so the hit test has to agree with the fill.
struct CrossingCounter {
  double x = 0.0;
  double y = 0.0;
  int winding = 0;
  int crossings = 0;
  gfx::FloatPoint start{};
  gfx::FloatPoint current{};
  bool open = false;

  void Segment(gfx::FloatPoint from, gfx::FloatPoint to) {
    const double y0 = static_cast<double>(from.y);
    const double y1 = static_cast<double>(to.y);
    if ((y0 <= y) == (y1 <= y)) {
      return;  // the segment does not straddle the ray's row
    }
    const double t = (y - y0) / (y1 - y0);
    const double crossing =
        static_cast<double>(from.x) + t * (static_cast<double>(to.x) - static_cast<double>(from.x));
    if (crossing <= x) {
      return;  // the crossing is behind the query point
    }
    ++crossings;
    winding += y1 > y0 ? 1 : -1;
  }

  void BeginContour(gfx::FloatPoint at) {
    Finish();
    start = at;
    current = at;
    open = true;
  }
  void LineTo(gfx::FloatPoint at) {
    Segment(current, at);
    current = at;
  }
  void EndContour(bool) { Finish(); }

  void Finish() {
    if (open) {
      Segment(current, start);
      open = false;
    }
  }
};

}  // namespace

void AppendEllipseArc(gfx::Path& path, const gfx::AffineTransform& transform, double cx, double cy,
                      double radius_x, double radius_y, double rotation, double start, double end,
                      bool counter_clockwise, bool& have_current) {
  if (!Finite(cx, cy) || !Finite(radius_x, radius_y) || !Finite(start, end) ||
      !std::isfinite(rotation) || radius_x < 0.0 || radius_y < 0.0) {
    return;
  }
  double sweep = end - start;
  if (counter_clockwise) {
    // Not just a sign: `arc(..., 0, 3*pi, true)` sweeps *backwards* by more than a full turn, which
    // draws a full circle. Normalising into (-2pi, 0] and then clamping to a full turn is what
    // produces that.
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
  const double cos_rotation = std::cos(rotation);
  const double sin_rotation = std::sin(rotation);
  // A point on the ellipse at parameter `angle`, rotated and translated into user space, then through
  // the current transform. The parameter is not the polar angle for an ellipse, which is fine: the
  // specification defines `ellipse()` in exactly these terms.
  const auto at = [&](double angle) {
    const double ex = radius_x * std::cos(angle);
    const double ey = radius_y * std::sin(angle);
    return Apply(transform, cx + ex * cos_rotation - ey * sin_rotation,
                 cy + ex * sin_rotation + ey * cos_rotation);
  };
  // The derivative, for the cubic control points. Same rotation, and the same reason it is exact
  // rather than a finite difference: a quarter-turn cubic's error budget is a thousandth of a radius
  // and a numerical tangent spends all of it.
  const auto tangent = [&](double angle) {
    const double ex = -radius_x * std::sin(angle);
    const double ey = radius_y * std::cos(angle);
    return gfx::FloatPoint{static_cast<float>(ex * cos_rotation - ey * sin_rotation),
                           static_cast<float>(ex * sin_rotation + ey * cos_rotation)};
  };
  const gfx::FloatPoint first = at(start);
  if (!have_current) {
    path.MoveTo(first);
    have_current = true;
  } else {
    path.LineTo(first);
  }
  if (sweep == 0.0) {
    return;
  }
  // At most a quarter turn per cubic, which is the standard bound for a quarter-arc's error.
  const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (kTwoPi / 8.0))));
  const double step = sweep / segments;
  // The tangent-length factor for a cubic approximation of an arc of this angle.
  const double alpha = std::sin(step) *
                       (std::sqrt(4.0 + 3.0 * std::tan(step * 0.5) * std::tan(step * 0.5)) - 1.0) /
                       3.0;
  double angle = start;
  for (int i = 0; i < segments; ++i) {
    const double next = angle + step;
    const gfx::FloatPoint p0 = at(angle);
    const gfx::FloatPoint p1 = at(next);
    const gfx::FloatPoint t0 = tangent(angle);
    const gfx::FloatPoint t1 = tangent(next);
    // The control points are user-space offsets from the endpoints, so the transform is applied to
    // them as *vectors*: its linear part only, because a translation added to an offset would be a
    // translation applied twice.
    const gfx::FloatPoint c0 = MapVector(transform, static_cast<float>(alpha) * t0.x,
                                         static_cast<float>(alpha) * t0.y);
    const gfx::FloatPoint c1 = MapVector(transform, static_cast<float>(-alpha) * t1.x,
                                         static_cast<float>(-alpha) * t1.y);
    path.CubicTo(gfx::FloatPoint{p0.x + c0.x, p0.y + c0.y},
                 gfx::FloatPoint{p1.x + c1.x, p1.y + c1.y}, p1);
    angle = next;
  }
}

void AppendArcTo(gfx::Path& path, const gfx::AffineTransform& transform, double from_x,
                 double from_y, double x1, double y1, double x2, double y2, double radius) {
  if (!Finite(from_x, from_y) || !Finite(x1, y1) || !Finite(x2, y2) || !std::isfinite(radius)) {
    return;
  }
  // The three degenerate cases the specification names, and each really is a straight line rather
  // than an approximation: the tangent circle is undefined or has collapsed to a point.
  const double d1x = from_x - x1;
  const double d1y = from_y - y1;
  const double d2x = x2 - x1;
  const double d2y = y2 - y1;
  const double len1 = std::sqrt(d1x * d1x + d1y * d1y);
  const double len2 = std::sqrt(d2x * d2x + d2y * d2y);
  const double cross = d1x * d2y - d1y * d2x;
  if (len1 == 0.0 || len2 == 0.0 || radius == 0.0 || cross == 0.0) {
    path.LineTo(Apply(transform, x1, y1));
    return;
  }
  // The half-angle at the corner, from the unit vectors along each leg. `tan(theta/2)` is what
  // converts the radius into how far back along each leg the tangent points sit.
  const double u1x = d1x / len1;
  const double u1y = d1y / len1;
  const double u2x = d2x / len2;
  const double u2y = d2y / len2;
  const double cos_theta = std::clamp(u1x * u2x + u1y * u2y, -1.0, 1.0);
  const double theta = std::acos(cos_theta);
  const double tangent_length = radius / std::tan(theta * 0.5);
  const double t1x = x1 + u1x * tangent_length;
  const double t1y = y1 + u1y * tangent_length;
  const double t2x = x1 + u2x * tangent_length;
  const double t2y = y1 + u2y * tangent_length;
  // The centre is along the corner's bisector, at the distance that puts it `radius` from both legs.
  double bx = u1x + u2x;
  double by = u1y + u2y;
  const double bisector = std::sqrt(bx * bx + by * by);
  if (bisector == 0.0) {
    path.LineTo(Apply(transform, x1, y1));
    return;
  }
  bx /= bisector;
  by /= bisector;
  const double centre_distance = radius / std::sin(theta * 0.5);
  const double cx = x1 + bx * centre_distance;
  const double cy = y1 + by * centre_distance;
  const double start = std::atan2(t1y - cy, t1x - cx);
  const double end = std::atan2(t2y - cy, t2x - cx);
  // Which way round is decided by the sign of the corner's cross product: a left turn sweeps one way
  // and a right turn the other, and getting it backwards draws the arc's 300-degree complement.
  bool have_current = true;
  AppendEllipseArc(path, transform, cx, cy, radius, radius, 0.0, start, end, cross > 0.0,
                   have_current);
}

void AppendRoundRect(gfx::Path& path, const gfx::AffineTransform& transform, double x, double y,
                     double width, double height, const std::vector<double>& radii) {
  if (!Finite(x, y) || !Finite(width, height) || radii.size() != 8) {
    return;
  }
  // Top-left, top-right, bottom-right, bottom-left, each as (x, y).
  const double tlx = radii[0];
  const double tly = radii[1];
  const double trx = radii[2];
  const double try_ = radii[3];
  const double brx = radii[4];
  const double bry = radii[5];
  const double blx = radii[6];
  const double bly = radii[7];
  const double right = x + width;
  const double bottom = y + height;
  // The specification's own construction, and the direction matters: with a negative width or height
  // the corners are traversed the other way round, which is what makes a flipped `roundRect` wind
  // oppositely -- observable through `fill('nonzero')` when it overlaps another subpath.
  bool have_current = false;
  const bool flipped = (width < 0.0) != (height < 0.0);
  const auto corner = [&](double cx, double cy, double rx, double ry, double from, double to) {
    AppendEllipseArc(path, transform, cx, cy, std::abs(rx), std::abs(ry), 0.0, from, to, flipped,
                     have_current);
  };
  constexpr double kPi = 3.141592653589793;
  path.MoveTo(Apply(transform, x + tlx, y));
  have_current = true;
  path.LineTo(Apply(transform, right - trx, y));
  corner(right - trx, y + try_, trx, try_, -kPi * 0.5, 0.0);
  path.LineTo(Apply(transform, right, bottom - bry));
  corner(right - brx, bottom - bry, brx, bry, 0.0, kPi * 0.5);
  path.LineTo(Apply(transform, x + blx, bottom));
  corner(x + blx, bottom - bly, blx, bly, kPi * 0.5, kPi);
  path.LineTo(Apply(transform, x, y + tly));
  corner(x + tlx, y + tly, tlx, tly, kPi, kPi * 1.5);
  path.Close();
}

bool PathContainsPoint(const gfx::Path& path, double x, double y, bool even_odd) {
  if (path.IsEmpty() || !Finite(x, y)) {
    return false;
  }
  CrossingCounter counter;
  counter.x = x;
  counter.y = y;
  gfx::FlattenPath(path, gfx::AffineTransform{}, gfx::kFlattenTolerance, counter);
  counter.Finish();
  return even_odd ? (counter.crossings % 2) != 0 : counter.winding != 0;
}

// A path with `transform` applied to every point. `gfx::Path` has no transform verb -- a path is
// geometry and a transform is a caller's business -- so the walk is here.
gfx::Path Transformed(const gfx::Path& path, const gfx::AffineTransform& transform) {
  gfx::Path out;
  const std::span<const gfx::PathVerb> verbs = path.Verbs();
  const std::span<const gfx::FloatPoint> points = path.Points();
  std::size_t at = 0;
  for (const gfx::PathVerb verb : verbs) {
    switch (verb) {
      case gfx::PathVerb::Move:
        out.MoveTo(transform.MapPoint(points[at]));
        break;
      case gfx::PathVerb::Line:
        out.LineTo(transform.MapPoint(points[at]));
        break;
      case gfx::PathVerb::Quad:
        out.QuadTo(transform.MapPoint(points[at]), transform.MapPoint(points[at + 1]));
        break;
      case gfx::PathVerb::Cubic:
        out.CubicTo(transform.MapPoint(points[at]), transform.MapPoint(points[at + 1]),
                    transform.MapPoint(points[at + 2]));
        break;
      case gfx::PathVerb::Close:
        out.Close();
        break;
    }
    at += gfx::PointsForVerb(verb);
  }
  return out;
}


}  // namespace microbrowser::engine

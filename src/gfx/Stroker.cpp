#include "gfx/Stroker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Points closer than this are the same point. Below it the segment direction is
// numerical noise, and a stroker that trusts a noisy direction emits a wedge
// pointing in a random direction — the classic "spike" artifact.
constexpr float kCoincident = 1e-5f;

struct Contour {
  std::vector<FloatPoint> points;
  bool closed = false;
};

class ContourCollector {
 public:
  void BeginContour(FloatPoint p) {
    contours_.emplace_back();
    contours_.back().points.push_back(p);
  }
  void LineTo(FloatPoint p) { contours_.back().points.push_back(p); }
  void EndContour(bool closed) { contours_.back().closed = closed; }

  std::vector<Contour>& Contours() { return contours_; }

 private:
  std::vector<Contour> contours_;
};

FloatPoint Sub(FloatPoint a, FloatPoint b) {
  return FloatPoint{a.x - b.x, a.y - b.y};
}

FloatPoint Add(FloatPoint a, FloatPoint b) {
  return FloatPoint{a.x + b.x, a.y + b.y};
}

FloatPoint Scaled(FloatPoint p, float k) {
  return FloatPoint{p.x * k, p.y * k};
}

float Length(FloatPoint p) {
  return std::sqrt(p.x * p.x + p.y * p.y);
}

// Returns false when the two points are coincident, so callers never divide by
// a length they did not check.
bool Direction(FloatPoint from, FloatPoint to, FloatPoint& out) {
  const FloatPoint delta = Sub(to, from);
  const float length = Length(delta);
  if (!(length > kCoincident)) {
    return false;
  }
  out = Scaled(delta, 1.0f / length);
  return true;
}

FloatPoint Normal(FloatPoint direction) {
  return FloatPoint{-direction.y, direction.x};
}

float Cross(FloatPoint a, FloatPoint b) {
  return a.x * b.y - a.y * b.x;
}

float Dot(FloatPoint a, FloatPoint b) {
  return a.x * b.x + a.y * b.y;
}

// Appends a convex polygon with a canonical orientation.
//
// The orientation normalization is the load-bearing line in this file: a single
// piece wound backwards would subtract from its overlap with a neighbour under
// the nonzero rule, punching a hole through the middle of a stroke rather than
// producing a visibly wrong outline. Normalizing here means no piece emitter
// has to reason about which way its own vertices happen to run.
void AddPolygon(Path& out, std::span<const FloatPoint> points) {
  if (points.size() < 3) {
    return;
  }
  float twice_area = 0.0f;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const FloatPoint& a = points[i];
    const FloatPoint& b = points[(i + 1) % points.size()];
    twice_area += a.x * b.y - b.x * a.y;
  }
  if (twice_area == 0.0f) {
    return;
  }

  if (twice_area > 0.0f) {
    out.MoveTo(points[0]);
    for (std::size_t i = 1; i < points.size(); ++i) {
      out.LineTo(points[i]);
    }
  } else {
    out.MoveTo(points.back());
    for (std::size_t i = points.size() - 1; i-- > 0;) {
      out.LineTo(points[i]);
    }
  }
  out.Close();
}

// Segment count for a circle of `radius` that stays within `tolerance`: the
// sagitta of a chord subtending angle t is r(1 - cos(t/2)), which inverts to
// pi * sqrt(r / 2 tolerance) segments.
std::size_t CircleSegments(float radius, float tolerance) {
  const float safe_tolerance = std::max(tolerance, 1e-4f);
  const float count = 3.14159265f * std::sqrt(radius / (2.0f * safe_tolerance));
  if (!(count > 8.0f)) {
    return 8;
  }
  return static_cast<std::size_t>(std::min(std::ceil(count), 256.0f));
}

void AddDisc(Path& out, FloatPoint center, float radius, float tolerance) {
  const std::size_t steps = CircleSegments(radius, tolerance);
  std::vector<FloatPoint> points;
  points.reserve(steps);
  for (std::size_t i = 0; i < steps; ++i) {
    const float angle =
        6.28318531f * static_cast<float>(i) / static_cast<float>(steps);
    points.push_back(FloatPoint{center.x + radius * std::cos(angle),
                                center.y + radius * std::sin(angle)});
  }
  AddPolygon(out, points);
}

void AddSegmentQuad(Path& out, FloatPoint a, FloatPoint b, FloatPoint direction, float half) {
  const FloatPoint offset = Scaled(Normal(direction), half);
  const std::array<FloatPoint, 4> quad{Add(a, offset), Add(b, offset), Sub(b, offset),
                                       Sub(a, offset)};
  AddPolygon(out, quad);
}

void AddCap(Path& out, FloatPoint tip, FloatPoint direction, float half, LineCap cap,
            float tolerance) {
  switch (cap) {
    case LineCap::Butt:
      return;
    case LineCap::Round:
      // A full disc rather than a half one: the inner half lands inside the
      // segment quad it caps, where the union swallows it.
      AddDisc(out, tip, half, tolerance);
      return;
    case LineCap::Square: {
      const FloatPoint offset = Scaled(Normal(direction), half);
      const FloatPoint extension = Scaled(direction, half);
      const std::array<FloatPoint, 4> quad{Add(tip, offset), Add(Add(tip, offset), extension),
                                           Add(Sub(tip, offset), extension), Sub(tip, offset)};
      AddPolygon(out, quad);
      return;
    }
  }
}

void AddJoin(Path& out, FloatPoint vertex, FloatPoint incoming, FloatPoint outgoing, float half,
             const StrokeStyle& style, float tolerance) {
  const float turn = Cross(incoming, outgoing);
  if (turn == 0.0f && Dot(incoming, outgoing) > 0.0f) {
    return;  // straight through: the two quads already meet flush
  }
  if (style.join == LineJoin::Round) {
    AddDisc(out, vertex, half, tolerance);
    return;
  }

  // The join fills the wedge on the *outside* of the turn; the inside is
  // already covered twice over by the two segment quads.
  const float side = turn > 0.0f ? -1.0f : 1.0f;
  const FloatPoint outer_in = Scaled(Normal(incoming), side * half);
  const FloatPoint outer_out = Scaled(Normal(outgoing), side * half);
  const FloatPoint corner_in = Add(vertex, outer_in);
  const FloatPoint corner_out = Add(vertex, outer_out);

  if (style.join == LineJoin::Miter) {
    const FloatPoint sum = Add(outer_in, outer_out);
    const float sum_length = Length(sum);
    if (sum_length > kCoincident) {
      const FloatPoint bisector = Scaled(sum, 1.0f / sum_length);
      // cos of half the exterior angle, which is sin(theta/2) for the interior
      // angle SVG's miter-limit rule is written against — so the ratio below is
      // exactly `miterLength / stroke-width`.
      const float cos_half = Dot(bisector, outer_in) / half;
      if (cos_half > 0.0f && 1.0f / cos_half <= style.miter_limit) {
        const std::array<FloatPoint, 4> wedge{vertex, corner_in,
                                              Add(vertex, Scaled(bisector, half / cos_half)),
                                              corner_out};
        AddPolygon(out, wedge);
        return;
      }
    }
    AddPerformanceCounter(PerfCounterId::GfxStrokeMiterFallbacks);
  }

  const std::array<FloatPoint, 3> bevel{vertex, corner_in, corner_out};
  AddPolygon(out, bevel);
}

// A contour that collapsed to a single point. SVG renders this for round and
// square caps and draws nothing for butt caps, which is also the only reading
// that makes a dotted line with round caps work.
void AddDot(Path& out, FloatPoint center, float half, const StrokeStyle& style, float tolerance) {
  switch (style.cap) {
    case LineCap::Butt:
      return;
    case LineCap::Round:
      AddDisc(out, center, half, tolerance);
      return;
    case LineCap::Square: {
      const std::array<FloatPoint, 4> square{FloatPoint{center.x - half, center.y - half},
                                             FloatPoint{center.x + half, center.y - half},
                                             FloatPoint{center.x + half, center.y + half},
                                             FloatPoint{center.x - half, center.y + half}};
      AddPolygon(out, square);
      return;
    }
  }
}

void DropCoincidentPoints(std::vector<FloatPoint>& points, bool closed) {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (kept > 0 && Length(Sub(points[i], points[kept - 1])) <= kCoincident) {
      continue;
    }
    points[kept++] = points[i];
  }
  points.resize(kept);
  // A closed contour that repeated its start point explicitly would otherwise
  // carry a zero-length wrap-around segment and a join with no direction.
  if (closed && points.size() > 1 &&
      Length(Sub(points.front(), points.back())) <= kCoincident) {
    points.pop_back();
  }
}

}  // namespace

void StrokeToPath(const Path& path, const StrokeStyle& style, float tolerance, Path& out) {
  out.Clear();
  AddPerformanceCounter(PerfCounterId::GfxStrokes);
  const float half = style.width * 0.5f;
  if (!std::isfinite(half) || half <= 0.0f) {
    return;
  }

  ContourCollector collector;
  FlattenPath(path, tolerance, collector);

  for (Contour& contour : collector.Contours()) {
    DropCoincidentPoints(contour.points, contour.closed);
    const std::vector<FloatPoint>& points = contour.points;
    const std::size_t count = points.size();
    if (count == 0) {
      continue;
    }
    if (count == 1) {
      AddDot(out, points[0], half, style, tolerance);
      continue;
    }

    const std::size_t segments = contour.closed ? count : count - 1;
    for (std::size_t i = 0; i < segments; ++i) {
      const FloatPoint a = points[i];
      const FloatPoint b = points[(i + 1) % count];
      FloatPoint direction{};
      if (Direction(a, b, direction)) {
        AddSegmentQuad(out, a, b, direction, half);
      }
    }

    // Interior vertices for an open contour; every vertex for a closed one.
    const std::size_t joins = contour.closed ? count : count - 2;
    for (std::size_t j = 0; j < joins; ++j) {
      const std::size_t vertex = contour.closed ? j : j + 1;
      FloatPoint incoming{};
      FloatPoint outgoing{};
      if (Direction(points[(vertex + count - 1) % count], points[vertex], incoming) &&
          Direction(points[vertex], points[(vertex + 1) % count], outgoing)) {
        AddJoin(out, points[vertex], incoming, outgoing, half, style, tolerance);
      }
    }

    if (!contour.closed) {
      FloatPoint direction{};
      if (Direction(points[1], points[0], direction)) {
        AddCap(out, points[0], direction, half, style.cap, tolerance);
      }
      if (Direction(points[count - 2], points[count - 1], direction)) {
        AddCap(out, points[count - 1], direction, half, style.cap, tolerance);
      }
    }
  }

  AddPerformanceCounter(PerfCounterId::GfxStrokePieces, out.VerbCount());
}

}  // namespace microbrowser::gfx

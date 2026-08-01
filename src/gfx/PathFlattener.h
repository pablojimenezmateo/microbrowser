#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "gfx/AffineTransform.h"
#include "gfx/Path.h"

namespace microbrowser::gfx {

// Curve subdivision: a Path becomes a sequence of polylines.
//
// Header-only and templated on the sink rather than taking a std::function. The
// sink is called once per line segment of every glyph of every page, so an
// indirect call per segment is the difference between an inlined store and a
// pipeline stall — this is the one place in gfx where a template is worth more
// than a stable interface.
//
// The sink is any type with:
//     void BeginContour(FloatPoint start);
//     void LineTo(FloatPoint p);
//     void EndContour(bool closed);
//
// `closed` is reported rather than resolved: a filler treats every contour as
// closed, a stroker must not, and deciding here would make the flattener wrong
// for one of them.

// Default flattening tolerance, in device pixels: the maximum distance the
// polyline may deviate from the true curve.
//
// A tenth of a pixel rather than the quarter that most rasterizers document,
// because the error is not random — a chord is always *inside* its arc, so
// every curve is systematically under-filled. For a circle of radius r the
// enclosed area comes out low by 4t/3r, which at t = 0.25 is 1.7% on a 20px
// radius: a visible inward pull on every rounded corner, in the same direction
// every time. Segment count only grows as 1/sqrt(t), so buying the bias down
// by 2.5x costs 1.6x the segments.
inline constexpr float kFlattenTolerance = 0.1f;

namespace detail {

inline float Hypotenuse(float dx, float dy) {
  return std::sqrt(dx * dx + dy * dy);
}

// Segment count for a uniform subdivision that stays within `tolerance`.
//
// Both curve types have chord error `deviation / (4n^2)` for n uniform pieces,
// where `deviation` is the second-order term of the control polygon — so both
// invert to the same square root. Deriving it (rather than subdividing
// recursively until a flatness test passes) means the segment count is known
// before the walk, so the output vector is sized once.
inline std::size_t SegmentsForDeviation(float deviation, float tolerance) {
  if (!(deviation > 0.0f) || !(tolerance > 0.0f)) {
    return 1;
  }
  const float n = std::sqrt(deviation / (4.0f * tolerance));
  if (!(n > 1.0f)) {
    return 1;
  }
  // Capped because the input is layout output, which is attacker-influenced:
  // a curve with a 10^7-pixel control point must cost a bounded number of
  // segments, not a bounded-only-by-memory number. Beyond this cap the error is
  // sub-pixel anyway for any curve that intersects a real surface.
  constexpr float kMaxSegments = 512.0f;
  return static_cast<std::size_t>(std::min(std::ceil(n), kMaxSegments));
}

inline std::size_t QuadSegments(FloatPoint p0, FloatPoint p1, FloatPoint p2, float tolerance) {
  // Second difference of the control polygon; |B(t) - chord| peaks at |d| / 4.
  const float dx = p0.x - 2.0f * p1.x + p2.x;
  const float dy = p0.y - 2.0f * p1.y + p2.y;
  return SegmentsForDeviation(Hypotenuse(dx, dy), tolerance);
}

inline std::size_t CubicSegments(FloatPoint p0, FloatPoint p1, FloatPoint p2, FloatPoint p3,
                                 float tolerance) {
  // |B(t) - chord| = t(1-t) * |(1-t)u + t v|, so max(|u|, |v|) / 4 bounds it.
  const float ux = 3.0f * p1.x - 2.0f * p0.x - p3.x;
  const float uy = 3.0f * p1.y - 2.0f * p0.y - p3.y;
  const float vx = 3.0f * p2.x - p0.x - 2.0f * p3.x;
  const float vy = 3.0f * p2.y - p0.y - 2.0f * p3.y;
  return SegmentsForDeviation(std::max(Hypotenuse(ux, uy), Hypotenuse(vx, vy)), tolerance);
}

inline FloatPoint QuadAt(FloatPoint p0, FloatPoint p1, FloatPoint p2, float t) {
  const float s = 1.0f - t;
  const float a = s * s;
  const float b = 2.0f * s * t;
  const float c = t * t;
  return FloatPoint{a * p0.x + b * p1.x + c * p2.x, a * p0.y + b * p1.y + c * p2.y};
}

inline FloatPoint CubicAt(FloatPoint p0, FloatPoint p1, FloatPoint p2, FloatPoint p3, float t) {
  const float s = 1.0f - t;
  const float a = s * s * s;
  const float b = 3.0f * s * s * t;
  const float c = 3.0f * s * t * t;
  const float d = t * t * t;
  return FloatPoint{a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                    a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

}  // namespace detail

// Points are transformed on the way in, not on the way out, and that ordering
// is the whole reason the transform is a parameter here rather than something
// the caller bakes into the path.
//
// The tolerance is a *device-space* distance. Flattening in user space and
// transforming the polyline afterwards would measure the error before a scale
// was applied, so a curve under `transform: scale(10)` would come out ten times
// too coarse — visibly faceted, at exactly the zoom level where somebody is
// looking closely.
template <typename LineSink>
void FlattenPath(const Path& path, const AffineTransform& transform, float tolerance,
                 LineSink& sink) {
  const std::span<const PathVerb> verbs = path.Verbs();
  const std::span<const FloatPoint> points = path.Points();

  std::size_t point_index = 0;
  FloatPoint current{};
  bool open = false;

  const auto next_point = [&](std::size_t& index) {
    return transform.MapPoint(points[index++]);
  };

  for (const PathVerb verb : verbs) {
    switch (verb) {
      case PathVerb::Move: {
        if (open) {
          sink.EndContour(false);
        }
        current = next_point(point_index);
        sink.BeginContour(current);
        open = true;
        break;
      }
      case PathVerb::Line: {
        current = next_point(point_index);
        sink.LineTo(current);
        break;
      }
      case PathVerb::Quad: {
        const FloatPoint control = next_point(point_index);
        const FloatPoint end = next_point(point_index);
        const std::size_t steps = detail::QuadSegments(current, control, end, tolerance);
        for (std::size_t i = 1; i < steps; ++i) {
          const float t = static_cast<float>(i) / static_cast<float>(steps);
          sink.LineTo(detail::QuadAt(current, control, end, t));
        }
        sink.LineTo(end);
        current = end;
        break;
      }
      case PathVerb::Cubic: {
        const FloatPoint c1 = next_point(point_index);
        const FloatPoint c2 = next_point(point_index);
        const FloatPoint end = next_point(point_index);
        const std::size_t steps = detail::CubicSegments(current, c1, c2, end, tolerance);
        for (std::size_t i = 1; i < steps; ++i) {
          const float t = static_cast<float>(i) / static_cast<float>(steps);
          sink.LineTo(detail::CubicAt(current, c1, c2, end, t));
        }
        sink.LineTo(end);
        current = end;
        break;
      }
      case PathVerb::Close: {
        if (open) {
          sink.EndContour(true);
          open = false;
        }
        break;
      }
    }
  }

  if (open) {
    sink.EndContour(false);
  }
}

template <typename LineSink>
void FlattenPath(const Path& path, float tolerance, LineSink& sink) {
  FlattenPath(path, AffineTransform{}, tolerance, sink);
}

}  // namespace microbrowser::gfx

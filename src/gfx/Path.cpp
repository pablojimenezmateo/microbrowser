#include "gfx/Path.h"

#include <algorithm>
#include <cmath>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool IsFinite(FloatPoint p) {
  return std::isfinite(p.x) && std::isfinite(p.y);
}

void CountRejection() {
  AddPerformanceCounter(PerfCounterId::GfxPathNonFiniteRejections);
}

// The circle-to-cubic magic number: the cubic control offset that approximates a
// quarter circle to within 0.02% of the radius. 4/3 * (sqrt(2) - 1).
constexpr float kCircleControl = 0.5522847498307933f;

}  // namespace

bool Path::EnsureContour(FloatPoint start) {
  if (has_contour_) {
    return true;
  }
  MoveTo(start);
  return false;
}

void Path::MoveTo(FloatPoint p) {
  if (!IsFinite(p)) {
    CountRejection();
    return;
  }
  verbs_.push_back(PathVerb::Move);
  points_.push_back(p);
  contour_start_ = p;
  has_contour_ = true;
}

void Path::LineTo(FloatPoint p) {
  if (!IsFinite(p)) {
    CountRejection();
    return;
  }
  if (!EnsureContour(p)) {
    return;
  }
  verbs_.push_back(PathVerb::Line);
  points_.push_back(p);
}

void Path::QuadTo(FloatPoint control, FloatPoint end) {
  if (!IsFinite(control) || !IsFinite(end)) {
    CountRejection();
    return;
  }
  EnsureContour(control);
  verbs_.push_back(PathVerb::Quad);
  points_.push_back(control);
  points_.push_back(end);
}

void Path::CubicTo(FloatPoint control1, FloatPoint control2, FloatPoint end) {
  if (!IsFinite(control1) || !IsFinite(control2) || !IsFinite(end)) {
    CountRejection();
    return;
  }
  EnsureContour(control1);
  verbs_.push_back(PathVerb::Cubic);
  points_.push_back(control1);
  points_.push_back(control2);
  points_.push_back(end);
}

void Path::Close() {
  if (!has_contour_) {
    return;
  }
  verbs_.push_back(PathVerb::Close);
  has_contour_ = false;
}

void Path::AddRect(const FloatRect& rect) {
  if (rect.IsEmpty()) {
    return;
  }
  MoveTo(FloatPoint{rect.x, rect.y});
  LineTo(FloatPoint{rect.Right(), rect.y});
  LineTo(FloatPoint{rect.Right(), rect.Bottom()});
  LineTo(FloatPoint{rect.x, rect.Bottom()});
  Close();
}

void Path::AddRoundedRect(const FloatRect& rect, float top_left, float top_right,
                          float bottom_right, float bottom_left) {
  if (rect.IsEmpty()) {
    return;
  }
  const auto sanitize = [](float radius) {
    return std::isfinite(radius) && radius > 0.0f ? radius : 0.0f;
  };
  float tl = sanitize(top_left);
  float tr = sanitize(top_right);
  float br = sanitize(bottom_right);
  float bl = sanitize(bottom_left);

  // CSS 3 Backgrounds §5.5: if adjacent radii would overlap, scale every radius
  // by the smallest factor that removes the overlap. Applying it uniformly (not
  // per side) is what keeps the corners looking like one family.
  float scale = 1.0f;
  const auto limit = [&scale](float extent, float a, float b) {
    const float sum = a + b;
    if (sum > extent && sum > 0.0f) {
      scale = std::min(scale, extent / sum);
    }
  };
  limit(rect.width, tl, tr);
  limit(rect.width, bl, br);
  limit(rect.height, tl, bl);
  limit(rect.height, tr, br);
  if (scale < 1.0f) {
    tl *= scale;
    tr *= scale;
    br *= scale;
    bl *= scale;
  }

  const float left = rect.x;
  const float top = rect.y;
  const float right = rect.Right();
  const float bottom = rect.Bottom();
  const float k = kCircleControl;

  MoveTo(FloatPoint{left + tl, top});
  LineTo(FloatPoint{right - tr, top});
  if (tr > 0.0f) {
    CubicTo(FloatPoint{right - tr + tr * k, top}, FloatPoint{right, top + tr - tr * k},
            FloatPoint{right, top + tr});
  }
  LineTo(FloatPoint{right, bottom - br});
  if (br > 0.0f) {
    CubicTo(FloatPoint{right, bottom - br + br * k}, FloatPoint{right - br + br * k, bottom},
            FloatPoint{right - br, bottom});
  }
  LineTo(FloatPoint{left + bl, bottom});
  if (bl > 0.0f) {
    CubicTo(FloatPoint{left + bl - bl * k, bottom}, FloatPoint{left, bottom - bl + bl * k},
            FloatPoint{left, bottom - bl});
  }
  LineTo(FloatPoint{left, top + tl});
  if (tl > 0.0f) {
    CubicTo(FloatPoint{left, top + tl - tl * k}, FloatPoint{left + tl - tl * k, top},
            FloatPoint{left + tl, top});
  }
  Close();
}

void Path::AddEllipse(const FloatRect& bounds) {
  if (bounds.IsEmpty()) {
    return;
  }
  const float rx = bounds.width * 0.5f;
  const float ry = bounds.height * 0.5f;
  const float cx = bounds.x + rx;
  const float cy = bounds.y + ry;
  const float ox = rx * kCircleControl;
  const float oy = ry * kCircleControl;

  MoveTo(FloatPoint{cx, bounds.y});
  CubicTo(FloatPoint{cx + ox, bounds.y}, FloatPoint{bounds.Right(), cy - oy},
          FloatPoint{bounds.Right(), cy});
  CubicTo(FloatPoint{bounds.Right(), cy + oy}, FloatPoint{cx + ox, bounds.Bottom()},
          FloatPoint{cx, bounds.Bottom()});
  CubicTo(FloatPoint{cx - ox, bounds.Bottom()}, FloatPoint{bounds.x, cy + oy},
          FloatPoint{bounds.x, cy});
  CubicTo(FloatPoint{bounds.x, cy - oy}, FloatPoint{cx - ox, bounds.y}, FloatPoint{cx, bounds.y});
  Close();
}

void Path::Clear() {
  verbs_.clear();
  points_.clear();
  contour_start_ = FloatPoint{};
  has_contour_ = false;
}

FloatPoint Path::CurrentPoint() const {
  if (points_.empty()) {
    return FloatPoint{};
  }
  // After Close the pen sits at the contour start, not at the last point fed
  // in — that is where a `z` leaves it in SVG, and where a stroker must begin
  // the next segment.
  return has_contour_ ? points_.back() : contour_start_;
}

FloatRect Path::ControlBounds() const {
  if (points_.empty()) {
    return FloatRect{};
  }
  float min_x = points_[0].x;
  float min_y = points_[0].y;
  float max_x = min_x;
  float max_y = min_y;
  for (const FloatPoint& p : points_) {
    min_x = std::min(min_x, p.x);
    min_y = std::min(min_y, p.y);
    max_x = std::max(max_x, p.x);
    max_y = std::max(max_y, p.y);
  }
  return FloatRect{min_x, min_y, max_x - min_x, max_y - min_y};
}

}  // namespace microbrowser::gfx

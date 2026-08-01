#include "gfx/AffineTransform.h"

#include <algorithm>
#include <cmath>

namespace microbrowser::gfx {

AffineTransform AffineTransform::Rotation(float radians) {
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  return AffineTransform{cosine, sine, -sine, cosine, 0.0f, 0.0f};
}

FloatRect AffineTransform::MapRect(const FloatRect& rect) const {
  const FloatPoint corners[4] = {
      MapPoint(FloatPoint{rect.x, rect.y}),
      MapPoint(FloatPoint{rect.Right(), rect.y}),
      MapPoint(FloatPoint{rect.Right(), rect.Bottom()}),
      MapPoint(FloatPoint{rect.x, rect.Bottom()}),
  };
  float min_x = corners[0].x;
  float min_y = corners[0].y;
  float max_x = min_x;
  float max_y = min_y;
  for (const FloatPoint& corner : corners) {
    min_x = std::min(min_x, corner.x);
    min_y = std::min(min_y, corner.y);
    max_x = std::max(max_x, corner.x);
    max_y = std::max(max_y, corner.y);
  }
  return FloatRect{min_x, min_y, max_x - min_x, max_y - min_y};
}

AffineTransform AffineTransform::Then(const AffineTransform& next) const {
  // next * this, written out. Applying `this` first means its columns are what
  // `next` acts on.
  return AffineTransform{next.a_ * a_ + next.c_ * b_,
                         next.b_ * a_ + next.d_ * b_,
                         next.a_ * c_ + next.c_ * d_,
                         next.b_ * c_ + next.d_ * d_,
                         next.a_ * e_ + next.c_ * f_ + next.e_,
                         next.b_ * e_ + next.d_ * f_ + next.f_};
}

std::optional<AffineTransform> AffineTransform::Inverted() const {
  const float determinant = Determinant();
  if (!std::isfinite(determinant) || determinant == 0.0f) {
    return std::nullopt;
  }
  const float inverse = 1.0f / determinant;
  return AffineTransform{d_ * inverse,
                         -b_ * inverse,
                         -c_ * inverse,
                         a_ * inverse,
                         (c_ * f_ - d_ * e_) * inverse,
                         (b_ * e_ - a_ * f_) * inverse};
}

float AffineTransform::MaximumScale() const {
  // Larger singular value of [[a c] [b d]], from the closed form for a 2x2:
  // the eigenvalues of M^T M are (t +/- sqrt(t^2 - 4 det^2)) / 2 with
  // t = a^2 + b^2 + c^2 + d^2. Using the closed form rather than the common
  // sqrt(max(column norms)) approximation matters for a rotated non-uniform
  // scale, where the approximation under-reports and a curve is then flattened
  // too coarsely along its longest axis.
  const float trace = a_ * a_ + b_ * b_ + c_ * c_ + d_ * d_;
  const float determinant = Determinant();
  const float discriminant = std::max(trace * trace - 4.0f * determinant * determinant, 0.0f);
  const float largest = (trace + std::sqrt(discriminant)) * 0.5f;
  return std::sqrt(std::max(largest, 0.0f));
}

}  // namespace microbrowser::gfx

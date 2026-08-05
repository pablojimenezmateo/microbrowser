#pragma once

#include <optional>

#include "gfx/Geometry.h"

namespace microbrowser::gfx {

// A 2D affine transform, stored in the six-value form CSS and SVG both use:
//
//     matrix(a, b, c, d, e, f)  ->  | a c e |     x' = a*x + c*y + e
//                                   | b d f |     y' = b*x + d*y + f
//                                   | 0 0 1 |
//
// The same order as `transform: matrix(...)`, so a CSS value maps to a
// constructor without a transposition step nobody would get right twice.
//
// Affine only. Perspective needs a third row, and a browser needs it eventually
// for `transform: perspective()`, but a 3x3 that is affine in every use is a
// third of the arithmetic wasted on every point of every glyph. When
// perspective arrives it arrives as its own type.
class AffineTransform {
 public:
  constexpr AffineTransform() = default;
  constexpr AffineTransform(float a, float b, float c, float d, float e, float f)
      : a_(a), b_(b), c_(c), d_(d), e_(e), f_(f) {}

  static constexpr AffineTransform Translation(float dx, float dy) {
    return AffineTransform{1.0f, 0.0f, 0.0f, 1.0f, dx, dy};
  }
  static constexpr AffineTransform Scaling(float sx, float sy) {
    return AffineTransform{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
  }
  static AffineTransform Rotation(float radians);

  constexpr float A() const { return a_; }
  constexpr float B() const { return b_; }
  constexpr float C() const { return c_; }
  constexpr float D() const { return d_; }
  constexpr float E() const { return e_; }
  constexpr float F() const { return f_; }

  // Whether the transform only moves things: the linear part is the identity. What
  // it decides is whether a rectangle stays a rectangle, which is the question a
  // clip, a rect fill and an image blit each have to ask before taking their fast
  // path -- so it is asked once, here, rather than reinvented three times.
  constexpr bool IsTranslationOnly() const {
    return a_ == 1.0f && b_ == 0.0f && c_ == 0.0f && d_ == 1.0f;
  }

  constexpr bool IsIdentity() const {
    return a_ == 1.0f && b_ == 0.0f && c_ == 0.0f && d_ == 1.0f && e_ == 0.0f && f_ == 0.0f;
  }

  constexpr FloatPoint MapPoint(FloatPoint p) const {
    return FloatPoint{a_ * p.x + c_ * p.y + e_, b_ * p.x + d_ * p.y + f_};
  }

  // Axis-aligned bounds of the mapped rectangle. Under a rotation the mapped
  // shape is not a rectangle, so this is the box that contains it — which is
  // what a clip or a damage rect wants, and is never smaller than the truth.
  FloatRect MapRect(const FloatRect& rect) const;

  // `this`, then `next`. Reading order matches the order the transforms are
  // applied, which is the opposite of the order the matrices multiply in — and
  // is the source of most transform bugs, so the name says which one this is.
  AffineTransform Then(const AffineTransform& next) const;

  constexpr float Determinant() const { return a_ * d_ - b_ * c_; }

  // Nullopt when the transform collapses the plane (a zero scale on either
  // axis), which is not an error — it is a shape with no area, and the caller
  // that wanted to un-map a point has to decide what that means.
  std::optional<AffineTransform> Inverted() const;

  // Largest factor by which the transform can stretch a unit vector: the larger
  // singular value. Curve flattening needs it, because a tolerance is a
  // *device-space* distance and a curve subdivided before a 10x scale is
  // subdivided ten times too coarsely.
  float MaximumScale() const;

  friend constexpr bool operator==(const AffineTransform&, const AffineTransform&) = default;

 private:
  float a_ = 1.0f;
  float b_ = 0.0f;
  float c_ = 0.0f;
  float d_ = 1.0f;
  float e_ = 0.0f;
  float f_ = 0.0f;
};

static_assert(sizeof(AffineTransform) == 24, "one transform per stacking context; keep it flat");

}  // namespace microbrowser::gfx

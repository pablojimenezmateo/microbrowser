#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "css/Length.h"
#include "gfx/AffineTransform.h"
#include "gfx/Geometry.h"

namespace microbrowser::css {

// `transform`, kept as the operations the author wrote rather than as a matrix.
//
// ADR 0014 §4. The reason it is not collapsed to a matrix at computed-value time is
// `translate(50%, 0)`: a percentage in a translation resolves against **the box's
// own border-box size**, which the cascade does not know and layout has not decided
// yet. A matrix here would either be wrong for every percentage or would need a size
// the resolver cannot have.
//
// It is also what `transition` will need. Interpolating two transforms means
// interpolating matching *operations*: `rotate(0deg)` to `rotate(90deg)` is a
// rotation, and interpolating those two matrices component-wise gives something that
// is not a rotation at all. Keeping the operations keeps that possible.
struct TransformOperation {
  enum class Kind : std::uint8_t {
    Translate,  // lengths, percentages against the box's own border box
    Scale,      // numbers
    Rotate,     // radians
    Skew,       // radians, as angles rather than as the tangents they become
    Matrix,     // the six components, in the order `matrix()` writes them
  };

  Kind kind = Kind::Translate;
  // Only `Translate` uses lengths; every other operation is numbers. Two fields
  // rather than a union, because a union here would be six bytes saved against one
  // more way to read the wrong member.
  Length length_x;
  Length length_y;
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  float d = 0.0f;
  float e = 0.0f;
  float f = 0.0f;

  friend bool operator==(const TransformOperation&, const TransformOperation&) = default;
};

// The list, in the order the author wrote it, which is the order it applies in:
// `translate(10px) rotate(45deg)` rotates about the translated origin, and the
// reverse spelling does something visibly different.
struct TransformList {
  std::vector<TransformOperation> operations;

  bool IsNone() const { return operations.empty(); }

  // The matrix, resolved against a box. `size` is the border box -- what a
  // percentage translation is a fraction of -- `origin` is the already-resolved
  // origin in the box's own coordinates, and `font_size` is what an `em` in a
  // translation means.
  //
  // The origin is applied here rather than at the call site, because
  // "translate to the origin, apply the list, translate back" applied twice by two
  // callers who each assumed the other had not is a bug that looks like a wrong
  // origin rather than like a double application.
  gfx::AffineTransform ToMatrix(gfx::FloatSize size, gfx::FloatPoint origin,
                                float font_size) const {
    gfx::AffineTransform result;
    for (const TransformOperation& operation : operations) {
      result = Apply(operation, size, font_size).Then(result);
    }
    return gfx::AffineTransform::Translation(-origin.x, -origin.y)
        .Then(result)
        .Then(gfx::AffineTransform::Translation(origin.x, origin.y));
  }

  friend bool operator==(const TransformList&, const TransformList&) = default;

 private:
  static gfx::AffineTransform Apply(const TransformOperation& operation, gfx::FloatSize size,
                                    float font_size) {
    switch (operation.kind) {
      case TransformOperation::Kind::Translate:
        return gfx::AffineTransform::Translation(operation.length_x.Used(size.width, font_size),
                                                operation.length_y.Used(size.height, font_size));
      case TransformOperation::Kind::Scale:
        return gfx::AffineTransform::Scaling(operation.a, operation.b);
      case TransformOperation::Kind::Rotate:
        return gfx::AffineTransform::Rotation(operation.a);
      case TransformOperation::Kind::Skew:
        // A shear, and the tangent of the angle is the whole of it. Written as one
        // matrix rather than composed from two, because `skew(a, b)` is one matrix
        // and the product of two single-axis skews is a different one.
        return gfx::AffineTransform{1.0f, std::tan(operation.b), std::tan(operation.a), 1.0f,
                                    0.0f, 0.0f};
      case TransformOperation::Kind::Matrix:
        return gfx::AffineTransform{operation.a, operation.b, operation.c,
                                    operation.d, operation.e, operation.f};
    }
    return {};
  }
};

}  // namespace microbrowser::css

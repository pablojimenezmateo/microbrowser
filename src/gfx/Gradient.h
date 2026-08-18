#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "gfx/AffineTransform.h"
#include "gfx/Color.h"
#include "gfx/Geometry.h"
#include "gfx/Image.h"

namespace microbrowser::gfx {

// A paint source that is not one colour: a gradient, or an image repeated over the plane.
//
// Here rather than in the canvas code because it is the same object CSS `linear-gradient` and
// `background-repeat` will want, and because the *evaluation* has to happen inside the span blitter --
// a caller that produced a colour per pixel from outside would be an indirect call per pixel.
//
// **Everything is defined in the space the page wrote it in, and the inverse transform brings a
// device pixel back to that space.** That is the only construction that is exact: a linear gradient
// under a skew is still a linear gradient in user space, and transforming the two endpoints instead
// would be right for a translation and wrong for everything else. The specification agrees -- it says
// gradient coordinates are in the coordinate space at the time of *filling*, which is what the stored
// inverse records.
struct ColorStop {
  float offset = 0.0f;
  Color color = Color::Rgba(0, 0, 0, 0);
};

class Paint {
 public:
  enum class Kind : std::uint8_t { Linear, Radial, Conic, Pattern };
  enum class Repeat : std::uint8_t { Both, X, Y, None };

  static Paint Linear(float x0, float y0, float x1, float y1);
  // Two circles, which is what the specification's radial gradient actually is -- the common
  // "one circle" case is the degenerate one where the first has radius zero.
  static Paint Radial(float x0, float y0, float r0, float x1, float y1, float r1);
  static Paint Conic(float angle, float cx, float cy);
  static Paint Pattern(std::shared_ptr<const Image> image, Repeat repeat);

  // Stops in any order; the evaluator sorts them once. Out-of-range offsets are refused rather than
  // clamped, because the specification makes that a throw at the call site.
  void AddStop(float offset, Color color);
  bool HasStops() const { return !stops_.empty(); }

  // The transform in force when the fill happens. Stored inverted, because every pixel needs the
  // inverse and inverting per pixel is a division a page can drive.
  void SetTransform(const AffineTransform& transform);

  Kind Which() const { return kind_; }

  // The colour at a device pixel centre. Transparent black when the gradient is degenerate, which is
  // the specification's answer for a radial gradient with no defined position for a point.
  Color At(float device_x, float device_y) const;

 private:
  Color Sample(float t) const;

  Kind kind_ = Kind::Linear;
  Repeat repeat_ = Repeat::Both;
  float x0_ = 0.0f;
  float y0_ = 0.0f;
  float r0_ = 0.0f;
  float x1_ = 0.0f;
  float y1_ = 0.0f;
  float r1_ = 0.0f;
  std::vector<ColorStop> stops_;
  std::shared_ptr<const Image> image_;
  AffineTransform inverse_;
  bool invertible_ = true;
  mutable bool sorted_ = false;
};

}  // namespace microbrowser::gfx

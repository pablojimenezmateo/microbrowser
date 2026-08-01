#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace microbrowser::gfx {

// A decoded image: tightly packed non-premultiplied ARGB8888, the same layout a
// Canvas uses, so drawing one is a blend rather than a conversion.
//
// Non-premultiplied to match Color and Canvas. Premultiplying on decode would
// be faster to blit and would lose precision on exactly the low-alpha pixels
// that get composited more than once — the same argument Color.h makes, and it
// has to be the same answer in both places or the two disagree at the seam.
class Image {
 public:
  Image() = default;

  // Nullopt-shaped: an image with no pixels is invalid, and every decoder path
  // returns one rather than a half-populated object.
  bool IsValid() const { return width_ > 0 && height_ > 0 && !pixels_.empty(); }
  int Width() const { return width_; }
  int Height() const { return height_; }
  IntSize Size() const { return IntSize{width_, height_}; }
  IntRect Bounds() const { return IntRect{0, 0, width_, height_}; }

  std::span<const std::uint32_t> Pixels() const { return pixels_; }
  const std::uint32_t* Row(int y) const;

  // True when no pixel has an alpha below 255. Worth knowing once at decode
  // time: an opaque image blits with a copy instead of a blend, and the
  // difference is the same order as the SIMD blitter bought.
  bool IsOpaque() const { return opaque_; }

  // Takes ownership of exactly width * height pixels. Returns false, and leaves
  // the image invalid, if the span is the wrong size.
  bool Adopt(int width, int height, std::vector<std::uint32_t> pixels);

 private:
  int width_ = 0;
  int height_ = 0;
  bool opaque_ = true;
  std::vector<std::uint32_t> pixels_;
};

}  // namespace microbrowser::gfx

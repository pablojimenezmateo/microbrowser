#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace microbrowser::gfx {

// A CPU-side ARGB8888 surface plus a clip stack. This is the whole output
// target of the rasterizer.
//
// Deliberately not in this class, and deliberately not coming later:
//
//   * No SDL, no window, no texture. Canvas produces pixels in memory; getting
//     them onto a screen is platform's job. (Architecture invariant: gfx must
//     not include SDL.)
//   * No damage tracking. What changed is a property of the *frame*, not of the
//     surface — see gfx/DirtyRegion.h, which the compositor owns.
//   * No drawing verbs beyond fills. Paths, glyphs, and images arrive as
//     separate rasterizer components that write through a Canvas, not as more
//     Canvas methods. That is what keeps this class from becoming the god
//     object every 2D library eventually grows.
//
// Pixels are tightly packed: stride == width. Sub-rect views would let callers
// hold a pointer across a Resize, so drawing into a region is expressed with
// the clip stack instead.
class Canvas {
 public:
  Canvas() = default;
  Canvas(int width, int height);

  // Reallocates when the size actually changes; contents are undefined
  // afterward either way. A non-positive extent yields an empty canvas rather
  // than a throw — window managers really do report a zero-size client area
  // during minimize.
  void Resize(int width, int height);

  int Width() const { return width_; }
  int Height() const { return height_; }
  IntRect Bounds() const { return IntRect{0, 0, width_, height_}; }
  bool IsEmpty() const { return width_ <= 0 || height_ <= 0; }

  std::span<const std::uint32_t> Pixels() const { return pixels_; }
  std::size_t StrideBytes() const { return static_cast<std::size_t>(width_) * sizeof(std::uint32_t); }

  // Row pointers are invalidated by Resize. Out-of-range `y` returns nullptr
  // rather than being undefined; the rasterizer clips first, so a null return
  // is a bug signal, not a routine case.
  const std::uint32_t* Row(int y) const;
  std::uint32_t* Row(int y);

  // Clip stack. PushClip intersects with the current clip, so a nested clip can
  // only ever shrink the drawable area — a child cannot escape its parent.
  void PushClip(const IntRect& rect);
  void PopClip();
  void ResetClip();
  IntRect Clip() const { return clip_stack_.empty() ? Bounds() : clip_stack_.back(); }
  std::size_t ClipDepth() const { return clip_stack_.size(); }

  // Overwrites the whole surface, ignoring the clip. This is the frame-start
  // wipe, not a drawing operation.
  void Clear(Color color);

  // Source-over fill of `rect` intersected with the current clip.
  void FillRect(const IntRect& rect, Color color);

 private:
  int width_ = 0;
  int height_ = 0;
  std::vector<std::uint32_t> pixels_;
  std::vector<IntRect> clip_stack_;
};

}  // namespace microbrowser::gfx

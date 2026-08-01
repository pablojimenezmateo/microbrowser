#pragma once

#include <algorithm>
#include <cstdint>

namespace microbrowser::gfx {

// Two coordinate spaces, deliberately distinct types.
//
//   Float* — CSS/layout space. Fractional by nature (percentages, em, sub-pixel
//            positioning), so rounding to integers must happen exactly once, at
//            the paint boundary, and be visible in the code that does it.
//   Int*   — device-pixel space. What the rasterizer and the presenter speak.
//
// Keeping them separate types is what stops the "which space is this rect in?"
// bug class that plagues layout code. Conversion is explicit: RoundToPixels.

struct FloatPoint {
  float x = 0.0f;
  float y = 0.0f;

  friend constexpr bool operator==(FloatPoint, FloatPoint) = default;
};

struct FloatSize {
  float width = 0.0f;
  float height = 0.0f;

  friend constexpr bool operator==(FloatSize, FloatSize) = default;
};

struct IntPoint {
  int x = 0;
  int y = 0;

  friend constexpr bool operator==(IntPoint, IntPoint) = default;
};

struct IntSize {
  int width = 0;
  int height = 0;

  constexpr bool IsEmpty() const { return width <= 0 || height <= 0; }

  friend constexpr bool operator==(IntSize, IntSize) = default;
};

// Half-open rectangle: covers x in [x, x + width), y in [y, y + height).
// Half-open is what makes Intersect/Union/adjacency arithmetic total — a closed
// rect needs a special case for zero extent everywhere.
struct IntRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  constexpr int Left() const { return x; }
  constexpr int Top() const { return y; }
  constexpr int Right() const { return x + width; }
  constexpr int Bottom() const { return y + height; }

  constexpr bool IsEmpty() const { return width <= 0 || height <= 0; }
  constexpr IntSize Size() const { return IntSize{width, height}; }

  // Pixel count, as a 64-bit value: a 8192x8192 surface overflows nothing here,
  // but `width * height` in int is one zoom level away from doing so.
  constexpr std::int64_t Area() const {
    if (IsEmpty()) {
      return 0;
    }
    return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
  }

  constexpr bool Contains(IntPoint p) const {
    return p.x >= x && p.x < Right() && p.y >= y && p.y < Bottom();
  }

  constexpr bool Intersects(const IntRect& other) const {
    return !IsEmpty() && !other.IsEmpty() && x < other.Right() && other.x < Right() &&
           y < other.Bottom() && other.y < Bottom();
  }

  // Empty when the rects do not overlap. The result is normalized (never
  // negative extent), so callers can test IsEmpty() rather than the signs.
  constexpr IntRect Intersected(const IntRect& other) const {
    const int left = std::max(x, other.x);
    const int top = std::max(y, other.y);
    const int right = std::min(Right(), other.Right());
    const int bottom = std::min(Bottom(), other.Bottom());
    if (right <= left || bottom <= top) {
      return IntRect{};
    }
    return IntRect{left, top, right - left, bottom - top};
  }

  // Smallest rect containing both. An empty operand is absorbed rather than
  // dragging the result to the origin.
  constexpr IntRect United(const IntRect& other) const {
    if (IsEmpty()) {
      return other;
    }
    if (other.IsEmpty()) {
      return *this;
    }
    const int left = std::min(x, other.x);
    const int top = std::min(y, other.y);
    const int right = std::max(Right(), other.Right());
    const int bottom = std::max(Bottom(), other.Bottom());
    return IntRect{left, top, right - left, bottom - top};
  }

  constexpr IntRect Translated(int dx, int dy) const {
    return IntRect{x + dx, y + dy, width, height};
  }

  // Grow (positive) or shrink (negative) on every side. Collapses to empty
  // rather than inverting when the inset exceeds the extent.
  constexpr IntRect Inflated(int amount) const {
    const int w = width + 2 * amount;
    const int h = height + 2 * amount;
    if (w <= 0 || h <= 0) {
      return IntRect{};
    }
    return IntRect{x - amount, y - amount, w, h};
  }

  friend constexpr bool operator==(const IntRect&, const IntRect&) = default;
};

struct FloatRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  constexpr float Right() const { return x + width; }
  constexpr float Bottom() const { return y + height; }
  constexpr bool IsEmpty() const { return width <= 0.0f || height <= 0.0f; }

  friend constexpr bool operator==(const FloatRect&, const FloatRect&) = default;
};

// The device-coordinate range every IntRect is required to stay inside.
//
// Not a screen size — it is the bound that makes rect arithmetic total. With
// every edge inside +/- 1e9, `x + width` and `y + height` cannot overflow an
// int, so Right(), Bottom(), Intersected() and United() need no saturation in
// their inner loops. Everything that produces an IntRect from something
// unbounded is responsible for landing inside it: EnclosingIntRect saturates
// there, and the IPC decoder rejects a rect that does not.
//
// The value is a round number rather than a power of two because it is a
// contract stated in the two places that enforce it, and 1e9 is the largest
// float that survives `static_cast<int>` exactly.
inline constexpr int kMaxDeviceCoordinate = 1000000000;

// Saturating float-to-int, the one sanctioned narrowing in the project.
//
// A NaN or an out-of-range float reaching `static_cast<int>` is undefined
// behavior, and layout arithmetic produces both: a percentage of an unresolved
// width is a NaN, and a stroke width arriving from a compromised renderer is
// whatever it wants to be. NaN maps to zero, which is the only answer that
// keeps a shape from moving somewhere arbitrary.
int SaturateFloatToInt(float value);

// True when every edge of `r` is inside the device range, so that Right() and
// Bottom() cannot overflow. Computed in 64 bits, because the whole point is
// that the 32-bit form of the question may not be asked safely.
constexpr bool IsWithinDeviceRange(const IntRect& r) {
  const std::int64_t left = r.x;
  const std::int64_t top = r.y;
  const std::int64_t right = left + r.width;
  const std::int64_t bottom = top + r.height;
  const std::int64_t limit = kMaxDeviceCoordinate;
  return left >= -limit && left <= limit && top >= -limit && top <= limit && right >= -limit &&
         right <= limit && bottom >= -limit && bottom <= limit;
}

// The one sanctioned layout-space to device-space conversion: the smallest
// pixel rect that fully covers `r`. Enclosing (not rounding) is what keeps a
// repaint from leaving a one-pixel seam of stale content along an edge.
//
// The result always satisfies IsWithinDeviceRange.
IntRect EnclosingIntRect(const FloatRect& r);

// Object-size budgets. These types are copied per draw command, per layout box,
// and eventually per DOM node; growth here is multiplied by the document size,
// so it is a compile error rather than a review comment.
// See docs/adr/0002-growth-budgets.md.
static_assert(sizeof(IntRect) == 16, "IntRect must stay 4 ints");
static_assert(sizeof(FloatRect) == 16, "FloatRect must stay 4 floats");
static_assert(sizeof(IntPoint) == 8, "IntPoint must stay 2 ints");

}  // namespace microbrowser::gfx

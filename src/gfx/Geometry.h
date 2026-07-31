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

// The one sanctioned layout-space to device-space conversion: the smallest
// pixel rect that fully covers `r`. Enclosing (not rounding) is what keeps a
// repaint from leaving a one-pixel seam of stale content along an edge.
IntRect EnclosingIntRect(const FloatRect& r);

// Object-size budgets. These types are copied per draw command, per layout box,
// and eventually per DOM node; growth here is multiplied by the document size,
// so it is a compile error rather than a review comment.
// See docs/adr/0002-growth-budgets.md.
static_assert(sizeof(IntRect) == 16, "IntRect must stay 4 ints");
static_assert(sizeof(FloatRect) == 16, "FloatRect must stay 4 floats");
static_assert(sizeof(IntPoint) == 8, "IntPoint must stay 2 ints");

}  // namespace microbrowser::gfx

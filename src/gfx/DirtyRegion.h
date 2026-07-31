#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gfx/Geometry.h"

namespace microbrowser::gfx {

// The set of device pixels that changed this frame, kept as a small bounded
// list of rects rather than an exact region.
//
// Exactness is not the goal: over-reporting costs a few redundant pixels,
// under-reporting leaves stale content on screen, and an exact region algebra
// costs more CPU than the fills it would save. So the policy is deliberately
// crude and deliberately bounded — merge overlapping-or-adjacent rects on add,
// and once the list would exceed kMaxRects, collapse to the bounding box.
//
// Whether the frame should promote to a full repaint anyway (many small rects
// covering most of the surface) is a *policy* question that depends on upload
// cost, so it lives in app/DirtyRegionPolicy rather than here. This type only
// accumulates.
class DirtyRegion {
 public:
  // Above this, tracking individual rects costs more than repainting the union.
  // Chosen to match the number of independently-scrolling/animating areas a
  // realistic page has on screen at once; revisit with measurements, not taste.
  static constexpr std::size_t kMaxRects = 16;

  void Clear() { rects_.clear(); }
  bool IsEmpty() const { return rects_.empty(); }
  std::size_t Count() const { return rects_.size(); }
  const std::vector<IntRect>& Rects() const { return rects_; }

  // No-op for an empty rect, so callers may add unconditionally.
  void Add(const IntRect& rect);

  // Union of everything added. Empty when nothing was.
  IntRect BoundingBox() const;

  // Total pixels across the tracked rects. Overlap is possible after a merge
  // widens a rect, so this is an upper bound on work, not an exact count.
  std::int64_t Area() const;

  // Clip every tracked rect to `bounds`, dropping those that fall outside.
  // Called on resize, when rects from the old surface size are still queued.
  void IntersectWith(const IntRect& bounds);

 private:
  std::vector<IntRect> rects_;
};

}  // namespace microbrowser::gfx

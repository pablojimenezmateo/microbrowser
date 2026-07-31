#include "gfx/DirtyRegion.h"

#include <algorithm>

namespace microbrowser::gfx {

namespace {

// Two rects are worth merging when they overlap, or when they merely touch —
// uniting adjacent rects costs no extra pixels and stops a column of
// single-row damage (what a scrolling text run produces) from consuming the
// whole rect budget.
bool ShouldMerge(const IntRect& a, const IntRect& b) {
  return a.Inflated(1).Intersects(b.Inflated(1));
}

}  // namespace

void DirtyRegion::Add(const IntRect& rect) {
  if (rect.IsEmpty()) {
    return;
  }

  IntRect pending = rect;
  // Absorb every rect the new one now touches, then repeat: a union can reach
  // rects the original did not, and stopping after one pass would leave two
  // mergeable rects in the list. Bounded by kMaxRects, so this terminates
  // quickly and cannot become quadratic in any meaningful sense.
  bool merged_any = true;
  while (merged_any) {
    merged_any = false;
    for (std::size_t i = 0; i < rects_.size();) {
      if (ShouldMerge(pending, rects_[i])) {
        pending = pending.United(rects_[i]);
        rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(i));
        merged_any = true;
        continue;
      }
      ++i;
    }
  }

  rects_.push_back(pending);

  if (rects_.size() > kMaxRects) {
    const IntRect box = BoundingBox();
    rects_.clear();
    rects_.push_back(box);
  }
}

IntRect DirtyRegion::BoundingBox() const {
  IntRect box;
  for (const IntRect& rect : rects_) {
    box = box.United(rect);
  }
  return box;
}

std::int64_t DirtyRegion::Area() const {
  std::int64_t total = 0;
  for (const IntRect& rect : rects_) {
    total += rect.Area();
  }
  return total;
}

void DirtyRegion::IntersectWith(const IntRect& bounds) {
  for (IntRect& rect : rects_) {
    rect = rect.Intersected(bounds);
  }
  rects_.erase(std::remove_if(rects_.begin(), rects_.end(),
                              [](const IntRect& rect) { return rect.IsEmpty(); }),
               rects_.end());
}

}  // namespace microbrowser::gfx

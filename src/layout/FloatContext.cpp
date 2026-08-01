#include "layout/FloatContext.h"

#include <algorithm>
#include <cmath>

namespace microbrowser::layout {

namespace {

// Half-open vertical overlap. A float ending exactly where a line begins does
// not narrow it — otherwise a float and the line below it fight over one row of
// pixels forever.
bool OverlapsVertically(const gfx::FloatRect& rect, float y, float height) {
  const float bottom = y + std::max(height, 0.0f);
  return rect.y < bottom && y < rect.Bottom();
}

}  // namespace

FloatContext::Band FloatContext::BandAt(float y, float height, float left, float right) const {
  Band band{left, right};
  for (const Placed& placed : floats_) {
    if (!OverlapsVertically(placed.rect, y, height)) {
      continue;
    }
    if (placed.side == css::Float::Left) {
      band.left = std::max(band.left, placed.rect.Right());
    } else {
      band.right = std::min(band.right, placed.rect.x);
    }
  }
  // A band narrower than nothing is nothing, not a negative width that later
  // arithmetic would treat as room.
  band.right = std::max(band.right, band.left);
  return band;
}

gfx::FloatRect FloatContext::Place(css::Float side, float width, float height, float y, float left,
                                   float right) {
  if (side == css::Float::None) {
    return gfx::FloatRect{};
  }
  const float clamped_width = std::clamp(width, 0.0f, std::max(0.0f, right - left));
  const float clamped_height = std::max(height, 0.0f);

  // A float may not be higher than any float placed before it. Enforced here
  // rather than by the caller, because the caller is walking a box tree and
  // does not know what else is in this formatting context.
  for (const Placed& placed : floats_) {
    y = std::max(y, placed.rect.y);
  }

  // Descend past floats it cannot fit beside. Bounded by the number of floats:
  // each iteration moves `y` to a strictly lower float bottom, and there are
  // finitely many.
  for (std::size_t attempt = 0; attempt <= floats_.size(); ++attempt) {
    const Band band = BandAt(y, clamped_height, left, right);
    if (band.Width() >= clamped_width) {
      const float x = side == css::Float::Left ? band.left : band.right - clamped_width;
      const gfx::FloatRect rect{x, y, clamped_width, clamped_height};
      floats_.push_back(Placed{rect, side});
      return rect;
    }

    // The next candidate is the lowest float bottom strictly below `y`.
    float next = std::numeric_limits<float>::infinity();
    for (const Placed& placed : floats_) {
      if (placed.rect.Bottom() > y) {
        next = std::min(next, placed.rect.Bottom());
      }
    }
    if (!std::isfinite(next)) {
      break;
    }
    y = next;
  }

  // Nothing fits anywhere: it goes at the left or right edge and overflows,
  // which is what a float wider than its containing block does.
  const float x = side == css::Float::Left ? left : std::max(left, right - clamped_width);
  const gfx::FloatRect rect{x, y, clamped_width, clamped_height};
  floats_.push_back(Placed{rect, side});
  return rect;
}

float FloatContext::ClearanceBelow(css::Clear which, float y) const {
  if (which == css::Clear::None) {
    return y;
  }
  float lowest = y;
  for (const Placed& placed : floats_) {
    const bool matches = which == css::Clear::Both ||
                         (which == css::Clear::Left && placed.side == css::Float::Left) ||
                         (which == css::Clear::Right && placed.side == css::Float::Right);
    if (matches) {
      lowest = std::max(lowest, placed.rect.Bottom());
    }
  }
  return lowest;
}

float FloatContext::LowestBottom() const {
  float lowest = 0.0f;
  for (const Placed& placed : floats_) {
    lowest = std::max(lowest, placed.rect.Bottom());
  }
  return lowest;
}

}  // namespace microbrowser::layout

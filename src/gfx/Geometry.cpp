#include "gfx/Geometry.h"

#include <cmath>
#include <limits>

namespace microbrowser::gfx {

namespace {

// Clamp to int range before the cast. A NaN or huge float reaching
// static_cast<int> is undefined behavior, and layout arithmetic produces both
// (a percentage of an unresolved width, an overflowing zoom).
int SaturateToInt(float value) {
  if (std::isnan(value)) {
    return 0;
  }
  constexpr float kMin = -1.0e9f;
  constexpr float kMax = 1.0e9f;
  if (value <= kMin) {
    return static_cast<int>(kMin);
  }
  if (value >= kMax) {
    return static_cast<int>(kMax);
  }
  return static_cast<int>(value);
}

}  // namespace

IntRect EnclosingIntRect(const FloatRect& r) {
  if (r.IsEmpty()) {
    return IntRect{};
  }
  const int left = SaturateToInt(std::floor(r.x));
  const int top = SaturateToInt(std::floor(r.y));
  const int right = SaturateToInt(std::ceil(r.Right()));
  const int bottom = SaturateToInt(std::ceil(r.Bottom()));
  if (right <= left || bottom <= top) {
    return IntRect{};
  }
  return IntRect{left, top, right - left, bottom - top};
}

}  // namespace microbrowser::gfx

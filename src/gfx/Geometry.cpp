#include "gfx/Geometry.h"

#include <cmath>

namespace microbrowser::gfx {

int SaturateFloatToInt(float value) {
  if (std::isnan(value)) {
    return 0;
  }
  constexpr float kMin = -static_cast<float>(kMaxDeviceCoordinate);
  constexpr float kMax = static_cast<float>(kMaxDeviceCoordinate);
  if (value <= kMin) {
    return -kMaxDeviceCoordinate;
  }
  if (value >= kMax) {
    return kMaxDeviceCoordinate;
  }
  return static_cast<int>(value);
}

IntRect EnclosingIntRect(const FloatRect& r) {
  if (r.IsEmpty()) {
    return IntRect{};
  }
  const int left = SaturateFloatToInt(std::floor(r.x));
  const int top = SaturateFloatToInt(std::floor(r.y));
  const int right = SaturateFloatToInt(std::ceil(r.Right()));
  const int bottom = SaturateFloatToInt(std::ceil(r.Bottom()));
  if (right <= left || bottom <= top) {
    return IntRect{};
  }
  // Both edges are inside +/- kMaxDeviceCoordinate, so the extent below is at
  // most twice that and the resulting rect satisfies IsWithinDeviceRange.
  return IntRect{left, top, right - left, bottom - top};
}

}  // namespace microbrowser::gfx

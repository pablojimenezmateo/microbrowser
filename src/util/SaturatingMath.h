#pragma once

#include <limits>
#include <type_traits>

namespace microbrowser::util {

// Saturating unsigned subtraction: returns `a - b`, or 0 when `b > a`, so the
// result never wraps around the modulus. Use for derived durations, one-based
// interval arithmetic, and any unsigned bookkeeping where an underflow would
// otherwise produce an enormous bogus value.
template <typename T>
constexpr T SaturatingSub(T a, T b) {
  static_assert(std::is_unsigned_v<T>, "SaturatingSub is for unsigned types");
  return a > b ? static_cast<T>(a - b) : T{0};
}

// Saturating unsigned addition: returns `a + b`, clamped to the type maximum
// instead of wrapping on overflow.
template <typename T>
constexpr T SaturatingAdd(T a, T b) {
  static_assert(std::is_unsigned_v<T>, "SaturatingAdd is for unsigned types");
  const T limit = std::numeric_limits<T>::max();
  return b > static_cast<T>(limit - a) ? limit : static_cast<T>(a + b);
}

// Saturating signed addition. Use when an attacker-controlled delta is applied
// to a signed timestamp or coordinate and wraparound would turn "far future"
// into "already expired".
template <typename T>
constexpr T SaturatingSignedAdd(T a, T b) {
  static_assert(std::is_signed_v<T>, "SaturatingSignedAdd is for signed types");
  if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
    return std::numeric_limits<T>::max();
  }
  if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) {
    return std::numeric_limits<T>::min();
  }
  return static_cast<T>(a + b);
}

}  // namespace microbrowser::util

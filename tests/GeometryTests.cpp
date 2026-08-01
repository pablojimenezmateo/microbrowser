#include <cmath>
#include <limits>
#include <vector>

#include "TestSupport.h"
#include "gfx/Geometry.h"

namespace microbrowser::tests {

using gfx::EnclosingIntRect;
using gfx::FloatRect;
using gfx::IntPoint;
using gfx::IntRect;
using gfx::IsWithinDeviceRange;
using gfx::kMaxDeviceCoordinate;
using gfx::SaturateFloatToInt;

void RegisterGeometryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Geometry/EmptyRectsAbsorbInUnion", [] {
    const IntRect empty;
    const IntRect real{10, 20, 30, 40};
    Expect(empty.United(real) == real, "empty ∪ r must be r, not a rect stretched to the origin");
    Expect(real.United(empty) == real, "r ∪ empty must be r");
  });

  AddTest(tests, "Geometry/IntersectionIsNormalized", [] {
    const IntRect a{0, 0, 10, 10};
    const IntRect b{20, 20, 5, 5};
    Expect(a.Intersected(b).IsEmpty(), "disjoint rects must intersect to empty");
    Expect(!a.Intersected(b).Contains(IntPoint{0, 0}),
           "an empty intersection must contain nothing");

    const IntRect c{5, 5, 10, 10};
    Expect(a.Intersected(c) == (IntRect{5, 5, 5, 5}), "overlapping intersection is wrong");
  });

  AddTest(tests, "Geometry/HalfOpenContainment", [] {
    const IntRect r{0, 0, 4, 4};
    Expect(r.Contains(IntPoint{0, 0}), "top-left corner is inside a half-open rect");
    Expect(r.Contains(IntPoint{3, 3}), "last inside pixel must be contained");
    Expect(!r.Contains(IntPoint{4, 4}), "bottom-right is exclusive in a half-open rect");
  });

  AddTest(tests, "Geometry/AreaDoesNotOverflowInt", [] {
    const IntRect huge{0, 0, 100000, 100000};
    ExpectEqInt(huge.Area(), 10000000000LL, "area must be computed in 64 bits");
  });

  AddTest(tests, "Geometry/InflateCollapsesRatherThanInverts", [] {
    const IntRect r{10, 10, 4, 4};
    Expect(r.Inflated(-10).IsEmpty(), "over-shrinking must collapse to empty, not invert");
    Expect(r.Inflated(1) == (IntRect{9, 9, 6, 6}), "inflate grows on every side");
  });

  AddTest(tests, "Geometry/EnclosingCoversFractionalEdges", [] {
    // The seam case: a box at x=10.2 with width 5.6 spans pixels 10..15 (right
    // edge 15.8). Rounding instead of enclosing would drop the last column and
    // leave a stale pixel line on repaint.
    const IntRect enclosing = EnclosingIntRect(FloatRect{10.2f, 20.7f, 5.6f, 3.1f});
    ExpectEqInt(enclosing.x, 10, "left must floor");
    ExpectEqInt(enclosing.y, 20, "top must floor");
    ExpectEqInt(enclosing.Right(), 16, "right must ceil");
    ExpectEqInt(enclosing.Bottom(), 24, "bottom must ceil");
  });

  AddTest(tests, "Geometry/EnclosingRejectsNonFinite", [] {
    // This assertion used to read `X || !X`, which is true for every X and
    // could not fail. It survived because the *call* was the point — a NaN
    // reaching static_cast<int> is undefined behavior, so the test still meant
    // something under UBSan and nothing at all otherwise.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const IntRect from_nan = EnclosingIntRect(FloatRect{nan, nan, 10.0f, 10.0f});
    Expect(from_nan.IsEmpty(),
           "a rect with no defined position encloses nothing; placing it at the origin would "
           "put a repaint somewhere arbitrary rather than nowhere");
    Expect(EnclosingIntRect(FloatRect{0.0f, 0.0f, 0.0f, 0.0f}).IsEmpty(),
           "a zero-extent rect encloses nothing");
  });

  AddTest(tests, "Geometry/SaturatingConversionIsTotal", [] {
    ExpectEqInt(SaturateFloatToInt(std::numeric_limits<float>::quiet_NaN()), 0,
                "NaN has no position; zero is the only answer that does not move a shape");
    ExpectEqInt(SaturateFloatToInt(std::numeric_limits<float>::infinity()),
                kMaxDeviceCoordinate, "infinity clamps to the top of the device range");
    ExpectEqInt(SaturateFloatToInt(-std::numeric_limits<float>::infinity()),
                -kMaxDeviceCoordinate, "and negative infinity to the bottom");
    ExpectEqInt(SaturateFloatToInt(3.4e38f), kMaxDeviceCoordinate, "so does any huge finite float");
    ExpectEqInt(SaturateFloatToInt(-7.5f), -7, "an ordinary value truncates toward zero");
  });

  // The invariant the whole rect vocabulary rests on: inside this range,
  // Right() and Bottom() cannot overflow, so no intersection or union needs
  // saturating arithmetic in its inner loop.
  AddTest(tests, "Geometry/DeviceRangeIsWhatMakesRectArithmeticTotal", [] {
    Expect(IsWithinDeviceRange(IntRect{0, 0, 1280, 800}), "an ordinary rect is in range");
    Expect(IsWithinDeviceRange(IntRect{}), "and so is an empty one");
    Expect(IsWithinDeviceRange(IntRect{-kMaxDeviceCoordinate, -kMaxDeviceCoordinate,
                                       2 * kMaxDeviceCoordinate, 2 * kMaxDeviceCoordinate}),
           "the widest legal rect spans the whole range exactly");
    Expect(!IsWithinDeviceRange(IntRect{2000000000, 0, 2000000000, 10}),
           "a rect whose right edge overflows an int is not a rect");
    Expect(!IsWithinDeviceRange(IntRect{0, 2000000000, 10, 2000000000}),
           "on either axis");
    Expect(!IsWithinDeviceRange(IntRect{kMaxDeviceCoordinate, 0, 1, 1}),
           "one past the range is out of range");
  });

  AddTest(tests, "Geometry/EnclosingAlwaysProducesARectThatCanBeUsed", [] {
    // The producer half of the contract the IPC decoder enforces on the other
    // side: if this could emit an out-of-range rect, the decoder would be
    // rejecting frames the engine legitimately sent.
    const float huge = 3.0e38f;
    for (const FloatRect& input : {FloatRect{-huge, -huge, huge, huge},
                                   FloatRect{huge, huge, huge, huge},
                                   FloatRect{-huge, 0.0f, huge * 2.0f, 10.0f},
                                   FloatRect{0.0f, 0.0f, 1e9f, 1e9f}}) {
      Expect(IsWithinDeviceRange(EnclosingIntRect(input)),
             "every rect this produces must satisfy the range the decoder checks for");
    }
  });

  AddTest(tests, "Geometry/InflatingByAHugeAmountDoesNotOverflow", [] {
    // Found by the IPC fuzzer. DisplayList::Bounds inflates by a stroke's
    // outset, and a stroke width arrives from a renderer that may be
    // compromised, so `width + 2 * amount` in int is a signed overflow with an
    // attacker's hand on it.
    const gfx::IntRect wide{-gfx::kMaxDeviceCoordinate, -gfx::kMaxDeviceCoordinate,
                            gfx::kMaxDeviceCoordinate, gfx::kMaxDeviceCoordinate};
    for (const int amount : {1, 1000, gfx::kMaxDeviceCoordinate,
                             std::numeric_limits<int>::max()}) {
      const gfx::IntRect grown = wide.Inflated(amount);
      Expect(gfx::IsWithinDeviceRange(grown),
             "the result stays inside the range that makes rect arithmetic total");
    }
  });

  AddTest(tests, "Geometry/InflatingStillGrowsAndShrinks", [] {
    // The overflow fix must not have turned Inflated into a clamp that ignores
    // small amounts, which is the way a bounds check quietly breaks the thing
    // it was protecting.
    const gfx::IntRect rect{10, 20, 30, 40};
    const gfx::IntRect grown = rect.Inflated(5);
    Expect(grown == gfx::IntRect{5, 15, 40, 50}, "growing by five on every side");
    const gfx::IntRect shrunk = rect.Inflated(-5);
    Expect(shrunk == gfx::IntRect{15, 25, 20, 30}, "and shrinking by five");
    Expect(rect.Inflated(-100).IsEmpty(),
           "an inset past the extent collapses to empty rather than inverting");
  });
}

}  // namespace microbrowser::tests

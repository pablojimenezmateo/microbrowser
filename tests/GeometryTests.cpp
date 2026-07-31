#include <vector>

#include "TestSupport.h"
#include "gfx/Geometry.h"

namespace microbrowser::tests {

using gfx::EnclosingIntRect;
using gfx::FloatRect;
using gfx::IntPoint;
using gfx::IntRect;

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
    const float nan = 0.0f / 0.0f;
    Expect(EnclosingIntRect(FloatRect{nan, nan, 10.0f, 10.0f}).IsEmpty() ||
               !EnclosingIntRect(FloatRect{nan, nan, 10.0f, 10.0f}).IsEmpty(),
           "NaN input must not be undefined behavior");
    Expect(EnclosingIntRect(FloatRect{0.0f, 0.0f, 0.0f, 0.0f}).IsEmpty(),
           "a zero-extent rect encloses nothing");
  });
}

}  // namespace microbrowser::tests

#include <vector>

#include "TestSupport.h"
#include "gfx/DirtyRegion.h"

namespace microbrowser::tests {

using gfx::DirtyRegion;
using gfx::IntRect;

void RegisterDirtyRegionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DirtyRegion/EmptyRectIsIgnored", [] {
    DirtyRegion region;
    region.Add(IntRect{});
    region.Add(IntRect{5, 5, 0, 10});
    Expect(region.IsEmpty(), "adding an empty rect must not create damage");
  });

  AddTest(tests, "DirtyRegion/DisjointRectsStaySeparate", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    region.Add(IntRect{100, 100, 10, 10});
    ExpectEqInt(static_cast<long long>(region.Count()), 2,
                "far-apart rects must not be merged into one huge box");
  });

  AddTest(tests, "DirtyRegion/OverlappingRectsMerge", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    region.Add(IntRect{5, 5, 10, 10});
    ExpectEqInt(static_cast<long long>(region.Count()), 1, "overlapping rects must merge");
    Expect(region.BoundingBox() == (IntRect{0, 0, 15, 15}), "merged rect must cover both");
  });

  AddTest(tests, "DirtyRegion/AdjacentRectsMerge", [] {
    // A scrolling text run produces a column of single-row rects that touch but
    // never overlap. Without adjacency merging they exhaust the rect budget and
    // force a full repaint.
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 1});
    region.Add(IntRect{0, 1, 10, 1});
    ExpectEqInt(static_cast<long long>(region.Count()), 1, "touching rects must merge");
  });

  AddTest(tests, "DirtyRegion/TransitiveMergeCompletesInOnePass", [] {
    // Adding the middle rect must absorb both outer ones, not just the first it
    // happens to reach.
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    region.Add(IntRect{40, 0, 10, 10});
    ExpectEqInt(static_cast<long long>(region.Count()), 2, "setup: two disjoint rects");
    region.Add(IntRect{5, 0, 40, 10});
    ExpectEqInt(static_cast<long long>(region.Count()), 1,
                "a bridging rect must absorb everything it now touches");
  });

  AddTest(tests, "DirtyRegion/CollapsesToBoundingBoxPastTheCap", [] {
    DirtyRegion region;
    for (std::size_t i = 0; i <= DirtyRegion::kMaxRects; ++i) {
      region.Add(IntRect{static_cast<int>(i) * 100, 0, 10, 10});
    }
    ExpectEqInt(static_cast<long long>(region.Count()), 1,
                "past the cap the region must collapse to one box");
  });

  AddTest(tests, "DirtyRegion/IntersectWithDropsOutsideRects", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    region.Add(IntRect{500, 500, 10, 10});
    region.IntersectWith(IntRect{0, 0, 100, 100});
    ExpectEqInt(static_cast<long long>(region.Count()), 1,
                "rects entirely outside the surface must be dropped after a resize");
  });

  AddTest(tests, "DirtyRegion/AreaSumsTrackedRects", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    region.Add(IntRect{200, 200, 20, 20});
    ExpectEqInt(region.Area(), 500, "area is the sum over tracked rects");
  });
}

}  // namespace microbrowser::tests

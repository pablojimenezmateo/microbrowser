#include <vector>

#include "TestSupport.h"
#include "app/DirtyRegionPolicy.h"

namespace microbrowser::tests {

using app::AnalyzeDirtyRegion;
using app::DirtyRegionAnalysis;
using app::ShouldPromoteToFullRepaint;
using gfx::DirtyRegion;
using gfx::IntRect;

void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DirtyRegionPolicy/NothingDirtyNeverPromotes", [] {
    DirtyRegion region;
    const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(region, IntRect{0, 0, 100, 100});
    Expect(!ShouldPromoteToFullRepaint(analysis),
           "an empty region must not trigger a full repaint; that is the most expensive "
           "possible wrong answer");
  });

  AddTest(tests, "DirtyRegionPolicy/SmallDamageStaysPartial", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(region, IntRect{0, 0, 1000, 1000});
    Expect(!ShouldPromoteToFullRepaint(analysis), "1% coverage must stay a partial repaint");
  });

  AddTest(tests, "DirtyRegionPolicy/HighCoveragePromotes", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 100, 90});
    const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(region, IntRect{0, 0, 100, 100});
    Expect(ShouldPromoteToFullRepaint(analysis),
           "90% coverage is cheaper as one full upload than as a partial one");
  });

  AddTest(tests, "DirtyRegionPolicy/ManyRectsPromoteRegardlessOfArea", [] {
    DirtyRegion region;
    // Far apart so they never merge; tiny so coverage stays negligible.
    for (std::size_t i = 0; i <= app::kFullRepaintRectCountThreshold; ++i) {
      region.Add(IntRect{static_cast<int>(i) * 50, 0, 1, 1});
    }
    const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(region, IntRect{0, 0, 4000, 4000});
    Expect(analysis.coverage < 0.01f, "setup: coverage must be negligible");
    Expect(ShouldPromoteToFullRepaint(analysis),
           "per-upload overhead dominates past the rect-count threshold");
  });

  AddTest(tests, "DirtyRegionPolicy/ZeroAreaSurfaceDoesNotDivide", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 10, 10});
    const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(region, IntRect{});
    ExpectEqInt(static_cast<long long>(analysis.coverage * 100.0f), 0,
                "a zero-area surface must report zero coverage, not a division result");
  });

  AddTest(tests, "DirtyRegionPolicy/CoverageIsClampedToOne", [] {
    DirtyRegion region;
    region.Add(IntRect{0, 0, 1000, 1000});
    const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(region, IntRect{0, 0, 10, 10});
    Expect(analysis.coverage <= 1.0f, "coverage must never exceed 1.0");
  });
}

}  // namespace microbrowser::tests

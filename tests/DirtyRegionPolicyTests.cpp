#include <vector>

#include "TestSupport.h"
#include "app/DirtyRegionPolicy.h"
#include "gfx/Surface.h"

namespace microbrowser::tests {

using app::AnalyzeDirtyRegion;
using app::DirtyRegionAnalysis;
using app::ShouldPromoteToFullRepaint;
using app::SurfaceDamageTracker;
using gfx::DirtyRegion;
using gfx::IntRect;
using gfx::IntSize;
using gfx::Surface;
using gfx::SurfacePlacement;
using gfx::SurfaceRegistry;

namespace {

// True when some tracked rect covers `rect`. A DirtyRegion merges as it grows,
// so a test must ask "is this repainted" rather than "is this one of the rects"
// -- the merge is allowed to answer with a bigger rectangle and still be right.
bool Covers(const DirtyRegion& region, const IntRect& rect) {
  for (const IntRect& tracked : region.Rects()) {
    if (tracked.Intersected(rect) == rect) {
      return true;
    }
  }
  return false;
}

}  // namespace

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

  // ADR 0013's requirement, in the form it is stated: a surface rectangle is
  // damaged every frame while it is playing and not at all when it is paused.
  //
  // The display list is byte-identical across all of these, which is the whole
  // point -- if the diff could see it, none of this would need to exist.
  AddTest(tests, "SurfaceDamage/APlayingSurfaceIsDamagedEveryFrame", [] {
    SurfaceRegistry registry;
    Surface* surface = registry.Create(IntSize{16, 16});
    Expect(surface != nullptr, "the registry allocates a surface");
    const std::vector<std::uint32_t> frame(16 * 16, 0xFF00FF00u);
    Expect(surface->Update(frame), "and it accepts a frame of the right size");

    const std::vector<SurfacePlacement> placements{{surface->Id(), IntRect{10, 10, 16, 16}}};
    SurfaceDamageTracker tracker;

    DirtyRegion first;
    tracker.AddSurfaceDamage(placements, registry, first);
    ExpectEqInt(static_cast<long long>(first.Count()), 1,
                "the first placement is damaged: none of it is on screen yet");

    DirtyRegion paused;
    tracker.AddSurfaceDamage(placements, registry, paused);
    ExpectEqInt(static_cast<long long>(paused.Count()), 0,
                "no new frame, same place: a paused video costs nothing");

    Expect(surface->Update(frame), "a new frame arrives");
    DirtyRegion playing;
    tracker.AddSurfaceDamage(placements, registry, playing);
    ExpectEqInt(static_cast<long long>(playing.Count()), 1,
                "which damages the rectangle even though the display list did not change");
  });

  // A surface that moves leaves pixels behind. Damaging only the new rectangle
  // is the stale-pixel bug this is the whole defence against, and it is
  // invisible in any test that only ever places a surface once.
  AddTest(tests, "SurfaceDamage/AMovedSurfaceDamagesBothRectangles", [] {
    SurfaceRegistry registry;
    Surface* surface = registry.Create(IntSize{8, 8});
    Expect(surface != nullptr, "a surface");

    SurfaceDamageTracker tracker;
    DirtyRegion seed;
    tracker.AddSurfaceDamage({{surface->Id(), IntRect{0, 0, 8, 8}}}, registry, seed);

    DirtyRegion moved;
    tracker.AddSurfaceDamage({{surface->Id(), IntRect{100, 100, 8, 8}}}, registry, moved);
    Expect(Covers(moved, IntRect{0, 0, 8, 8}), "the rectangle it left is repainted");
    Expect(Covers(moved, IntRect{100, 100, 8, 8}), "and so is the one it moved to");
  });

  AddTest(tests, "SurfaceDamage/ASurfaceThatDisappearsDamagesWhatItLeft", [] {
    SurfaceRegistry registry;
    Surface* surface = registry.Create(IntSize{8, 8});
    Expect(surface != nullptr, "a surface");

    SurfaceDamageTracker tracker;
    DirtyRegion seed;
    tracker.AddSurfaceDamage({{surface->Id(), IntRect{5, 5, 8, 8}}}, registry, seed);

    DirtyRegion gone;
    tracker.AddSurfaceDamage({}, registry, gone);
    Expect(Covers(gone, IntRect{5, 5, 8, 8}),
           "the page underneath has to be repainted, or the last frame stays on screen forever");
    ExpectEqInt(static_cast<long long>(tracker.TrackedCount()), 0, "and it stops being tracked");
  });

  // A placement naming a surface that does not exist is reachable two ways: a
  // frame that arrived a turn before its surface, and a renderer that made the
  // id up. Both must be harmless, which is the entire reason a surface is
  // addressed by a name looked up in a map rather than by an index.
  AddTest(tests, "SurfaceDamage/AnUnknownSurfaceIdIsHarmless", [] {
    SurfaceRegistry registry;
    SurfaceDamageTracker tracker;

    DirtyRegion first;
    tracker.AddSurfaceDamage({{4242u, IntRect{0, 0, 4, 4}}}, registry, first);
    ExpectEqInt(static_cast<long long>(first.Count()), 1,
                "the rectangle is still damaged the first time: something may be under it");

    DirtyRegion second;
    tracker.AddSurfaceDamage({{4242u, IntRect{0, 0, 4, 4}}}, registry, second);
    ExpectEqInt(static_cast<long long>(second.Count()), 0,
                "and never again, because a surface that does not exist never produces a frame");
  });
}

}  // namespace microbrowser::tests

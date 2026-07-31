#pragma once

#include <cstddef>

#include "gfx/DirtyRegion.h"
#include "gfx/Geometry.h"

namespace microbrowser::app {

// Should this partial frame be promoted to a full repaint?
//
// Partial repaint is not free. Each dirty rect costs a separate texture upload
// with its own driver round trip, and rasterizing N rects re-walks the display
// list N times. Past a certain coverage, one full upload beats many partial
// ones even though it touches more pixels — so the policy trades pixels for
// call count, deliberately.
//
// Separate from gfx::DirtyRegion because this is a *policy* about upload cost,
// and gfx knows nothing about uploads. Keeping the two apart is what lets the
// thresholds be tuned against measurements without touching the rasterizer.

struct DirtyRegionAnalysis {
  std::size_t rect_count = 0;
  // Fraction of the surface the dirty rects cover, 0.0 to 1.0. May exceed the
  // true fraction when rects overlap after a merge; that errs toward promoting,
  // which is the safe direction.
  float coverage = 0.0f;

  friend bool operator==(const DirtyRegionAnalysis&, const DirtyRegionAnalysis&) = default;
};

// Coverage above which a full repaint is cheaper than the equivalent partial
// uploads. Provisional: set from reasoning about upload overhead, to be
// replaced by a measured value once the perf harness has a scroll scenario.
inline constexpr float kFullRepaintCoverageThreshold = 0.6f;

// Above this many rects the per-upload overhead dominates regardless of how
// little area they cover.
inline constexpr std::size_t kFullRepaintRectCountThreshold = 12;

DirtyRegionAnalysis AnalyzeDirtyRegion(const gfx::DirtyRegion& dirty, const gfx::IntRect& surface);

bool ShouldPromoteToFullRepaint(const DirtyRegionAnalysis& analysis);

}  // namespace microbrowser::app

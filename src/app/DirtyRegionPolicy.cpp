#include "app/DirtyRegionPolicy.h"

#include <cstdint>

namespace microbrowser::app {

DirtyRegionAnalysis AnalyzeDirtyRegion(const gfx::DirtyRegion& dirty,
                                       const gfx::IntRect& surface) {
  DirtyRegionAnalysis analysis;
  analysis.rect_count = dirty.Count();

  const std::int64_t surface_area = surface.Area();
  if (surface_area <= 0) {
    // A zero-area surface has no meaningful coverage. Report 0 rather than
    // dividing, and let the rect-count rule decide.
    return analysis;
  }

  const std::int64_t dirty_area = dirty.Area();
  analysis.coverage =
      static_cast<float>(static_cast<double>(dirty_area) / static_cast<double>(surface_area));
  if (analysis.coverage > 1.0f) {
    analysis.coverage = 1.0f;
  }
  return analysis;
}

bool ShouldPromoteToFullRepaint(const DirtyRegionAnalysis& analysis) {
  if (analysis.rect_count == 0) {
    // Nothing is dirty. "Promote" would mean repainting everything for no
    // reason, which is the single most expensive wrong answer available here.
    return false;
  }
  return analysis.rect_count > kFullRepaintRectCountThreshold ||
         analysis.coverage >= kFullRepaintCoverageThreshold;
}

}  // namespace microbrowser::app

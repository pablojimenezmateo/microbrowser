#include "app/DirtyRegionPolicy.h"

#include <cstdint>

namespace microbrowser::app {

DirtyRegionAnalysis AnalyzeDirtyRegion(const gfx::DirtyRegion& dirty,
                                       const gfx::IntRect& viewport) {
  DirtyRegionAnalysis analysis;
  analysis.rect_count = dirty.Count();

  const std::int64_t surface_area = viewport.Area();
  if (surface_area <= 0) {
    // A zero-area viewport has no meaningful coverage. Report 0 rather than
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

void SurfaceDamageTracker::AddSurfaceDamage(
    const std::vector<gfx::SurfacePlacement>& placements, const gfx::SurfaceRegistry& surfaces,
    gfx::DirtyRegion& out) {
  std::unordered_map<gfx::SurfaceId, Composited> next;
  next.reserve(placements.size());

  for (const gfx::SurfacePlacement& placement : placements) {
    const gfx::Surface* surface = surfaces.Find(placement.surface);
    // An id naming nothing is the case the registry exists to make harmless:
    // there are no pixels to composite, so there is nothing to damage. It is
    // reachable from a frame that arrived a turn before its surface, and from a
    // renderer that made the id up.
    const std::uint64_t generation = surface == nullptr ? 0 : surface->Generation();

    const auto previous = composited_.find(placement.surface);
    if (previous == composited_.end()) {
      // First time this surface is on screen. Damaged whatever its generation
      // says, because none of it has been composited yet.
      out.Add(placement.destination);
    } else {
      if (!(previous->second.rect == placement.destination)) {
        // Moved or resized: both rectangles, or the pixels it vacated stay.
        out.Add(previous->second.rect);
        out.Add(placement.destination);
      } else if (previous->second.generation != generation) {
        // A new frame in the same place. This is the playing case, and the
        // whole reason this class exists -- the display list is identical.
        out.Add(placement.destination);
      }
      // Otherwise: same rectangle, same generation. Nothing changed, so nothing
      // is damaged. A paused video costs zero here, which is the invariant.
    }
    next.emplace(placement.surface, Composited{placement.destination, generation});
  }

  // Anything composited last frame and absent from this one leaves its pixels
  // behind. Repainting the rectangle is what lets the page underneath show
  // through again.
  for (const auto& [id, was] : composited_) {
    if (next.find(id) == next.end()) {
      out.Add(was.rect);
    }
  }

  composited_ = std::move(next);
}

}  // namespace microbrowser::app

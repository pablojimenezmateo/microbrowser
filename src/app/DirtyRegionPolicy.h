#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gfx/DirtyRegion.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"

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

// `viewport` is the rectangle being painted into -- named that rather than
// `surface` since gfx::Surface came to mean something specific and adjacent.
DirtyRegionAnalysis AnalyzeDirtyRegion(const gfx::DirtyRegion& dirty, const gfx::IntRect& viewport);

bool ShouldPromoteToFullRepaint(const DirtyRegionAnalysis& analysis);

// Damage for the surface holes in a frame, which the display list diff cannot
// compute and must not try to.
//
// ADR 0013 states the requirement as "the dirty-region policy has to learn that
// a surface rectangle is damaged every frame while it is playing and not at all
// when it is paused". This is that, and the reason it belongs here rather than
// in gfx is in the same sentence: it is a policy about what to repaint, and
// paint has no way to know. Two byte-identical DrawSurfaceCommands is what a
// playing video looks like to ComputeDamage, and that answer is correct about
// the display list and useless about the screen.
//
// **"Playing" is derived, never declared.** The tracker compares each surface's
// generation counter against the one it last composited. A decoder that stops
// producing frames stops damaging the screen without anything having to
// remember to say "paused"; a decoder that stalls costs nothing rather than a
// full-rect repaint per frame of unchanged pixels. There is no is_playing flag
// to get out of sync with reality, which is the failure mode this shape exists
// to remove.
//
// Three other cases it has to get right, each of which is a stale-pixel bug:
//   - A surface placed for the first time is damaged, because there is nothing
//     of it on screen yet whatever its generation says.
//   - A surface that moved or was resized damages both rectangles, or the pixels
//     it used to occupy stay on screen forever.
//   - A surface that was on the previous frame and is not on this one damages
//     the rectangle it left behind, for the same reason.
class SurfaceDamageTracker {
 public:
  // Adds to `out` every rectangle that must be repainted because of a surface,
  // and records what was composited so the next call can tell what changed.
  //
  // Called once per presented frame, after the display-list diff and before the
  // promote-to-full decision, so that a playing video counts toward coverage
  // like any other damage.
  void AddSurfaceDamage(const std::vector<gfx::SurfacePlacement>& placements,
                        const gfx::SurfaceRegistry& surfaces, gfx::DirtyRegion& out);

  // Forgets everything. Used when the whole frame is being repainted anyway, so
  // that the next frame's comparison is against what is actually on screen.
  void Reset() { composited_.clear(); }

  std::size_t TrackedCount() const { return composited_.size(); }

 private:
  struct Composited {
    gfx::IntRect rect;
    std::uint64_t generation = 0;
  };

  std::unordered_map<gfx::SurfaceId, Composited> composited_;
};

}  // namespace microbrowser::app

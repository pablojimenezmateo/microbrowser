#include "gfx/DisplayList.h"

#include <algorithm>
#include <cmath>

#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How far a stroke can reach beyond the geometry it follows. A miter tip sits
// at half the width divided by the sine of the half angle, which the miter
// limit caps at `miter_limit * width / 2`; a square cap reaches the corner of a
// half-width square, which is sqrt(2)/2 of the width.
//
// Over-estimating here costs a slightly wider damage rect. Under-estimating
// leaves stale pixels on screen, so the rounding goes one way only.
float StrokeOutset(const StrokeStyle& style) {
  if (!std::isfinite(style.width) || style.width <= 0.0f) {
    return 0.0f;
  }
  const float miter = std::isfinite(style.miter_limit) ? std::max(style.miter_limit, 1.0f) : 1.0f;
  return style.width * 0.5f * std::max(miter, 1.5f);
}

IntRect BoundsOfPath(const Path& path) {
  return EnclosingIntRect(path.ControlBounds());
}

}  // namespace

void DisplayList::Clear() {
  // clear(), not a fresh vector: display lists are rebuilt every frame, and
  // reusing the capacity is what keeps painting off the allocator.
  commands_.clear();
  paths_.clear();
}

void DisplayList::FillRect(const IntRect& rect, Color color) {
  if (rect.IsEmpty() || color.IsFullyTransparent()) {
    return;
  }
  commands_.emplace_back(FillRectCommand{rect, color});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::PushClip(const IntRect& rect) {
  commands_.emplace_back(PushClipCommand{rect});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::PopClip() {
  commands_.emplace_back(PopClipCommand{});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::FillPath(const Path& path, Color color, FillRule rule) {
  if (path.IsEmpty() || color.IsFullyTransparent()) {
    return;
  }
  paths_.push_back(path);
  commands_.emplace_back(
      FillPathCommand{static_cast<std::uint32_t>(paths_.size() - 1), rule, color});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::StrokePath(const Path& path, const StrokeStyle& style, Color color) {
  if (path.IsEmpty() || color.IsFullyTransparent() || !std::isfinite(style.width) ||
      style.width <= 0.0f) {
    return;
  }
  paths_.push_back(path);
  commands_.emplace_back(
      StrokePathCommand{static_cast<std::uint32_t>(paths_.size() - 1), color, style});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

IntRect DisplayList::Bounds() const {
  IntRect bounds;
  for (const DisplayCommand& command : commands_) {
    if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
      bounds = bounds.United(fill->rect);
    } else if (const auto* fill_path = std::get_if<FillPathCommand>(&command)) {
      if (const Path* geometry = PathAt(fill_path->path)) {
        bounds = bounds.United(BoundsOfPath(*geometry));
      }
    } else if (const auto* stroke = std::get_if<StrokePathCommand>(&command)) {
      if (const Path* geometry = PathAt(stroke->path)) {
        // SaturateFloatToInt, not static_cast: the stroke width can arrive from a
        // compromised renderer, and ceil(1e38) reaching a raw cast is undefined
        // behavior rather than a very wide damage rect.
        const int outset = SaturateFloatToInt(std::ceil(StrokeOutset(stroke->style)));
        bounds = bounds.United(BoundsOfPath(*geometry).Inflated(outset));
      }
    }
  }
  return bounds;
}

void Execute(const DisplayList& list, Painter& painter, const IntRect& damage) {
  AddPerformanceCounter(PerfCounterId::DisplayListExecutions);

  Canvas& canvas = painter.Target();
  const IntRect region = damage.Intersected(canvas.Bounds());
  if (region.IsEmpty()) {
    return;
  }

  const std::size_t entry_depth = canvas.ClipDepth();
  canvas.PushClip(region);

  for (const DisplayCommand& command : list.Commands()) {
    if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
      canvas.FillRect(fill->rect, fill->color);
    } else if (const auto* push = std::get_if<PushClipCommand>(&command)) {
      canvas.PushClip(push->rect);
    } else if (const auto* fill_path = std::get_if<FillPathCommand>(&command)) {
      if (const Path* geometry = list.PathAt(fill_path->path)) {
        painter.FillPath(*geometry, fill_path->color, fill_path->rule);
      }
    } else if (const auto* stroke = std::get_if<StrokePathCommand>(&command)) {
      if (const Path* geometry = list.PathAt(stroke->path)) {
        painter.StrokePath(*geometry, stroke->style, stroke->color);
      }
    } else {
      // PopClip. Refuse to pop past our own damage clip: an unbalanced list
      // would otherwise widen the clip beyond the damage region and paint
      // outside it.
      if (canvas.ClipDepth() > entry_depth + 1) {
        canvas.PopClip();
      }
    }
  }

  // Restore whatever depth the caller had, regardless of how balanced the list
  // turned out to be.
  while (canvas.ClipDepth() > entry_depth) {
    canvas.PopClip();
  }
}

}  // namespace microbrowser::gfx

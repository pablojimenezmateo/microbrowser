#include "gfx/DisplayList.h"

#include "gfx/Canvas.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

void DisplayList::Clear() {
  // clear(), not a fresh vector: display lists are rebuilt every frame, and
  // reusing the capacity is what keeps painting off the allocator.
  commands_.clear();
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

IntRect DisplayList::Bounds() const {
  IntRect bounds;
  for (const DisplayCommand& command : commands_) {
    if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
      bounds = bounds.United(fill->rect);
    }
  }
  return bounds;
}

void Execute(const DisplayList& list, Canvas& canvas, const IntRect& damage) {
  AddPerformanceCounter(PerfCounterId::DisplayListExecutions);

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

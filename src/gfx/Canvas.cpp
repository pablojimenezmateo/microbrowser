#include "gfx/Canvas.h"

#include <algorithm>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

Canvas::Canvas(int width, int height) {
  Resize(width, height);
}

void Canvas::Resize(int width, int height) {
  const int clamped_width = std::max(0, width);
  const int clamped_height = std::max(0, height);
  if (clamped_width == width_ && clamped_height == height_) {
    return;
  }
  width_ = clamped_width;
  height_ = clamped_height;
  // resize() rather than assign(): the frame that follows a resize repaints in
  // full, so zeroing the new buffer would be work thrown away. The elements are
  // still value-initialized on growth, so no uninitialized memory is ever read
  // even if a caller ignores that contract.
  pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0u);
  clip_stack_.clear();
}

const std::uint32_t* Canvas::Row(int y) const {
  if (y < 0 || y >= height_ || IsEmpty()) {
    return nullptr;
  }
  return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
}

std::uint32_t* Canvas::Row(int y) {
  if (y < 0 || y >= height_ || IsEmpty()) {
    return nullptr;
  }
  return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
}

void Canvas::PushClip(const IntRect& rect) {
  AddPerformanceCounter(PerfCounterId::GfxClipPushes);
  clip_stack_.push_back(Clip().Intersected(rect));
}

void Canvas::PopClip() {
  if (!clip_stack_.empty()) {
    clip_stack_.pop_back();
  }
}

void Canvas::ResetClip() {
  clip_stack_.clear();
}

void Canvas::Clear(Color color) {
  std::fill(pixels_.begin(), pixels_.end(), color.argb);
}

void Canvas::FillRect(const IntRect& rect, Color color) {
  AddPerformanceCounter(PerfCounterId::GfxFillRectCalls);
  if (color.IsFullyTransparent()) {
    return;
  }
  const IntRect target = rect.Intersected(Clip()).Intersected(Bounds());
  if (target.IsEmpty()) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::GfxFillRectPixels,
                        static_cast<std::uint64_t>(target.Area()));

  if (color.IsOpaque()) {
    AddPerformanceCounter(PerfCounterId::GfxOpaqueFills);
    for (int y = target.Top(); y < target.Bottom(); ++y) {
      std::uint32_t* row = Row(y) + target.Left();
      std::fill(row, row + target.width, color.argb);
    }
    return;
  }

  AddPerformanceCounter(PerfCounterId::GfxBlendedFills);
  for (int y = target.Top(); y < target.Bottom(); ++y) {
    std::uint32_t* row = Row(y) + target.Left();
    for (int i = 0; i < target.width; ++i) {
      row[i] = BlendSrcOver(row[i], color);
    }
  }
}

}  // namespace microbrowser::gfx

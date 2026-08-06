#include "layout/FontTextMeasurer.h"

#include <algorithm>

#include "util/PerformanceCounters.h"

namespace microbrowser::layout {

float FontTextMeasurer::MeasureWidth(std::string_view text, const css::ComputedStyle& style,
                                     bool right_to_left) const {
  util::AddPerformanceCounter(util::PerfCounterId::LayoutTextMeasurements);
  return text_->MeasureRun(text, FontRequestFor(style), right_to_left);
}

float FontTextMeasurer::LineHeight(const css::ComputedStyle& style) const {
  if (style.line_height > 0.0f) {
    return style.line_height;
  }
  const gfx::FontMetrics metrics = text_->MetricsFor(FontRequestFor(style));
  const float natural = metrics.LineHeight();
  // A face with no usable metrics still needs a line height, or every line
  // stacks at the same y and the page is one illegible row.
  return natural > 0.0f ? natural : style.font_size * 1.2f;
}

float FontTextMeasurer::Ascent(const css::ComputedStyle& style) const {
  const gfx::FontMetrics metrics = text_->MetricsFor(FontRequestFor(style));
  if (metrics.ascent > 0.0f) {
    // Extra leading is split above and below the text, per CSS `line-height`:
    // half-leading above the ascent is what centers a line in a taller box.
    const float leading = std::max(0.0f, LineHeight(style) - metrics.LineHeight());
    return metrics.ascent + leading * 0.5f;
  }
  return style.font_size * 0.8f;
}

}  // namespace microbrowser::layout

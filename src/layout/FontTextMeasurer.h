#pragma once

#include "gfx/TextRenderer.h"
#include "layout/Box.h"

namespace microbrowser::layout {

// Measures text with the real font stack.
//
// The counterpart to FixedTextMeasurer, and the reason TextMeasurer is an
// interface at all: this one's numbers depend on which typefaces are installed,
// which is correct for a browser and useless for a test.
//
// It shares the TextRenderer that paint uses, so a run measured during layout
// is already shaped when it is drawn. Giving layout its own shaper would double
// the most expensive step in the text stack and hide it behind a layer where
// nobody would look for it.
class FontTextMeasurer : public TextMeasurer {
 public:
  explicit FontTextMeasurer(gfx::TextRenderer& text) : text_(&text) {}

  float MeasureWidth(std::string_view text, const css::ComputedStyle& style,
                     bool right_to_left = false) const override;
  float LineHeight(const css::ComputedStyle& style) const override;
  float Ascent(const css::ComputedStyle& style) const override;

 private:
  gfx::TextRenderer* text_;
};

}  // namespace microbrowser::layout

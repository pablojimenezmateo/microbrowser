#pragma once

#include "gfx/TextRenderer.h"
#include "layout/FontTextMeasurer.h"

namespace microbrowser::engine {

// Text measurement for one page: the renderer and the measurer layout asks.
// Split from Page so the coordinator does not hold members from both `gfx` and
// `layout` -- Page's fan-out lint counts only depth-one members, and one
// `PageTextContext` here is four modules on Page rather than five.
class PageTextContext {
 public:
  explicit PageTextContext(gfx::FontProvider& fonts) : text_(fonts), measurer_(text_) {}

  gfx::TextRenderer& Text() { return text_; }
  const gfx::TextRenderer& Text() const { return text_; }
  layout::FontTextMeasurer& Measurer() { return measurer_; }
  const layout::FontTextMeasurer& Measurer() const { return measurer_; }

 private:
  gfx::TextRenderer text_;
  layout::FontTextMeasurer measurer_;
};

}  // namespace microbrowser::engine

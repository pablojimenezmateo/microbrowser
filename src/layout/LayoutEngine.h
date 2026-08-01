#pragma once

#include <memory>
#include <string_view>

#include "css/StyleResolver.h"
#include "dom/Node.h"
#include "gfx/DisplayList.h"
#include "layout/Box.h"

namespace microbrowser::layout {

// Builds a box tree from a styled document and lays it out.
//
// Two phases, deliberately separate. Building the tree is where anonymous
// boxes appear and where `display: none` removes subtrees; laying it out is
// pure geometry over a tree that no longer needs the DOM. Fusing them would
// mean every layout re-decided what the boxes are, which is the change that
// makes incremental layout impossible later.
class LayoutEngine {
 public:
  LayoutEngine(const css::StyleResolver& resolver, const TextMeasurer& measurer)
      : resolver_(&resolver), measurer_(&measurer) {}

  // Builds the box tree for a document. Never null: a document with nothing
  // visible still has a root box, because "nothing to paint" and "no layout"
  // are different states and only one of them is a bug.
  std::unique_ptr<Box> BuildBoxTree(const dom::Document& document) const;

  // Lays out `root` into a viewport `width` wide. Returns the total content
  // height, which is what a scrollbar needs.
  float Layout(Box& root, float width) const;

 private:
  std::unique_ptr<Box> BuildFor(const dom::Node& node, const css::ComputedStyle& parent_style,
                                bool& produced_inline) const;
  void LayoutBlock(Box& box, float available_width, float& cursor_y) const;
  float LayoutInlineChildren(Box& box, float content_left, float content_width,
                             float start_y) const;

  const css::StyleResolver* resolver_;
  const TextMeasurer* measurer_;
};

// Turns a laid-out box tree into a display list.
//
// Separate from layout for the reason the display list exists at all: paint
// output has to be a value that can be compared, serialized and diffed, and a
// painter that walked the box tree directly would put device state back into
// the middle of the engine.
// `offset` translates everything recorded, which is how scrolling moves the
// page. Baked into the geometry rather than expressed as a transform command:
// the display list has no transform, and adding one would make every damage
// rect depend on replaying it.
void BuildDisplayList(const Box& root, gfx::DisplayList& out, gfx::FloatPoint offset = {});

}  // namespace microbrowser::layout

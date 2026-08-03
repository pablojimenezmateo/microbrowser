#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "css/StyleResolver.h"
#include "dom/Node.h"
#include "gfx/DisplayList.h"
#include "layout/Box.h"
#include "layout/FloatContext.h"

namespace microbrowser::layout {

// What each column of a table can and wants to be, in table order.
//
// Two bounds rather than one width because a table's columns are sized against
// the space available, and neither number alone answers that: the minimum is
// what a column cannot go below without its text spilling out, and the maximum
// is what it would take if nothing ever wrapped. Everything between is a
// distribution problem.
struct TableColumnWidths {
  std::vector<float> min;
  std::vector<float> max;
};

// Builds a box tree from a styled document and lays it out.
//
// Two phases, deliberately separate. Building the tree is where anonymous
// boxes appear and where `display: none` removes subtrees; laying it out is
// pure geometry over a tree that no longer needs the DOM. Fusing them would
// mean every layout re-decided what the boxes are, which is the change that
// makes incremental layout impossible later.
class LayoutEngine {
 public:
  // `images` may be null, in which case every replaced element renders as an
  // empty box of its declared size. That is what a page looks like before its
  // images arrive, so it is a state layout has to be able to express anyway.
  LayoutEngine(const css::StyleResolver& resolver, const TextMeasurer& measurer,
               const ImageProvider* images = nullptr)
      : resolver_(&resolver), measurer_(&measurer), images_(images) {}

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
  // `floats` is the formatting context this box participates in. A box that
  // establishes its own -- the root, and every float -- passes a fresh one to
  // its children, which is what keeps a float inside a sidebar from shortening
  // the lines of the article next to it.
  // `center_in_container` is the <center> rule: the containing block centres
  // its block-level children outright, whatever their margins say. It centres
  // this box's lines too, but that part is ordinary `text-align: center` and
  // inherits with it; *this* half is a property of the container, which is why
  // it is a parameter and why css::ComputedStyle::centers_block_children is
  // deliberately not inherited. Inheriting it would make every block inside a
  // <center>, however deep, re-centre itself against a container it already
  // fits exactly -- and a nested block that fits exactly must not move.
  void LayoutBlock(Box& box, float container_left, float available_width, float& cursor_y,
                   FloatContext& floats, bool center_in_container = false) const;
  float LayoutInlineChildren(Box& box, float content_left, float content_width, float start_y,
                             FloatContext& floats) const;
  float LayoutTableChildren(Box& box, float content_left, float content_width, float start_y) const;
  float LayoutTableRowGroup(Box& group, float content_left, float content_width, float start_y,
                            const std::vector<float>& columns) const;
  float LayoutTableRow(Box& row, float content_left, float content_width, float start_y,
                       const std::vector<float>& columns) const;
  // Widest this box would be if it never wrapped. Needed for shrink-to-fit,
  // which is what `float: left` with no declared width means.
  float MaxContentWidth(const Box& box) const;
  // Narrowest this box can be without its content spilling out: the widest
  // single unbreakable piece. The other half of shrink-to-fit, and what stops
  // a table column from being squeezed to nothing.
  float MinContentWidth(const Box& box) const;

  // The bounds of every column of `table`, and the width each one is actually
  // given once the table's own width is known. Separate because the first is a
  // measurement of the content and the second is a policy over it -- and
  // because the table's width is decided *from* the first, so the two cannot
  // happen in one call.
  TableColumnWidths MeasureTableColumns(const Box& table) const;
  static std::vector<float> DistributeTableColumns(const TableColumnWidths& bounds,
                                                   float table_width);
  void PlaceFloat(Box& child, float content_left, float content_width, float cursor_y,
                  FloatContext& floats) const;

  const css::StyleResolver* resolver_;
  const TextMeasurer* measurer_;
  const ImageProvider* images_;
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

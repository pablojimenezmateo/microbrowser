#pragma once

#include <map>
#include <memory>
#include <optional>
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

// A size the caller has already decided, which the box must use rather than
// derive.
//
// Both halves are optional because the two flex directions fix different ones:
// a row fixes an item's width and leaves its height to its content, a column
// does the reverse. Nothing outside flex layout sets either -- an ordinary
// block's size is a function of its style and its container, and forcing it
// would be a way to make those disagree.
struct ForcedSize {
  std::optional<float> content_width;
  std::optional<float> content_height;
};

// Moves a laid-out subtree by `(dx, dy)`.
//
// Geometry is stored in absolute coordinates -- the painter walks the tree
// without an ancestor stack -- so a box that moves takes everything under it,
// text fragments included. This is the operation TD-0001 was waiting on: the
// flex place pass, a float's second LayoutBlock, and an atomic-inline's
// place-on-line all used to re-run the whole algorithm just to change origin.
void OffsetLaidOutSubtree(Box& box, float dx, float dy);

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
                                std::uint64_t parent_style_id, bool& produced_inline) const;
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
                   FloatContext& floats, bool center_in_container = false,
                   const ForcedSize* forced = nullptr) const;
  // Lays out a flex container's children and returns the content height they
  // occupy. In its own translation unit: the algorithm is long, and it is the
  // only place in layout where children are sized against each other rather
  // than each against the container.
  //
  // `definite_main_height` is the column container's definite content height
  // when it has one (a stated height, a forced size, or a max-height that bound
  // an auto-height pass). Row containers ignore it: their main size is
  // `content_width`. Without it a column never grows or shrinks — there is no
  // free space to speak of — which is how youtube's consent dialog kept
  // `#content` at its intrinsic ~1490px inside an 896px `max-height` box.
  float LayoutFlexChildren(Box& box, float content_left, float content_width,
                           float start_y,
                           std::optional<float> definite_main_height = std::nullopt) const;
  // Positioning, in its own translation unit. Two passes and not one: an
  // absolutely positioned box is sized against the padding box of the nearest
  // positioned ancestor, and that ancestor's height is not known until its
  // in-flow children have been laid out.
  void ApplyRelativeOffset(Box& box) const;
  void LayoutAbsoluteDescendants(Box& container, const gfx::FloatRect& containing_block) const;
  void LayoutAbsoluteBox(Box& box, const gfx::FloatRect& containing_block) const;
  float LayoutInlineChildren(Box& box, float content_left, float content_width, float start_y,
                             FloatContext& floats) const;
  // `measured` carries the column bounds in when the caller already needed
  // them to decide the table's own width, and takes them out when it did not.
  float LayoutTableChildren(Box& box, float content_left, float content_width, float start_y,
                            std::optional<TableColumnWidths>& measured) const;
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
  // The measurements themselves. Separate from the two above, which are the
  // memoized entry points -- keeping the recursion pointed at the cached form
  // is the whole reason the cache helps, and calling the wrong one is the easy
  // mistake to make.
  float MeasureMaxContentWidth(const Box& box) const;
  float MeasureMinContentWidth(const Box& box) const;

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

// Where a scroll offset lives between two layouts.
//
// Keyed by element rather than by box, because the box tree is thrown away and
// rebuilt on every relayout and the offset must not be: a page that scrolls a
// menu and then changes a class on it would jump back to the top. Layout writes
// the stored offset into the box it belongs to and clamps it against the
// overflow it just measured, which is ADR 0018's "state layout consults and
// does not own".
using ScrollOffsets = std::map<const dom::Element*, gfx::FloatPoint>;

// Measures the scrollable overflow of every scroll container in `root`, applies
// the stored offsets, and clamps each one into the range its box can actually
// scroll.
//
// Run after layout and before paint. Entries in `offsets` whose element no
// longer generates a scroll container are dropped, which is what keeps the map
// bounded by the document rather than by the history of the document.
void UpdateScrollState(Box& root, ScrollOffsets& offsets);

// The largest offset `box` can be scrolled to: its scrollable overflow less
// what it already shows. Zero on both axes for a box whose content fits.
gfx::FloatPoint MaxScrollOffset(const Box& box);

// The nearest scroll container at or above `point` in the tree that can still
// move by `delta` on either axis, or null when nothing can.
//
// The chaining rule of ADR 0018 §4, and the difference between a browser that
// feels right and one that does not: a wheel inside a menu scrolls the menu
// until it reaches its end and then scrolls the page behind it.
const Box* ScrollTargetAt(const Box& root, gfx::FloatPoint document_point,
                          gfx::FloatPoint delta);

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
//
// `viewport` is the size of the scrollport the *document* scrolls in, and it is
// here for the two positions that are a function of the scroll rather than of
// the flow: a `fixed` box drops the offset entirely, and a `sticky` one is
// pinned inside its containing block against the nearest scrollport's edge.
// Both are resolved here rather than in layout because both change when nothing
// about the geometry does -- which is what "a scroll is a paint" means. A zero
// viewport leaves `bottom`/`right` sticky insets unresolved, which is the
// honest answer for a caller that has not said how big its window is.
void BuildDisplayList(const Box& root, gfx::DisplayList& out, gfx::FloatPoint offset = {},
                      gfx::FloatSize viewport = {});

}  // namespace microbrowser::layout

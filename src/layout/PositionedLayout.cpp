#include <algorithm>
#include <cstddef>

#include "layout/LayoutEngine.h"

// `position: relative`, `absolute` and `fixed`.
//
// Two passes rather than one, and it has to be two. An absolutely positioned
// box is sized against its containing block -- the padding box of the nearest
// positioned ancestor -- and that ancestor's own height is not known until its
// in-flow children have been laid out. So the flow runs first, and every
// positioned box then places the absolute descendants that belong to it.
//
// "Belong to it" is the part with a rule behind it: the walk stops at the next
// positioned box, because that box is a containing block of its own and has
// already placed its own descendants at the end of its own layout.
//
// The static position -- where a box with no `left`/`top` would have been --
// is approximated by the containing block's origin. Getting it exactly right
// means remembering where the flow would have put a box it never placed, and
// the approximation is correct for the case pages actually write: an absolute
// box with at least one offset on each axis.

namespace microbrowser::layout {

namespace {

// An inset, against the containing block extent it resolves against.
float Inset(const css::Length& length, float font_size, float extent) {
  return length.Used(extent, font_size);
}

}  // namespace

void OffsetLaidOutSubtree(Box& box, float dx, float dy) {
  if (dx == 0.0f && dy == 0.0f) {
    return;
  }
  box.Geometry().content.x += dx;
  box.Geometry().content.y += dy;
  for (TextFragment& fragment : box.MutableFragments()) {
    fragment.rect.x += dx;
    fragment.rect.y += dy;
    fragment.baseline += dy;
  }
  for (std::unique_ptr<Box>& child : box.MutableChildren()) {
    OffsetLaidOutSubtree(*child, dx, dy);
  }
}

void LayoutEngine::ApplyRelativeOffset(Box& box) const {
  const css::ComputedStyle& style = box.Style();
  if (style.position != css::Position::Relative) {
    return;
  }
  // A relative box keeps the space it had: it is *drawn* somewhere else and
  // its siblings are laid out as though it had not moved. That is the whole
  // difference from an absolute one, and it is why this runs after the flow
  // rather than instead of it.
  //
  // `left` wins over `right` and `top` over `bottom` when both are given,
  // because the two are the same offset stated twice and the spec picks the
  // first for a left-to-right page.
  const float font_size = style.font_size;
  const gfx::FloatRect& content = box.Geometry().content;
  float dx = 0.0f;
  float dy = 0.0f;
  if (!style.inset.left.IsAuto()) {
    dx = Inset(style.inset.left, font_size, content.width);
  } else if (!style.inset.right.IsAuto()) {
    dx = -Inset(style.inset.right, font_size, content.width);
  }
  if (!style.inset.top.IsAuto()) {
    dy = Inset(style.inset.top, font_size, content.height);
  } else if (!style.inset.bottom.IsAuto()) {
    dy = -Inset(style.inset.bottom, font_size, content.height);
  }
  OffsetLaidOutSubtree(box, dx, dy);
}

void LayoutEngine::LayoutAbsoluteDescendants(Box& container,
                                             const gfx::FloatRect& containing_block) const {
  for (std::unique_ptr<Box>& child : container.MutableChildren()) {
    if (child->IsAbsolutelyPositioned()) {
      LayoutAbsoluteBox(*child, containing_block);
      continue;
    }
    // A positioned child is a containing block of its own and placed the boxes
    // inside it at the end of its own layout. Descending into it here would
    // place them a second time, against the wrong rectangle.
    if (child->Style().IsPositioned()) {
      continue;
    }
    LayoutAbsoluteDescendants(*child, containing_block);
  }
}

void LayoutEngine::LayoutAbsoluteBox(Box& box, const gfx::FloatRect& containing_block) const {
  const css::ComputedStyle& style = box.Style();
  const float font_size = style.font_size;
  const css::Edges& inset = style.inset;

  const auto resolve = [font_size](const css::Length& length, float extent) {
    return length.Used(extent, font_size);
  };
  const css::Edges& border = style.has_border ? style.border_width : css::Edges{};
  const float horizontal_extra =
      resolve(style.margin.left, 0.0f) + resolve(style.margin.right, 0.0f) +
      resolve(style.padding.left, 0.0f) + resolve(style.padding.right, 0.0f) +
      resolve(border.left, 0.0f) + resolve(border.right, 0.0f);
  const float vertical_extra =
      resolve(style.margin.top, 0.0f) + resolve(style.margin.bottom, 0.0f) +
      resolve(style.padding.top, 0.0f) + resolve(style.padding.bottom, 0.0f) +
      resolve(border.top, 0.0f) + resolve(border.bottom, 0.0f);

  const bool has_left = !inset.left.IsAuto();
  const bool has_right = !inset.right.IsAuto();
  const bool has_top = !inset.top.IsAuto();
  const bool has_bottom = !inset.bottom.IsAuto();
  const float left = has_left ? resolve(inset.left, containing_block.width) : 0.0f;
  const float right = has_right ? resolve(inset.right, containing_block.width) : 0.0f;
  const float top = has_top ? resolve(inset.top, containing_block.height) : 0.0f;
  const float bottom = has_bottom ? resolve(inset.bottom, containing_block.height) : 0.0f;

  // The used width, in the order the spec decides it: a declared one, then the
  // space between two offsets, then shrink-to-fit. The middle case is what
  // `left: 0; right: 0` means, and it is the one an overlay is written with.
  float outer_width = 0.0f;
  if (!style.width.IsAuto()) {
    outer_width = resolve(style.width, containing_block.width) + horizontal_extra;
  } else if (has_left && has_right) {
    outer_width = std::max(0.0f, containing_block.width - left - right);
  } else {
    outer_width = std::min(MaxContentWidth(box), containing_block.width);
  }
  outer_width = std::max(horizontal_extra, outer_width);

  // Same for height: `top: 0; bottom: 0` (youtube's `#thumbnail`) fills the
  // containing block. A percentage height resolves against that block too --
  // LayoutBlock cannot, because a normal-flow percentage height is indefinite.
  std::optional<float> forced_content_height;
  if (!style.height.IsAuto()) {
    // Includes percentages: against the containing block, which is definite
    // for an absolutely positioned box (unlike normal flow).
    forced_content_height = resolve(style.height, containing_block.height);
  } else if (has_top && has_bottom) {
    forced_content_height =
        std::max(0.0f, containing_block.height - top - bottom - vertical_extra);
  }

  // Placed from whichever edge was given. With neither, the static position --
  // where the flow would have put it -- stands in as the containing block's
  // own origin.
  float x = containing_block.x;
  if (has_left) {
    x = containing_block.x + left;
  } else if (has_right) {
    x = containing_block.x + containing_block.width - right - outer_width;
  }

  float y = containing_block.y;
  if (has_top) {
    y = containing_block.y + top;
  }

  // Laid out with its own formatting context: an absolutely positioned box
  // does not interact with the floats around it, and the floats inside it do
  // not escape.
  FloatContext floats;
  ForcedSize forced;
  forced.content_width = std::max(0.0f, outer_width - horizontal_extra);
  forced.content_height = forced_content_height;
  float cursor = y;
  LayoutBlock(box, x, outer_width, cursor, floats, false, &forced);

  // `bottom` without `top` places the box by its lower edge, which needs the
  // height -- so it is applied after the box has one rather than guessed at.
  if (!has_top && has_bottom) {
    const float height = cursor - y;
    const float wanted = containing_block.y + containing_block.height - bottom - height;
    OffsetLaidOutSubtree(box, 0.0f, wanted - y);
  }
}

}  // namespace microbrowser::layout

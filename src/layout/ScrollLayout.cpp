// What a box can be scrolled to, and where it currently is.
//
// ADR 0018 §1: a scroll offset is layout state owned per box, and the size it
// is clamped against comes out of layout. Neither is derived from style, which
// is what makes this a separate pass rather than something LayoutBlock does on
// its way past -- the flow decides where boxes are, and this decides how much
// of the result is reachable.
//
// The measurement is the *scrollable overflow region* of CSS Overflow §3: the
// padding box of the scroll container, unioned with the border boxes of its
// descendants, clipped to the directions a scroll can actually reach. Content
// above and to the left of the origin is unreachable and deliberately not
// counted -- every engine agrees on that, and counting it would make
// `scrollHeight` grow when a page puts a negative margin on something.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "layout/LayoutEngine.h"

namespace microbrowser::layout {

namespace {

// The far edges a scroll can reach inside `container`, in absolute coordinates.
//
// Stops at a nested scroll container: that box's own overflow is its business,
// and its border box -- which is what this counts -- is the whole of what it
// contributes to its parent. Descending into it instead would make an outer
// scroller's `scrollHeight` include content an inner one already hid.
void AccumulateOverflow(const Box& box, bool is_root, float& right, float& bottom) {
  if (!is_root) {
    const gfx::FloatRect border = box.Geometry().BorderBox();
    right = std::max(right, border.Right());
    bottom = std::max(bottom, border.Bottom());
    for (const TextFragment& fragment : box.Fragments()) {
      right = std::max(right, fragment.rect.Right());
      bottom = std::max(bottom, fragment.rect.Bottom());
    }
    if (box.IsScrollContainer()) {
      return;
    }
  }
  for (const std::unique_ptr<Box>& child : box.Children()) {
    // A fixed box does not scroll with anything, so it cannot extend what a
    // scroller can reach. Counting it would give every page with a fixed
    // footer a scrollable area it never scrolls into.
    if (child->Style().position == css::Position::Fixed) {
      continue;
    }
    AccumulateOverflow(*child, false, right, bottom);
  }
}

bool Contains(const gfx::FloatRect& rect, gfx::FloatPoint point) {
  return point.x >= rect.x && point.x < rect.Right() && point.y >= rect.y &&
         point.y < rect.Bottom();
}

void UpdateSubtree(Box& box, ScrollOffsets& offsets, ScrollOffsets& kept) {
  if (box.IsScrollContainer()) {
    box.SetScrollableOverflow(MeasureScrollableOverflow(box));
    const gfx::FloatPoint limit = MaxScrollOffset(box);
    gfx::FloatPoint wanted;
    if (const dom::Element* element = box.Origin(); element != nullptr) {
      if (const auto found = offsets.find(element); found != offsets.end()) {
        wanted = found->second;
      }
    }
    const gfx::FloatPoint clamped{std::clamp(wanted.x, 0.0f, limit.x),
                                  std::clamp(wanted.y, 0.0f, limit.y)};
    box.SetScrollOffset(clamped);
    // Written back, not just applied: a container that shrank has to forget the
    // offset it can no longer reach, or the next relayout that makes it tall
    // again would restore a position the user never asked for.
    if (box.Origin() != nullptr && clamped != gfx::FloatPoint{}) {
      kept.emplace(box.Origin(), clamped);
    }
  }
  for (std::unique_ptr<Box>& child : box.MutableChildren()) {
    UpdateSubtree(*child, offsets, kept);
  }
}

}  // namespace

gfx::FloatSize MeasureScrollableOverflow(const Box& box) {
  const gfx::FloatRect padding = box.Geometry().PaddingBox();
  float right = padding.Right();
  float bottom = padding.Bottom();
  AccumulateOverflow(box, true, right, bottom);
  return gfx::FloatSize{std::max(0.0f, right - padding.x), std::max(0.0f, bottom - padding.y)};
}

gfx::FloatPoint MaxScrollOffset(const Box& box) {
  const gfx::FloatRect padding = box.Geometry().PaddingBox();
  const gfx::FloatSize overflow = box.ScrollableOverflow();
  return gfx::FloatPoint{std::max(0.0f, overflow.width - padding.width),
                         std::max(0.0f, overflow.height - padding.height)};
}

void UpdateScrollState(Box& root, ScrollOffsets& offsets) {
  ScrollOffsets kept;
  UpdateSubtree(root, offsets, kept);
  offsets = std::move(kept);
}

const Box* ScrollTargetAt(const Box& root, gfx::FloatPoint document_point,
                          gfx::FloatPoint delta) {
  // The chain from the root down to the deepest box containing the point, then
  // walked back up: the deepest scroller that can still move in the requested
  // direction wins, and when it cannot, its ancestor gets the wheel. That is
  // the whole of ADR 0018 §4, and the reason the chain is built rather than
  // recursed through is that "can this one still move" has to be asked from the
  // inside out.
  //
  // The point moves with the descent. Geometry is where a box was laid out;
  // what is *under the pointer* inside a scrolled container is that much
  // further down its content, so the offset is added back on the way in. A
  // version that skipped this would route a wheel to whatever happens to sit at
  // the same place in the unscrolled page.
  std::vector<const Box*> chain;
  const Box* at = &root;
  gfx::FloatPoint point = document_point;
  for (;;) {
    chain.push_back(at);
    if (at->IsScrollContainer()) {
      point.x += at->ScrollOffset().x;
      point.y += at->ScrollOffset().y;
    }
    const Box* next = nullptr;
    for (const std::unique_ptr<Box>& child : at->Children()) {
      if (Contains(child->Geometry().BorderBox(), point)) {
        next = child.get();
      }
    }
    if (next == nullptr) {
      break;
    }
    at = next;
  }

  for (std::size_t i = chain.size(); i-- > 0;) {
    const Box* box = chain[i];
    if (!box->AllowsUserScroll()) {
      continue;
    }
    const gfx::FloatPoint limit = MaxScrollOffset(*box);
    const gfx::FloatPoint offset = box->ScrollOffset();
    const auto can_move = [](float at_now, float by, float high) {
      return (by < 0.0f && at_now > 0.0f) || (by > 0.0f && at_now < high);
    };
    if (can_move(offset.x, delta.x, limit.x) || can_move(offset.y, delta.y, limit.y)) {
      return box;
    }
  }
  return nullptr;
}

}  // namespace microbrowser::layout

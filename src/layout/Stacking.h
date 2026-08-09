#pragma once

#include <vector>

#include "css/ComputedStyle.h"
#include "gfx/Geometry.h"
#include "layout/Box.h"

// CSS 2.1 Appendix E helpers shared by paint (`BuildDisplayList`) and hit-testing
// (`PageHitTest`). Paint and hit-test must agree on who is on top: youtube's
// consent dialog is `position:fixed; z-index:2202` under `ytd-app`, while
// `tp-yt-iron-overlay-backdrop` is a later `body` sibling with `z-index:auto`.
// Tree-order hit-testing alone always picks the backdrop.

namespace microbrowser::layout {

// A box that paints as a unit of its parent stacking context rather than in
// ordinary tree order: positioned, transformed, translucent, or carrying an
// explicit z-index.
inline bool PaintsAsUnit(const Box& box) {
  if (box.GetKind() == Box::Kind::Text) {
    return false;
  }
  const css::ComputedStyle& style = box.Style();
  return style.position != css::Position::Static || !style.transform.IsNone() ||
         style.opacity < 1.0f || style.z_index.has_value();
}

// Layer index within a stacking context. `z-index:auto` orders as zero.
inline int PaintLayer(const Box& box) { return box.Style().z_index.value_or(0); }

// Whether this box forms a stacking context that collects descendant units.
// `z-index:auto` is a unit but not a context: its positioned descendants are
// ordered against its siblings by the nearest ancestor context.
// `opacity < 1` is a stacking context on its own (CSS 2.1 Appendix E / CSS
// Color), matching `transform`.
inline bool IsStackingContext(const Box& box) {
  if (box.GetKind() == Box::Kind::Text) {
    return false;
  }
  const css::ComputedStyle& style = box.Style();
  if (!style.transform.IsNone() || style.opacity < 1.0f) {
    return true;
  }
  return style.position != css::Position::Static && style.z_index.has_value();
}

// Overflow clip between a collecting stacking context and a collected unit
// (TD-0030). `padding_box` is in layout coordinates; `scroll_before` is the
// intervening scroll accumulated *before* entering this container — the same
// value paint subtracts from the stacking context's child_offset to place the
// padding box (mirrors the tree-walk PushClip).
struct InterveningClip {
  gfx::FloatRect padding_box{};
  gfx::FloatPoint scroll_before{};
};

// One Appendix E paint unit, with the scroll and clip chain that the tree walk
// would have applied had the unit been painted in place.
struct StackingUnit {
  const Box* box = nullptr;
  int layer = 0;
  std::size_t order = 0;
  gfx::FloatPoint scroll_delta{};
  std::vector<InterveningClip> intervening_clips;
};

// Abspos / fixed stay under a DOM parent that may `overflow:hidden` with a
// padding box that does not cover them (youtube thumbnails under ink layers).
// The tree-walk clip would hide them; collect used to skip *all* intervening
// clips. TD-0030 restores clips for in-flow / relative units; this keeps the
// existing abspos exception in lockstep for paint and hit-test
// (`Page/LinkAtThroughOverflowHiddenAncestor`).
inline bool SkipsInterveningOverflowClip(const Box& box) {
  return box.Style().IsAbsolutelyPositioned();
}

// When collect descends through a scroll container, record its padding clip
// then accumulate its scroll — same order as BuildDisplayList's PushClip then
// child_offset -= ScrollOffset.
inline void AccumulateOverflowForCollect(const Box& child, gfx::FloatPoint& scroll_from_sc,
                                         std::vector<InterveningClip>& clips) {
  if (!child.IsScrollContainer()) {
    return;
  }
  clips.push_back(InterveningClip{child.Geometry().PaddingBox(), scroll_from_sc});
  scroll_from_sc.x += child.ScrollOffset().x;
  scroll_from_sc.y += child.ScrollOffset().y;
}

}  // namespace microbrowser::layout

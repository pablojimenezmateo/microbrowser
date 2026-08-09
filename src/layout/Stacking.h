#pragma once

#include "css/ComputedStyle.h"
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

}  // namespace microbrowser::layout

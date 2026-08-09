// Scrolling: where the wheel goes, what moves, and who is told.
//
// ADR 0018, and its load-bearing sentence is the reason this file is short:
// **a scroll is a paint, not a layout.** Nothing here rebuilds a box tree or
// re-resolves a cascade *because of the scroll itself*. `EnsureLayoutClean` at
// the start of `ScrollAt` only runs layout when the tree was already dirty —
// without it a wheel hit-tests a stale tree while `scrollTop =` (which cleans
// first) still moves the same scroller.
//
// Split out of Page.cpp because it is a different question from the rest of
// what a Page does: everything else in that file turns a document into boxes,
// and this decides which part of the result is on screen.

#include <algorithm>
#include <utility>
#include <vector>

#include "engine/Page.h"
#include "layout/LayoutEngine.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

void Page::SetScrollOffsetY(float y) {
  if (y == layout_.scroll_y) {
    return;
  }
  layout_.scroll_y = y;
  NoteScrolled(nullptr);
}

void Page::NoteScrolled(const dom::Element* element) {
  for (const dom::Element* pending : scroll_.pending_events) {
    if (pending == element) {
      return;
    }
  }
  scroll_.pending_events.push_back(element);
}

Page::ScrollOutcome Page::ScrollAt(gfx::FloatPoint document_point, gfx::FloatPoint delta) {
  ScrollOutcome outcome;
  // Same clean gate as `scrollTop =` / click hit-testing.
  EnsureLayoutClean();
  if (boxes_ == nullptr) {
    outcome.viewport = true;
    util::AddPerformanceCounter(util::PerfCounterId::ScrollViewportFallback);
    return outcome;
  }
  // Route through the same deepest-element walk as clicks. `ScrollTargetAt`'s
  // "every ancestor BorderBox must contain the point" misses `position:fixed`
  // (and abspos) under a 0×0 host — youtube's consent dialog lives in
  // `ytd-consent-bump-v2-lightbox` at height 0 while `#content { overflow:auto }`
  // is the scroller Accept needs (TD-0022).
  const dom::Element* hit = ElementAt(document_point);
  const auto can_move = [](float at_now, float by, float high) {
    return (by < 0.0f && at_now > 0.0f) || (by > 0.0f && at_now < high);
  };
  for (const dom::Node* at = hit; at != nullptr; at = at->Parent()) {
    if (!at->IsElement()) {
      continue;
    }
    const auto& element = static_cast<const dom::Element&>(*at);
    const auto found = layout_.box_by_element.find(&element);
    if (found == layout_.box_by_element.end()) {
      continue;
    }
    layout::Box* box = found->second;
    if (box == nullptr || !box->AllowsUserScroll()) {
      continue;
    }
    const gfx::FloatPoint limit = layout::MaxScrollOffset(*box);
    const gfx::FloatPoint was = box->ScrollOffset();
    if (!can_move(was.x, delta.x, limit.x) && !can_move(was.y, delta.y, limit.y)) {
      continue;
    }
    const gfx::FloatPoint wanted{std::clamp(was.x + delta.x, 0.0f, limit.x),
                                 std::clamp(was.y + delta.y, 0.0f, limit.y)};
    if (wanted == was) {
      continue;
    }
    box->SetScrollOffset(wanted);
    scroll_.offsets[&element] = wanted;
    NoteScrolled(&element);
    util::AddPerformanceCounter(util::PerfCounterId::ScrollOverflowMoved);
    const gfx::FloatRect port = box->Geometry().PaddingBox();
    outcome.damage = gfx::EnclosingIntRect(
        gfx::FloatRect{port.x, port.y - layout_.scroll_y, port.width, port.height});
    outcome.moved = true;
    return outcome;
  }
  outcome.viewport = true;
  util::AddPerformanceCounter(util::PerfCounterId::ScrollViewportFallback);
  return outcome;
}

void Page::AppendScrollInvariantRects(std::vector<gfx::IntRect>& out) const {
  if (boxes_ == nullptr) {
    return;
  }
  const auto visit = [this, &out](const layout::Box& box) {
    const css::Position position = box.Style().position;
    if (position != css::Position::Fixed && position != css::Position::Sticky) {
      return;
    }
    const gfx::FloatRect border = box.Geometry().BorderBox();
    if (border.IsEmpty()) {
      return;
    }
    // A fixed box is already in viewport coordinates: it does not move with the
    // document, so its laid-out rectangle is where it is.
    if (position == css::Position::Fixed) {
      out.push_back(gfx::EnclosingIntRect(border));
      return;
    }
    // A sticky box is somewhere between where the flow put it and the edge it
    // sticks to, and which of the two depends on the scroll. The damage is the
    // strip containing both readings -- an over-report bounded by the box's own
    // height, and over-reporting damage repaints too much where under-reporting
    // it leaves a stale rectangle on screen forever.
    const css::ComputedStyle& style = box.Style();
    const float port = viewport_.viewport_height;
    gfx::FloatRect rect{border.x, border.y - layout_.scroll_y, border.width, border.height};
    if (!style.inset.top.IsAuto()) {
      const float pin = style.inset.top.Used(port, style.font_size);
      rect = rect.United(gfx::FloatRect{rect.x, pin, rect.width, rect.height});
    }
    if (!style.inset.bottom.IsAuto() && port > 0.0f) {
      const float pin = port - style.inset.bottom.Used(port, style.font_size) - rect.height;
      rect = rect.United(gfx::FloatRect{rect.x, pin, rect.width, rect.height});
    }
    out.push_back(gfx::EnclosingIntRect(rect));
  };
  visit(*boxes_);
  boxes_->ForEachDescendant(visit);
}

bool Page::DispatchPendingScrollEvents() {
  if (scroll_.pending_events.empty()) {
    return false;
  }
  // Taken before dispatching: a handler that scrolls something else files a new
  // one, and running the list while it grows is how a page with a `scroll`
  // handler that scrolls becomes an infinite loop inside one turn.
  std::vector<const dom::Element*> targets;
  targets.swap(scroll_.pending_events);
  bool ran = false;
  for (const dom::Element* target : targets) {
    ran = script_.DispatchScroll(const_cast<dom::Element*>(target)) || ran;
  }
  return ran;
}

}  // namespace microbrowser::engine

// Scrolling: where the wheel goes, what moves, and who is told.
//
// ADR 0018, and its load-bearing sentence is the reason this file is short:
// **a scroll is a paint, not a layout.** Nothing here rebuilds a box tree or
// re-resolves a cascade. A scroll writes a number onto a box, records that the
// box owes a `scroll` event, and reports the rectangle that has to be redrawn.
//
// Split out of Page.cpp because it is a different question from the rest of
// what a Page does: everything else in that file turns a document into boxes,
// and this decides which part of the result is on screen.

#include <algorithm>
#include <utility>
#include <vector>

#include "engine/Page.h"
#include "layout/LayoutEngine.h"

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
  if (boxes_ == nullptr) {
    outcome.viewport = true;
    return outcome;
  }
  const layout::Box* target = layout::ScrollTargetAt(*boxes_, document_point, delta);
  if (target == nullptr || target->Origin() == nullptr) {
    // Nothing inside the page can take it, so the document does. That is the
    // chaining rule reaching its last link rather than a failure.
    outcome.viewport = true;
    return outcome;
  }
  const gfx::FloatPoint limit = layout::MaxScrollOffset(*target);
  const gfx::FloatPoint was = target->ScrollOffset();
  const gfx::FloatPoint wanted{std::clamp(was.x + delta.x, 0.0f, limit.x),
                               std::clamp(was.y + delta.y, 0.0f, limit.y)};
  if (wanted == was) {
    outcome.viewport = true;
    return outcome;
  }
  const_cast<layout::Box*>(target)->SetScrollOffset(wanted);
  scroll_.offsets[target->Origin()] = wanted;
  NoteScrolled(target->Origin());
  // What moved: the scroller's padding box, which is the clip its content is
  // drawn inside. Nothing outside it changed, which is the point.
  const gfx::FloatRect port = target->Geometry().PaddingBox();
  outcome.damage = gfx::EnclosingIntRect(
      gfx::FloatRect{port.x, port.y - layout_.scroll_y, port.width, port.height});
  outcome.moved = true;
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

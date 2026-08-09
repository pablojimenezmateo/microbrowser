#include "engine/Page.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "html/FormControl.h"
#include "layout/LayoutEngine.h"
#include "layout/Stacking.h"
#include "util/PerformanceCounters.h"

// Hit testing for the page: links, form controls, and the deepest element under
// a point. Lives here rather than in Page.cpp because that file is at the
// module's line cap, and because `visibility: hidden` (youtube's closed guide
// drawer) belongs next to the walk that has to honour it — ADR 0017 §5.
//
// Paint order is CSS 2.1 Appendix E (`layout/Stacking.h` + BuildDisplayList).
// Hit-testing walks that order in reverse. A three-band sibling walk is not
// enough: youtube's consent dialog is `z-index:2202` under `ytd-app`, while
// `tp-yt-iron-overlay-backdrop` is a later `body` sibling with `z-index:auto`.
// Only collecting units into the root stacking context puts the dialog above
// the backdrop the way paint already does.

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool Contains(const gfx::FloatRect& rect, gfx::FloatPoint point) {
  return point.x >= rect.x && point.x < rect.Right() && point.y >= rect.y &&
         point.y < rect.Bottom();
}

bool ReceivesPointerEvents(const layout::Box& box) {
  const css::ComputedStyle& style = box.Style();
  return style.visibility == css::Visibility::Visible &&
         style.pointer_events != css::PointerEvents::None;
}

std::optional<gfx::FloatPoint> UntransformedPoint(const layout::Box& box,
                                                  gfx::FloatPoint point) {
  const css::ComputedStyle& style = box.Style();
  if (style.transform.IsNone() || box.Origin() == nullptr) {
    return point;
  }
  const gfx::FloatRect border_box = box.Geometry().BorderBox();
  const gfx::FloatPoint origin{
      border_box.x + style.transform_origin_x.Used(border_box.width, style.font_size),
      border_box.y + style.transform_origin_y.Used(border_box.height, style.font_size)};
  const std::optional<gfx::AffineTransform> inverse =
      style.transform
          .ToMatrix(gfx::FloatSize{border_box.width, border_box.height}, origin,
                    style.font_size)
          .Inverted();
  if (!inverse.has_value()) {
    return std::nullopt;
  }
  return inverse->MapPoint(point);
}

std::optional<gfx::FloatPoint> PointInside(const layout::Box& box, gfx::FloatPoint point) {
  if (!box.IsScrollContainer()) {
    return point;
  }
  if (!Contains(box.Geometry().PaddingBox(), point)) {
    return std::nullopt;
  }
  return gfx::FloatPoint{point.x + box.ScrollOffset().x, point.y + box.ScrollOffset().y};
}

// Scroll displacement without the clip test. Abspos / fixed descendants stay in
// the box tree under a DOM parent that may `overflow: hidden` with a padding
// box that does not cover them (youtube's thumbnail link under ink layers and
// clipped ancestors). Painting still finds them; hit testing must too.
gfx::FloatPoint ScrollAdjust(const layout::Box& box, gfx::FloatPoint point) {
  if (!box.IsScrollContainer()) {
    return point;
  }
  return gfx::FloatPoint{point.x + box.ScrollOffset().x, point.y + box.ScrollOffset().y};
}

const std::string* AnchorHref(const dom::Element* element) {
  if (element == nullptr || element->TagName() != "a") {
    return nullptr;
  }
  const std::string* href = element->GetAttribute("href");
  return href != nullptr && !href->empty() ? href : nullptr;
}

struct PaintUnit {
  const layout::Box* box = nullptr;
  int layer = 0;
  std::size_t order = 0;
  // Scroll offsets of overflow ancestors between the collecting stacking context
  // and this unit. Layout BorderBoxes stay in unscrolled coordinates; jumping
  // to a collected unit (youtube Accept: position:relative button under
  // #content { overflow:auto }) must add these or the button never contains
  // the viewport point and the static yt-button-shape parent steals the hit.
  gfx::FloatPoint scroll_delta{};
};

std::vector<PaintUnit> CollectPaintUnits(const layout::Box& parent) {
  std::vector<PaintUnit> units;
  const auto recurse = [&units](const layout::Box& box, gfx::FloatPoint scroll_from_sc,
                                auto& self) -> void {
    for (const std::unique_ptr<layout::Box>& child : box.Children()) {
      const bool unit = layout::PaintsAsUnit(*child);
      if (unit) {
        units.push_back(
            PaintUnit{child.get(), layout::PaintLayer(*child), units.size(), scroll_from_sc});
      }
      if (!unit || !layout::IsStackingContext(*child)) {
        gfx::FloatPoint into = scroll_from_sc;
        if (child->IsScrollContainer()) {
          into.x += child->ScrollOffset().x;
          into.y += child->ScrollOffset().y;
        }
        self(*child, into, self);
      }
    }
  };
  recurse(parent, gfx::FloatPoint{}, recurse);
  std::stable_sort(units.begin(), units.end(), [](const PaintUnit& a, const PaintUnit& b) {
    return a.layer != b.layer ? a.layer < b.layer : a.order < b.order;
  });
  return units;
}

// Reverse of BuildDisplayList's Appendix E steps for one stacking context:
// positive layers (high→low), layer 0 (late tree→early), in-flow non-units,
// then negative layers (high→low).
template <typename VisitUnit, typename VisitInFlow>
bool VisitReversePaintChildren(const layout::Box& box, gfx::FloatPoint point, bool collects,
                               VisitUnit&& visit_unit, VisitInFlow&& visit_in_flow) {
  const gfx::FloatPoint scrolled = ScrollAdjust(box, point);
  std::vector<PaintUnit> units;
  if (collects) {
    units = CollectPaintUnits(box);
    for (std::size_t i = units.size(); i-- > 0;) {
      if (units[i].layer <= 0) {
        continue;
      }
      const gfx::FloatPoint at{scrolled.x + units[i].scroll_delta.x,
                               scrolled.y + units[i].scroll_delta.y};
      if (visit_unit(*units[i].box, at, layout::IsStackingContext(*units[i].box))) {
        return true;
      }
    }
    for (std::size_t i = units.size(); i-- > 0;) {
      if (units[i].layer != 0) {
        continue;
      }
      const gfx::FloatPoint at{scrolled.x + units[i].scroll_delta.x,
                               scrolled.y + units[i].scroll_delta.y};
      if (visit_unit(*units[i].box, at, layout::IsStackingContext(*units[i].box))) {
        return true;
      }
    }
  }
  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    const auto& children = box.Children();
    // Appendix E paints in-flow blocks, then floats, then inlines. Reverse for
    // hit-testing: floats before overlapping in-flow blocks (old.reddit `.side`).
    for (std::size_t i = children.size(); i-- > 0;) {
      if (!layout::PaintsAsUnit(*children[i]) && children[i]->IsFloating() &&
          visit_in_flow(*children[i], *inside)) {
        return true;
      }
    }
    for (std::size_t i = children.size(); i-- > 0;) {
      if (!layout::PaintsAsUnit(*children[i]) && !children[i]->IsFloating() &&
          visit_in_flow(*children[i], *inside)) {
        return true;
      }
    }
  }
  if (collects) {
    for (std::size_t i = units.size(); i-- > 0;) {
      if (units[i].layer >= 0) {
        continue;
      }
      const gfx::FloatPoint at{scrolled.x + units[i].scroll_delta.x,
                               scrolled.y + units[i].scroll_delta.y};
      if (visit_unit(*units[i].box, at, layout::IsStackingContext(*units[i].box))) {
        return true;
      }
    }
  }
  return false;
}

using ElementPredicate = bool (*)(const dom::Element&);

dom::Element* HitTestFormControl(const layout::Box& box, gfx::FloatPoint point,
                                 ElementPredicate predicate, bool collects) {
  const std::optional<gfx::FloatPoint> local = UntransformedPoint(box, point);
  if (!local.has_value()) {
    return nullptr;
  }
  point = *local;

  dom::Element* found = nullptr;
  if (VisitReversePaintChildren(
          box, point, collects,
          [&](const layout::Box& child, gfx::FloatPoint p, bool child_collects) {
            found = HitTestFormControl(child, p, predicate, child_collects);
            return found != nullptr;
          },
          [&](const layout::Box& child, gfx::FloatPoint p) {
            found = HitTestFormControl(child, p, predicate, false);
            return found != nullptr;
          })) {
    return found;
  }

  if (!ReceivesPointerEvents(box)) {
    return nullptr;
  }
  const dom::Element* element = box.Origin();
  if (element == nullptr || html::IsDisabledFormControl(*element) || !predicate(*element)) {
    return nullptr;
  }
  if (!Contains(box.Geometry().BorderBox(), point)) {
    return nullptr;
  }
  return const_cast<dom::Element*>(element);
}

const dom::Element* HitTestElement(const layout::Box& box, gfx::FloatPoint point,
                                   const dom::Element* enclosing, bool collects) {
  const std::optional<gfx::FloatPoint> local = UntransformedPoint(box, point);
  if (!local.has_value()) {
    return nullptr;
  }
  point = *local;
  if (box.Origin() != nullptr) {
    enclosing = box.Origin();
  }

  const dom::Element* found = nullptr;
  if (VisitReversePaintChildren(
          box, point, collects,
          [&](const layout::Box& child, gfx::FloatPoint p, bool child_collects) {
            found = HitTestElement(child, p, enclosing, child_collects);
            return found != nullptr;
          },
          [&](const layout::Box& child, gfx::FloatPoint p) {
            found = HitTestElement(child, p, enclosing, false);
            return found != nullptr;
          })) {
    return found;
  }

  if (!ReceivesPointerEvents(box)) {
    return nullptr;
  }
  if (box.GetKind() == layout::Box::Kind::Text) {
    for (const layout::TextFragment& fragment : box.Fragments()) {
      if (Contains(fragment.rect, point)) {
        return enclosing;
      }
    }
    return nullptr;
  }
  if (enclosing != nullptr && Contains(box.Geometry().BorderBox(), point)) {
    return enclosing;
  }
  return nullptr;
}

std::optional<std::string> HitTestLink(const layout::Box& box, gfx::FloatPoint point,
                                       const std::string* active_href, bool collects) {
  const std::optional<gfx::FloatPoint> local = UntransformedPoint(box, point);
  if (!local.has_value()) {
    return std::nullopt;
  }
  point = *local;
  if (const std::string* href = AnchorHref(box.Origin())) {
    active_href = href;
  }

  std::optional<std::string> found;
  if (VisitReversePaintChildren(
          box, point, collects,
          [&](const layout::Box& child, gfx::FloatPoint p, bool child_collects) {
            found = HitTestLink(child, p, active_href, child_collects);
            return found.has_value();
          },
          [&](const layout::Box& child, gfx::FloatPoint p) {
            found = HitTestLink(child, p, active_href, false);
            return found.has_value();
          })) {
    return found;
  }

  if (!ReceivesPointerEvents(box) || active_href == nullptr) {
    return std::nullopt;
  }
  if (box.GetKind() == layout::Box::Kind::Text) {
    for (const layout::TextFragment& fragment : box.Fragments()) {
      if (Contains(fragment.rect, point)) {
        return *active_href;
      }
    }
    return std::nullopt;
  }
  if (Contains(box.Geometry().BorderBox(), point)) {
    return *active_href;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> Page::LinkAt(gfx::FloatPoint document_point) const {
  // Script that ran for the click may have mutated the tree and cleared `boxes_`
  // (InvalidateBoxTree). Default actions still have to hit-test the page that
  // exists *now*, which is why geometry queries already call EnsureLayoutClean.
  const_cast<Page*>(this)->EnsureLayoutClean();
  if (boxes_ == nullptr) {
    return std::nullopt;
  }
  AddPerformanceCounter(PerfCounterId::EngineHitTests);
  return HitTestLink(*boxes_, document_point, nullptr, true);
}

const dom::Element* Page::ElementAt(gfx::FloatPoint document_point) const {
  const_cast<Page*>(this)->EnsureLayoutClean();
  if (boxes_ == nullptr) {
    return nullptr;
  }
  AddPerformanceCounter(PerfCounterId::EngineHitTests);
  return HitTestElement(*boxes_, document_point, nullptr, true);
}

dom::Element* HitTestFormControlAt(const layout::Box& root, gfx::FloatPoint document_point,
                                   bool (*predicate)(const dom::Element&)) {
  AddPerformanceCounter(PerfCounterId::EngineHitTests);
  return HitTestFormControl(root, document_point, predicate, true);
}

}  // namespace microbrowser::engine

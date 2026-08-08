#include "engine/Page.h"

#include <optional>
#include <string>

#include "html/FormControl.h"
#include "layout/LayoutEngine.h"

// Hit testing for the page: links, form controls, and the deepest element under
// a point. Lives here rather than in Page.cpp because that file is at the
// module's line cap, and because `visibility: hidden` (youtube's closed guide
// drawer) belongs next to the walk that has to honour it — ADR 0017 §5.

namespace microbrowser::engine {

namespace {

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

bool PaintsAboveInFlowBlocks(const layout::Box& box) {
  return box.IsFloating() || box.IsAbsolutelyPositioned();
}

using ElementPredicate = bool (*)(const dom::Element&);

dom::Element* HitTestFormControl(const layout::Box& box, gfx::FloatPoint point,
                                 ElementPredicate predicate) {
  const std::optional<gfx::FloatPoint> local = UntransformedPoint(box, point);
  if (!local.has_value()) {
    return nullptr;
  }
  point = *local;
  const gfx::FloatPoint scrolled = ScrollAdjust(box, point);
  // Elevated children first, even when this box clips in-flow content away —
  // see ScrollAdjust. In-flow children only when the point is inside.
  {
    dom::Element* hit = nullptr;
    const auto& children = box.Children();
    for (std::size_t i = children.size(); i-- > 0;) {
      if (PaintsAboveInFlowBlocks(*children[i])) {
        hit = HitTestFormControl(*children[i], scrolled, predicate);
        if (hit != nullptr) {
          return hit;
        }
      }
    }
  }
  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    dom::Element* hit = nullptr;
    const auto& children = box.Children();
    for (std::size_t i = children.size(); i-- > 0;) {
      if (!PaintsAboveInFlowBlocks(*children[i])) {
        hit = HitTestFormControl(*children[i], *inside, predicate);
        if (hit != nullptr) {
          return hit;
        }
      }
    }
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
                                   const dom::Element* enclosing) {
  const std::optional<gfx::FloatPoint> local = UntransformedPoint(box, point);
  if (!local.has_value()) {
    return nullptr;
  }
  point = *local;
  if (box.Origin() != nullptr) {
    enclosing = box.Origin();
  }
  const gfx::FloatPoint scrolled = ScrollAdjust(box, point);
  {
    const dom::Element* hit = nullptr;
    const auto& children = box.Children();
    for (std::size_t i = children.size(); i-- > 0;) {
      if (PaintsAboveInFlowBlocks(*children[i])) {
        hit = HitTestElement(*children[i], scrolled, enclosing);
        if (hit != nullptr) {
          return hit;
        }
      }
    }
  }
  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    const dom::Element* hit = nullptr;
    const auto& children = box.Children();
    for (std::size_t i = children.size(); i-- > 0;) {
      if (!PaintsAboveInFlowBlocks(*children[i])) {
        hit = HitTestElement(*children[i], *inside, enclosing);
        if (hit != nullptr) {
          return hit;
        }
      }
    }
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
                                       const std::string* active_href) {
  const std::optional<gfx::FloatPoint> local = UntransformedPoint(box, point);
  if (!local.has_value()) {
    return std::nullopt;
  }
  point = *local;
  if (const std::string* href = AnchorHref(box.Origin())) {
    active_href = href;
  }

  const gfx::FloatPoint scrolled = ScrollAdjust(box, point);
  {
    std::optional<std::string> hit;
    const auto& children = box.Children();
    for (std::size_t i = children.size(); i-- > 0;) {
      if (PaintsAboveInFlowBlocks(*children[i])) {
        hit = HitTestLink(*children[i], scrolled, active_href);
        if (hit.has_value()) {
          return hit;
        }
      }
    }
  }
  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    std::optional<std::string> hit;
    const auto& children = box.Children();
    for (std::size_t i = children.size(); i-- > 0;) {
      if (!PaintsAboveInFlowBlocks(*children[i])) {
        hit = HitTestLink(*children[i], *inside, active_href);
        if (hit.has_value()) {
          return hit;
        }
      }
    }
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
  return HitTestLink(*boxes_, document_point, nullptr);
}

const dom::Element* Page::ElementAt(gfx::FloatPoint document_point) const {
  const_cast<Page*>(this)->EnsureLayoutClean();
  if (boxes_ == nullptr) {
    return nullptr;
  }
  return HitTestElement(*boxes_, document_point, nullptr);
}

dom::Element* HitTestFormControlAt(const layout::Box& root, gfx::FloatPoint document_point,
                                   bool (*predicate)(const dom::Element&)) {
  return HitTestFormControl(root, document_point, predicate);
}

}  // namespace microbrowser::engine

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Page.h"
#include "html/FormControl.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

// Dynamic element state, and what a change to it is allowed to cost.
//
// ADR 0016 §2 puts the state on the element and §3 puts the decision about what
// to recompute in an index built once per stylesheet. This file is the engine's
// half of both: it is the only place that *writes* a state bit, and the only
// place that asks the index what a write is worth.
//
// The order of the two questions is the whole design. The index is asked
// *before* anything is looked up, so a pointer crossing a page whose stylesheet
// never mentions `:hover` does not hit-test, does not restyle, does not lay out
// and does not paint -- which is the property that decays silently, because a
// browser that restyles on every mouse move looks exactly like one that does
// not until somebody measures it.
//
// Split from Page.cpp because that file is at the module's translation-unit cap
// and because the split falls somewhere real: everything in Page.cpp turns a
// document into boxes, and this decides how little of that has to happen again.

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The topmost element at `point` and every ancestor of it, deepest first.
// `:hover` is on all of them: the specification's rule is that an element is
// hovered when the pointer is over it *or* over anything it contains, which is
// what makes `nav:hover .menu { display: block }` work.
std::vector<const dom::Element*> AncestorChain(const dom::Element* deepest) {
  std::vector<const dom::Element*> chain;
  for (const dom::Node* at = deepest; at != nullptr; at = at->Parent()) {
    if (at->IsElement()) {
      chain.push_back(static_cast<const dom::Element*>(at));
    }
  }
  return chain;
}

// Every box in the tree that came from an element, by element.
//
// Built per restyle rather than kept, because the box tree is rebuilt on every
// layout and a map that outlived one would hold pointers into freed boxes. It
// is one walk over the tree that is about to be walked again to paint it.
void CollectOriginBoxes(layout::Box& box,
                        std::map<const dom::Element*, layout::Box*>& out) {
  if (box.Origin() != nullptr) {
    out.emplace(box.Origin(), &box);
  }
  for (const std::unique_ptr<layout::Box>& child : box.MutableChildren()) {
    CollectOriginBoxes(*child, out);
  }
}

// Text boxes carry the inherited half of the style of the box around them, so
// they are refreshed from their parent after the element boxes have been.
void RefreshTextStyles(layout::Box& box) {
  for (const std::unique_ptr<layout::Box>& child : box.MutableChildren()) {
    if (child->GetKind() == layout::Box::Kind::Text) {
      child->SetStyle(layout::TextStyleFrom(box.Style()));
    }
    RefreshTextStyles(*child);
  }
}

// The value a text control currently holds, which is what decides whether its
// placeholder is showing. The `value` attribute is the one copy of it -- typing
// writes there -- so this is a read of the document rather than a second store.
bool PlaceholderIsShown(const dom::Element& element) {
  const std::string* placeholder = element.GetAttribute("placeholder");
  if (placeholder == nullptr || placeholder->empty()) {
    return false;
  }
  if (html::IsTextareaElement(element)) {
    return element.TextContent().empty();
  }
  if (!html::IsTextInputType(element)) {
    return false;
  }
  const std::string* value = element.GetAttribute("value");
  return value == nullptr || value->empty();
}

bool IsCheckedControl(const dom::Element& element) {
  if (html::IsCheckableInput(element)) {
    return element.HasAttribute("checked");
  }
  // An `<option>` in a `<select>`. The same attribute-is-the-truth rule: this
  // engine has no separate selectedness, so the attribute is where a click puts
  // the answer and where `:checked` reads it.
  return element.TagName() == "option" && element.HasAttribute("selected");
}

bool IsRequiredControl(const dom::Element& element) {
  const std::string_view tag = element.TagName();
  return (tag == "input" || tag == "select" || tag == "textarea") &&
         element.HasAttribute("required");
}

}  // namespace

bool Page::StyleDependsOn(dom::ElementState state) const {
  return resolver_.Invalidation().DependsOn(state);
}

css::StyleChangeEffect Page::StateChangeEffect(dom::ElementState changed) const {
  const css::StyleChangeEffect effect = resolver_.Invalidation().EffectOf(changed);
  if (effect == css::StyleChangeEffect::None) {
    AddPerformanceCounter(PerfCounterId::StyleInvalidationSkips);
  }
  return effect;
}

dom::ElementState Page::SetStateOn(const std::vector<const dom::Element*>& on,
                                   dom::ElementState state) {
  if (document_ == nullptr) {
    return dom::ElementState::None;
  }
  dom::ElementState changed = dom::ElementState::None;
  // One pass over the document rather than a remembered chain to undo.
  //
  // A stored `Element*` would be cheaper and is what a mature engine keeps; it
  // also dangles the moment a script removes the element the pointer is over,
  // and unlike focus there is no single choke point that could clear it --
  // `Node::ReleaseFocusWithin` exists because focus has exactly one home. This
  // is the safe shape, and it is only reached when some rule depends on the
  // state, which is the measurement that would justify changing it.
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    // Const away for the same reason hit testing does it: the box tree hands
    // out const elements to keep layout honest, and the document behind them is
    // not const.
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    const bool wanted = std::find(on.begin(), on.end(), &element) != on.end();
    if (element.SetState(state, wanted)) {
      AddPerformanceCounter(PerfCounterId::StyleStateChanges);
      changed |= state;
    }
  });
  return changed;
}

dom::ElementState Page::UpdateHoverChain(gfx::FloatPoint document_point, bool active) {
  const bool wants_hover = StyleDependsOn(dom::ElementState::Hover);
  const bool wants_active = StyleDependsOn(dom::ElementState::Active);
  if (!wants_hover && !wants_active) {
    // The headline property of ADR 0016, and it is this early return rather
    // than anything downstream: no rule can care, so the pointer's position is
    // not a fact worth computing. The cost of that is that the state is not
    // tracked while nothing reads it, so a stylesheet arriving mid-load leaves
    // the first hover un-styled until the pointer next moves -- which is one
    // frame, and is why this is the trade rather than a bug.
    AddPerformanceCounter(PerfCounterId::StyleInvalidationSkips);
    return dom::ElementState::None;
  }
  if (boxes_ == nullptr || document_ == nullptr) {
    return dom::ElementState::None;
  }
  AddPerformanceCounter(PerfCounterId::StyleHoverHitTests);
  const std::vector<const dom::Element*> chain = AncestorChain(ElementAt(document_point));
  dom::ElementState changed = dom::ElementState::None;
  if (wants_hover) {
    changed |= SetStateOn(chain, dom::ElementState::Hover);
  }
  if (wants_active) {
    changed |= SetStateOn(active ? chain : std::vector<const dom::Element*>{},
                          dom::ElementState::Active);
  }
  return changed;
}

dom::ElementState Page::ClearHoverChain() {
  dom::ElementState changed = dom::ElementState::None;
  if (StyleDependsOn(dom::ElementState::Hover)) {
    changed |= SetStateOn({}, dom::ElementState::Hover);
  }
  if (StyleDependsOn(dom::ElementState::Active)) {
    changed |= SetStateOn({}, dom::ElementState::Active);
  }
  return changed;
}

void Page::RefreshDocumentStates() {
  if (document_ == nullptr) {
    return;
  }
  util::PerformanceTrace::Scope scope("engine::Page::RefreshDocumentStates");
  document_->ForEachDescendant([](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    // Recomputed from the document every time rather than maintained at each
    // of the dozen places that can change one -- a click on a checkbox, a form
    // reset, a script's setAttribute, the parser. Maintaining it would be
    // faster and would be wrong the first time one of those was added without
    // remembering, which is the failure mode ADR 0016 warns about: a state bit
    // the engine forgets is a rule that silently never applies.
    element.SetState(dom::ElementState::Checked, IsCheckedControl(element));
    element.SetState(dom::ElementState::Disabled, html::IsDisabledFormControl(element));
    element.SetState(dom::ElementState::Required, IsRequiredControl(element));
    element.SetState(dom::ElementState::PlaceholderShown, PlaceholderIsShown(element));
  });
}

void Page::RefreshTargetState() {
  if (document_ == nullptr) {
    return;
  }
  const std::size_t hash = url_.find('#');
  const std::string_view fragment =
      hash == std::string::npos ? std::string_view() : std::string_view(url_).substr(hash + 1);
  std::vector<const dom::Element*> target;
  if (!fragment.empty()) {
    document_->ForEachDescendant([&](const dom::Node& node) {
      if (!target.empty() || !node.IsElement()) {
        return;
      }
      const auto& element = static_cast<const dom::Element&>(node);
      const std::string* id = element.GetAttribute("id");
      if (id != nullptr && *id == fragment) {
        target.push_back(&element);
      }
    });
  }
  // One element and not a chain, which is the whole reason SetStateOn takes a
  // set rather than a deepest element: `:target` is the element the fragment
  // names, and its ancestors are not targets. `:hover` is the other shape.
  SetStateOn(target, dom::ElementState::Target);
}

void Page::RestyleWithoutLayout() {
  if (document_ == nullptr || boxes_ == nullptr) {
    return;
  }
  util::PerformanceTrace::Scope scope("engine::Page::RestyleWithoutLayout");
  AddPerformanceCounter(PerfCounterId::StyleRestylesWithoutLayout);
  RefreshDocumentStates();

  std::map<const dom::Element*, layout::Box*> boxes;
  CollectOriginBoxes(*boxes_, boxes);
  // The cascade is re-resolved over the *document*, in tree order, which is
  // what gets inheritance right; the result is then written into whichever box
  // that element produced. Walking the box tree instead would inherit through
  // anonymous boxes and foster-parented content, which is not the tree the
  // cascade is defined over.
  resolver_.ForEachStyledElement(*document_, [&](const dom::Element& element,
                                                 const css::ComputedStyle& style) {
    const auto found = boxes.find(&element);
    if (found == boxes.end()) {
      return;
    }
    found->second->SetStyle(style);
    // A background image is attached to the box rather than looked up at paint
    // time, so a rule that changes one has to re-attach it. Clearing it when
    // the new style names none is the half that would otherwise leave the old
    // picture on screen.
    found->second->SetBackgroundImage(style.background.image.empty()
                                          ? nullptr
                                          : ImageFor(style.background.image));
  });
  RefreshTextStyles(*boxes_);
}

}  // namespace microbrowser::engine

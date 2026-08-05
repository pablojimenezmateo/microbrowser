#include "engine/Page.h"

#include <algorithm>
#include <utility>

#include "css/StyleSheet.h"
#include "gfx/SvgDecoder.h"
#include "engine/FormAlgorithms.h"
#include "engine/ImageSelection.h"
#include "html/Focus.h"
#include "html/FormControl.h"
#include "html/TreeBuilder.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The text of an element's direct text children, concatenated. Enough for
// <title>, which is a text-only element by definition.
std::string DirectText(const dom::Element& element) {
  std::string text;
  for (const std::unique_ptr<dom::Node>& child : element.Children()) {
    if (child->IsText()) {
      text += static_cast<const dom::Text&>(*child).Data();
    }
  }
  return text;
}

bool Contains(const gfx::FloatRect& rect, gfx::FloatPoint point) {
  return point.x >= rect.x && point.x < rect.Right() && point.y >= rect.y &&
         point.y < rect.Bottom();
}

// Where a point lands inside `box`'s children, or nothing when it lands
// outside them entirely.
//
// A scroll container does two things to a point, and they are the same fact
// stated twice: it displaces its content, so what is under the pointer is that
// much further down; and it clips, so a point outside its padding box hits
// nothing inside it however far the geometry extends. Hit testing has to walk
// the paint rather than the flow, and this is the whole of the difference.
std::optional<gfx::FloatPoint> PointInside(const layout::Box& box, gfx::FloatPoint point) {
  if (!box.IsScrollContainer()) {
    return point;
  }
  if (!Contains(box.Geometry().PaddingBox(), point)) {
    return std::nullopt;
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


bool IsValueResettableControl(const dom::Element& element) {
  return html::IsTextControl(element);
}

bool IsRadioGroupPeer(const dom::Element& candidate,
                      const dom::Element& activated,
                      const dom::Document& document) {
  if (&candidate == &activated || !html::IsRadioInput(candidate)) {
    return false;
  }
  const std::string* candidate_name = candidate.GetAttribute("name");
  const std::string* activated_name = activated.GetAttribute("name");
  if (candidate_name == nullptr || activated_name == nullptr || candidate_name->empty() ||
      *candidate_name != *activated_name) {
    return false;
  }
  return html::FormOwner(candidate, document) == html::FormOwner(activated, document);
}

using ElementPredicate = bool (*)(const dom::Element&);

// The one hit-test walk over form controls: submit, reset, checkbox, radio and
// text field differ only in the predicate.
//
// Last child first, because a later sibling paints over an earlier one and the
// topmost box under the point is the one that was clicked. A disabled control
// is never a target, which is a property of every control rather than of any
// one predicate -- so it is checked here, once, instead of being re-derived in
// each caller.
//
// The box tree is const because hit testing does not change layout, and
// `Origin()` hands out a const element to preserve that. The element itself is
// not const: activating a control mutates it -- a checkbox flips `checked`, a
// text field takes focus -- and the document it belongs to is mutable. That is
// what the cast crosses, and why it lives here rather than at four call sites.
dom::Element* HitTestFormControl(const layout::Box& box, gfx::FloatPoint point,
                                 ElementPredicate predicate) {
  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    for (std::size_t i = box.Children().size(); i-- > 0;) {
      if (dom::Element* hit = HitTestFormControl(*box.Children()[i], *inside, predicate)) {
        return hit;
      }
    }
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

// The innermost element whose box contains `point`, or null.
//
// Deepest-first, and the last child first within a level: a box painted over
// another is the one a click lands on, and the paint order is child-after-
// parent and later-sibling-after-earlier.
//
// `enclosing` is the nearest ancestor that came from an element, and it is
// what makes this work at all. A text box has no element of its own, and an
// *inline* box has no useful geometry -- its text fragments carry the
// rectangles. So a click on the words inside `<a>hello</a>` hits a text box
// with no origin, inside a box with no area, and testing either alone finds
// nothing. Carrying the enclosing element down is the same shape HitTestLink
// uses to carry an href.
const dom::Element* HitTestElement(const layout::Box& box, gfx::FloatPoint point,
                                   const dom::Element* enclosing) {
  if (box.Origin() != nullptr) {
    enclosing = box.Origin();
  }
  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    for (std::size_t i = box.Children().size(); i-- > 0;) {
      if (const dom::Element* hit = HitTestElement(*box.Children()[i], *inside, enclosing)) {
        return hit;
      }
    }
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
  if (const std::string* href = AnchorHref(box.Origin())) {
    active_href = href;
  }

  if (const std::optional<gfx::FloatPoint> inside = PointInside(box, point)) {
    for (std::size_t i = box.Children().size(); i-- > 0;) {
      if (std::optional<std::string> hit = HitTestLink(*box.Children()[i], *inside, active_href)) {
        return hit;
      }
    }
  }

  if (active_href == nullptr) {
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

Page::Page(gfx::FontProvider& fonts) : text_(fonts), measurer_(text_) {
  // The binding layer asks its geometry questions here. Handed over in the
  // constructor rather than per navigation because it is this object for the
  // life of the page, and a source that arrived later would leave the first
  // script of a document without one.
  script_.SetGeometrySource(this);
}

const std::vector<std::string>& Page::ConsoleOutput() const { return script_.ConsoleOutput(); }

const std::vector<std::string>& Page::ScriptErrors() const { return script_.ScriptErrors(); }

void Page::AddScript(std::size_t pending_index, std::string source) {
  script_.AddFetched(pending_index, std::move(source));
}

void Page::RunScripts(std::int64_t now_ms) {
  if (document_ == nullptr) {
    return;
  }
  script_.Run(*document_, url_, now_ms);
  // A script can change the tree, so anything derived from it is stale. The
  // box tree is dropped rather than patched: incremental layout is a later
  // decision and a wrong one made early here would be invisible.
  boxes_.reset();
  CollectImages();
}

void Page::Load(std::string_view html, std::string url, csp::PolicyList header_policy) {
  util::PerformanceTrace::Scope scope("engine::Page::Load");

  url_ = std::move(url);
  // Before anything is collected. The policy decides which stylesheets and
  // scripts this document even has.
  policy_.Reset(std::move(header_policy), url_);
  // Before the document goes, and this order is load-bearing: the binding layer
  // holds a reference to it, so dropping the script half after replacing the
  // document would leave that reference dangling for exactly as long as it took
  // the next page's first script to read the tree.
  script_.Detach();
  // A fresh resolver per document. Author sheets belong to the document that
  // carried them, and keeping the old one would let the previous page's CSS
  // style this one.
  resolver_ = css::StyleResolver{};
  document_ = html::ParseDocument(html);
  boxes_.reset();
  // A new document starts at the top, and the scroll offset goes with the
  // layout state rather than surviving it. So does every per-element offset:
  // the keys are pointers into the document that just went, and keeping them
  // would be a use-after-free waiting for the next page to allocate an element
  // at the same address.
  layout_ = LayoutState{};
  scroll_ = ScrollState{};
  content_height_ = 0.0f;
  resources_ = DocumentResources{};
  control_defaults_.clear();

  // The `<meta>` policies and the `<base href>`, before the collections that
  // depend on both: a policy delivered in the document governs that document's
  // own resources, and a `<base>` changes what every relative URL in it means.
  ApplyDocumentHeadPolicy();

  CollectStyleSheets();
  CollectImages();
  // `:target` comes from the address rather than from the markup, so it is set
  // where the address arrives. ADR 0016 §2 -- one copy, and it cannot disagree
  // with what the URL bar says.
  RefreshTargetState();
  if (document_ != nullptr) {
    // Found now, run later: an external script has to arrive before anything
    // after it in the document may run, and what a URL turns into is the
    // loader's problem rather than this one's.
    script_.Collect(*document_, policy_);
  }
  if (document_ != nullptr) {
    document_->ForEachDescendant([&](const dom::Node& node) {
      if (!node.IsElement()) {
        return;
      }
      const auto& element = static_cast<const dom::Element&>(node);
      if (element.TagName() == "input" || element.TagName() == "textarea") {
        control_defaults_.emplace(&element,
                                  std::pair<std::string, bool>{ControlValue(element),
                                                               element.HasAttribute("checked")});
      }
    });
  }
  ExtractTitle();
}

void Page::SetViewport(const css::MediaContext& viewport) {
  viewport_ = viewport;
  // So that a layout forced by a geometry query before the engine's first
  // Layout still runs at the width the document will be shown at.
  layout_.width = viewport.viewport_width;
}

void Page::ExtractTitle() {
  title_.clear();
  if (document_ != nullptr) {
    if (const dom::Element* element = document_->FirstElementByTagName("title")) {
      title_ = DirectText(*element);
    }
  }
  if (title_.empty()) {
    // Never empty: a tab strip has to show something, and "" is not a title,
    // it is a missing one.
    title_ = url_.empty() ? std::string("New Tab") : url_;
  }
}

float Page::Layout(float width) {
  util::PerformanceTrace::Scope scope("engine::Page::Layout");
  layout_.width = width;
  if (document_ == nullptr) {
    content_height_ = 0.0f;
    return 0.0f;
  }
  // Recorded before the layout rather than after: nothing here mutates the
  // document, and reading it afterwards would fold any future mutation made
  // *during* layout into the version this claims to describe.
  layout_.document_version = document_->MutationVersion();
  // The dynamic states that are facts about the document rather than about the
  // pointer, refreshed before the cascade reads them. Here rather than at the
  // dozen places that can change one, for the reason Node::NoteMutation is
  // where it is: missing a call is the failure mode, and a stale `:disabled`
  // bit is a rule that silently stops applying. See ADR 0016 §2.
  RefreshDocumentStates();
  const layout::LayoutEngine engine(resolver_, measurer_, this);
  // The box tree is rebuilt per layout for now. It depends only on the document
  // and the cascade, neither of which changes here, so this is the obvious
  // thing to cache -- and the split between BuildBoxTree and Layout is what
  // makes caching it a change to this function alone.
  boxes_ = engine.BuildBoxTree(*document_);
  content_height_ = engine.Layout(*boxes_, width);
  // The scroll offsets go back on, clamped against the overflow this layout
  // just measured. Layout consults them and does not own them -- ADR 0018 §1 --
  // which is what makes a scrolled menu still scrolled after a script changes a
  // class on the page around it.
  layout::UpdateScrollState(*boxes_, scroll_.offsets);
  return content_height_;
}

void Page::Paint(gfx::DisplayList& out) const {
  util::PerformanceTrace::Scope scope("engine::Page::Paint");
  if (boxes_ == nullptr) {
    return;
  }
  layout::BuildDisplayList(*boxes_, out, gfx::FloatPoint{0.0f, -layout_.scroll_y},
                           gfx::FloatSize{viewport_.viewport_width, viewport_.viewport_height});
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);
}

std::optional<std::string> Page::LinkAt(gfx::FloatPoint document_point) const {
  if (boxes_ == nullptr) {
    return std::nullopt;
  }
  return HitTestLink(*boxes_, document_point, nullptr);
}

std::optional<FormSubmission> Page::SubmitForm(const dom::Element& form,
                                               const dom::Element* submitter) {
  // The event first. A page that adds fields in `onsubmit` -- which is what
  // reddit's interstitial does -- has to have run before the data set is
  // built, and one that calls `preventDefault` must not be submitted at all.
  if (script_.DispatchSubmit(const_cast<dom::Element&>(form))) {
    return std::nullopt;
  }
  return BuildFormSubmission(form, submitter, *document_, url_);
}

std::optional<FormSubmission> Page::TakeScriptFormSubmission() {
  const std::optional<bindings::PendingSubmit> pending = script_.TakePendingSubmit();
  if (!pending.has_value() || pending->form == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  // No `submit` event here: `requestSubmit()` already fired one on its way
  // through and `submit()` fires none by definition. Firing one now would run
  // a page's handler twice for one submission.
  return BuildFormSubmission(*pending->form, pending->submitter, *document_, url_);
}

std::optional<FormSubmission> Page::FormSubmissionRequestAt(gfx::FloatPoint document_point) {
  if (boxes_ == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  const dom::Element* submitter =
      HitTestFormControl(*boxes_, document_point, html::IsSubmitControl);
  if (submitter == nullptr) {
    return std::nullopt;
  }
  const dom::Element* form = html::FormOwner(*submitter, *document_);
  if (form == nullptr) {
    return std::nullopt;
  }
  return SubmitForm(*form, submitter);
}

DispatchOutcome Page::DispatchClickAt(gfx::FloatPoint document_point,
                                   const bindings::PointerInput& pointer) {
  if (boxes_ == nullptr) {
    return {};
  }
  const dom::Element* target = ElementAt(document_point);
  if (target == nullptr) {
    return {};
  }
  DispatchOutcome outcome;
  outcome.ran = script_.HasListeners();
  outcome.prevented = script_.DispatchClick(*const_cast<dom::Element*>(target), pointer);
  return outcome;
}


std::optional<std::uint32_t> Page::NextWakeDelay(std::int64_t now_ms) const {
  const std::optional<std::uint32_t> from_script = script_.NextWakeDelay(now_ms);
  if (scroll_.pending_events.empty()) {
    return from_script;
  }
  // A `scroll` event is owed. Zero rather than a frame interval, because the
  // wheel that caused it already woke the loop and the event is delivered on
  // the way through this turn -- and because a delay would let a second notch
  // arrive first, which is what the throttling is *for* rather than something
  // to schedule around. The queue empties in RunDueWork and the loop goes back
  // to blocking; a settled page never reaches this line.
  return 0;
}

bool Page::RunDueWork(std::int64_t now_ms) {
  bool ran = DispatchPendingScrollEvents();
  ran = script_.RunDueWork(now_ms) || ran;
  if (!ran) {
    return false;
  }
  InvalidateLayout();
  return true;
}

void Page::SetNetworkSource(bindings::NetworkSource* network) {
  script_.SetNetworkSource(network);
}

bool Page::DeliverFetchResponse(std::uint64_t id, const bindings::ScriptResponse& response) {
  return script_.DeliverFetchResponse(id, response);
}

bool Page::DeliverObservations(std::int64_t now_ms) {
  if (document_ == nullptr) {
    return false;
  }
  // A loop, and the bound is the reason it is one. A `ResizeObserver` callback
  // that resizes what it observes is a page fighting itself: each delivery
  // makes the next one have something to say, and the specification's answer is
  // a depth limit rather than a promise that it settles. Without one this is a
  // hang a page can cause on purpose.
  //
  // The relayout inside the loop is what makes the second pass mean anything:
  // a callback that moved something must be measured against where it moved it
  // to, not against where it was.
  static constexpr int kObservationDepthLimit = 8;
  bool ran = false;
  int depth = 0;
  for (; depth < kObservationDepthLimit; ++depth) {
    EnsureLayoutClean();
    if (!script_.DeliverViewObservations(now_ms)) {
      break;
    }
    ran = true;
  }
  if (depth == kObservationDepthLimit) {
    AddPerformanceCounter(PerfCounterId::ViewResizeLoopLimit);
  }
  if (ran) {
    // Whatever the last callback did has to be on screen, and the caller paints
    // from the box tree rather than from the document.
    EnsureLayoutClean();
  }
  return ran;
}

const dom::Element* Page::ElementAt(gfx::FloatPoint document_point) const {
  if (boxes_ == nullptr) {
    return nullptr;
  }
  return HitTestElement(*boxes_, document_point, nullptr);
}

void Page::InvalidateLayout() {
  boxes_.reset();
  CollectImages();
}

bool Page::FocusFromClickAt(gfx::FloatPoint document_point) {
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  // The nearest focusable ancestor of what was hit, which is what makes a click
  // on the text inside a `<button>` focus the button rather than nothing. Null
  // when there is none, and that is not a failure: a click on the background
  // blurs whatever had focus, which is the only way to leave a field with the
  // mouse.
  //
  // Here rather than in PageEditing.cpp with the rest of the focus model
  // because this is the one part of it that is a *hit test*, and the hit-test
  // walk is in this file with the four others that use it.
  const dom::Element* target = ElementAt(document_point);
  while (target != nullptr && !html::IsFocusable(*target)) {
    const dom::Node* parent = target->Parent();
    target = parent != nullptr && parent->IsElement() ? static_cast<const dom::Element*>(parent)
                                                      : nullptr;
  }
  // Not keyboard-driven, so no focus ring: a ring on every click is the reason
  // authors write `outline: none`, which is worse for the user than either
  // behaviour. ADR 0017 §4.
  return MoveFocus(const_cast<dom::Element*>(target), false);
}

bool Page::ActivateCheckableInputAt(gfx::FloatPoint document_point) {
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  dom::Element* hit = HitTestFormControl(*boxes_, document_point, html::IsCheckableInput);
  if (hit == nullptr) {
    return false;
  }
  dom::Element& input = *hit;
  if (html::IsCheckboxInput(input)) {
    if (input.HasAttribute("checked")) {
      input.RemoveAttribute("checked");
    } else {
      input.SetAttribute("checked", "");
    }
    boxes_.reset();
    return true;
  }
  if (input.HasAttribute("checked")) {
    return false;
  }
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& candidate = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (IsRadioGroupPeer(candidate, input, *document_)) {
      candidate.RemoveAttribute("checked");
    }
  });
  input.SetAttribute("checked", "");
  boxes_.reset();
  return true;
}

bool Page::ResetFormAt(gfx::FloatPoint document_point) {
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  const dom::Element* reset = HitTestFormControl(*boxes_, document_point, html::IsResetControl);
  if (reset == nullptr) {
    return false;
  }
  const dom::Element* form = html::FormOwner(*reset, *document_);
  if (form == nullptr) {
    return false;
  }
  bool changed = false;
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (!html::BelongsToForm(element, *form, *document_)) {
      return;
    }
    if (element.TagName() != "input" && element.TagName() != "textarea") {
      return;
    }
    const auto found = control_defaults_.find(&element);
    if (found == control_defaults_.end()) {
      return;
    }
    const auto& [value, checked] = found->second;
    if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
      if (checked) {
        changed = !element.HasAttribute("checked") || changed;
        element.SetAttribute("checked", "");
      } else {
        changed = element.RemoveAttribute("checked") || changed;
      }
    } else if (IsValueResettableControl(element)) {
      const std::string* current = element.GetAttribute("value");
      changed = current == nullptr || *current != value || changed;
      element.SetAttribute("value", value);
    }
  });
  if (!changed) {
    return false;
  }
  boxes_.reset();
  return true;
}


}  // namespace microbrowser::engine

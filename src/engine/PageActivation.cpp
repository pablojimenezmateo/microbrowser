#include "engine/Page.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>
#include <string>

#include "dom/FlatTree.h"
#include "engine/PageGestures.h"
#include "html/FormControl.h"
#include "util/StringUtil.h"

// What a click *does* after the event has been dispatched and nothing
// cancelled it: the DOM's activation behaviour.
//
// Its own translation unit because Page.cpp reached the module's line cap, and
// the cap means a missing file rather than a bigger one. The seam is a real
// one: everything else in Page coordinates a document -- loading it, laying it
// out, painting it -- and this is the one walk that asks "what did the user
// just do to it", which is a question with a list of answers rather than an
// algorithm.
//
// **There is one of these and both callers reach it.** A real pointer release
// arrives from EngineInput.cpp; an `element.click()` a script ran arrives
// through Page::ApplyScriptActivation, which the engine drains after the turn.
// Two copies of this walk is how a checkbox comes to toggle under the mouse
// and not under script, which is exactly the state this browser was in until
// the second caller existed.

namespace microbrowser::engine {

namespace {

// The first `<summary>` child of a `<details>`, which is the only one with an
// activation behaviour. A second summary in the same details is content.
const dom::Node* FirstSummaryChild(const dom::Element& details) {
  for (const std::unique_ptr<dom::Node>& child : details.Children()) {
    if (child->IsElement()) {
      const auto& element = static_cast<const dom::Element&>(*child);
      if (element.Namespace().IsHtml() && element.LocalName() == "summary") {
        return child.get();
      }
    }
  }
  return nullptr;
}

// Composed-tree parent for activation, matching the click and focus walks and
// EventDispatch's propagation: a shadow root has no parent by design (ADR 0019
// §2), and crossing to the host is how a click on an `<img>` inside a
// component reaches the anchor that wraps it. A local copy of Page.cpp's, and
// small enough that sharing it would cost a header for four lines.
dom::Element* ComposedParentElement(dom::Element* element) {
  if (element == nullptr) {
    return nullptr;
  }
  if (dom::Node* parent = element->Parent(); parent != nullptr && parent->IsElement()) {
    return static_cast<dom::Element*>(parent);
  }
  if (const dom::Element* host = dom::ShadowHostOf(*element)) {
    return const_cast<dom::Element*>(host);
  }
  return nullptr;
}

}  // namespace

// --- Pre-click activation ------------------------------------------------------------------
//
// HTML runs a checkbox's toggle **before** the `click` event is dispatched and undoes it if the
// event is cancelled ("pre-click activation steps" and "canceled activation steps"). This engine
// ran it after, which is observably different in the one place a page looks: inside its own
// handler, `checkbox.checked` answered the *old* value. `the-input-element/checkbox.html` measures
// exactly that and four of its six subtests are this one difference.
//
// Running it here rather than in the binding layer is what keeps one algorithm: both a pointer
// release and an `element.click()` reach `DomBindings::DispatchClick`, and it calls these hooks.
bool Page::PreClickActivation(dom::Element& click_target) {
  pre_click_element_ = nullptr;
  if (document_ == nullptr) {
    return false;
  }
  for (dom::Element* at = &click_target; at != nullptr; at = ComposedParentElement(at)) {
    // The same stop conditions the walk below has, and for the same reason: a `<button type=button>`
    // has an empty activation behaviour, and that *is* the answer rather than a reason to keep
    // looking at its ancestors.
    if (at->Namespace().IsHtml() &&
        (at->LocalName() == "button" || at->LocalName() == "input")) {
      const std::string* type = at->GetAttribute("type");
      if (type != nullptr && util::AsciiLowerCase(*type) == "button") {
        return false;
      }
    }
    if (html::IsSubmitControl(*at) || html::IsResetControl(*at)) {
      return false;
    }
    if (html::IsCheckableInput(*at)) {
      // Remembered before it changes, and as the **set of everything that was checked** rather than
      // as one element's boolean: a radio's pre-click step clears every peer in its group, so
      // undoing it has to put the one that *was* checked back rather than leave the group empty.
      // Recorded by scanning rather than by asking which elements are peers, because the grouping
      // rule lives in one place and a second reading of it is a second answer.
      pre_click_element_ = at;
      pre_click_finished_ = false;
      pre_click_indeterminate_ = at->HasState(dom::ElementState::Indeterminate);
      // HTML's pre-click activation steps set a checkbox's indeterminate flag to false, and the
      // canceled steps put it back. It is IDL-only state with no attribute behind it, which is why
      // it is a bit on the element rather than something derivable from the tree.
      at->SetState(dom::ElementState::Indeterminate, false);
      pre_click_checked_.clear();
      document_->ForEachDescendant([&](const dom::Node& node) {
        if (!node.IsElement()) {
          return;
        }
        auto& candidate = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
        if (html::IsCheckableInput(candidate) && candidate.HasAttribute("checked")) {
          pre_click_checked_.push_back(&candidate);
        }
      });
      return ActivateCheckableInputOn(*at);
    }
    if (at->TagName() == "a" || (at->Namespace().IsHtml() && at->LocalName() == "summary")) {
      return false;
    }
  }
  return false;
}

void Page::CancelClickActivation() {
  if (pre_click_element_ == nullptr || document_ == nullptr) {
    return;
  }
  pre_click_element_->SetState(dom::ElementState::Indeterminate, pre_click_indeterminate_);
  pre_click_element_ = nullptr;
  pre_click_finished_ = false;
  // Every checkable put back to what the snapshot said, which restores a radio group as a whole.
  // A handler that changed some *other* checkbox from inside the click loses that change, and that
  // is what the specification says: the canceled activation steps restore the state the pre-click
  // steps captured.
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& candidate = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (!html::IsCheckableInput(candidate)) {
      return;
    }
    const bool was = std::find(pre_click_checked_.begin(),
                               pre_click_checked_.end(),
                               &candidate) != pre_click_checked_.end();
    if (was) {
      candidate.SetAttribute("checked", "");
    } else {
      candidate.RemoveAttribute("checked");
    }
  });
  pre_click_checked_.clear();
  InvalidateBoxTree();
}

// The rest of a checkable's activation behaviour, **synchronously inside the click**. HTML fires
// `input` and then `change` as part of the activation behaviour, and a page reads them there: the
// suite's own `the-input-element/checkbox.html` calls `checkbox.click()` and asserts both handlers
// have already run on the line after it. Deferring them to the turn boundary -- which is right for
// a form submission and for following an `href`, because both replace the document -- is wrong for
// two events that change nothing outside the DOM.
void Page::FinishClickActivation() {
  if (pre_click_element_ == nullptr || pre_click_finished_) {
    return;
  }
  pre_click_finished_ = true;
  dom::Element& input = *pre_click_element_;
  script_.DispatchMediaEvent(input, "input");
  script_.DispatchMediaEvent(input, "change");
}

ClickActivation Page::ResolveClickActivation(dom::Element* click_target) {
  ClickActivation activation;
  if (click_target == nullptr || document_ == nullptr) {
    return activation;
  }
  EnsureLayoutClean();

  auto parent_element = [](dom::Element* element) -> dom::Element* {
    return ComposedParentElement(element);
  };

  for (dom::Element* at = click_target; at != nullptr; at = parent_element(at)) {
    // **A control with no activation behaviour still stops the search.** A
    // `<button type=button>` does nothing when clicked -- and clicking one
    // inside an `<a>` must therefore do *nothing*, not follow the link. The
    // walk is for the nearest activatable ancestor, and a button is one; that
    // its behaviour is empty is the answer rather than a reason to keep
    // looking. Without this, a button inside an anchor navigated.
    if (at->Namespace().IsHtml() &&
        (at->LocalName() == "button" || at->LocalName() == "input")) {
      const std::string* type = at->GetAttribute("type");
      if (type != nullptr && util::AsciiLowerCase(*type) == "button") {
        return activation;
      }
    }
    if (html::IsSubmitControl(*at)) {
      const dom::Element* form = html::FormOwner(*at, *document_);
      if (form != nullptr) {
        activation.form = SubmitForm(*form, at);
      }
      return activation;
    }
    if (html::IsResetControl(*at)) {
      activation.reset_form = ResetFormOn(*at);
      return activation;
    }
    if (html::IsCheckableInput(*at)) {
      // **Already toggled**, by `PreClickActivation` before the event went out. What is left of the
      // activation behaviour is the pair of events, in the order HTML fires them and the order
      // `the-input-element/checkbox.html` asserts: `input`, then `change`, both after `click`.
      // Already toggled when a click event went out; toggled *here* when none did. A document with
      // no script has no binding layer and therefore no dispatch, and a checkbox on one still has
      // to work -- which is why this is a fallback rather than an assertion.
      const bool pre_toggled = pre_click_element_ == at;
      const bool already_finished = pre_toggled && pre_click_finished_;
      pre_click_element_ = nullptr;
      pre_click_finished_ = false;
      activation.toggled_checkable = pre_toggled || ActivateCheckableInputOn(*at);
      // The events, unless `FinishClickActivation` already fired them inside the dispatch. A
      // document with no script has no binding layer and therefore no dispatch, so this is the path
      // that keeps a checkbox working on one.
      if (activation.toggled_checkable && !already_finished) {
        script_.DispatchMediaEvent(*at, "input");
        script_.DispatchMediaEvent(*at, "change");
      }
      return activation;
    }
    // `<summary>` opens and closes the `<details>` it is the summary of --
    // the **first** summary child, which is the only one with an activation
    // behaviour; a second `<summary>` in the same details is ordinary content.
    // The state goes in the `open` *content* attribute, so the cascade and
    // `details.open` read one fact rather than two.
    if (at->Namespace().IsHtml() && at->LocalName() == "summary") {
      dom::Node* parent = at->Parent();
      if (parent != nullptr && parent->IsElement()) {
        auto& details = static_cast<dom::Element&>(*parent);
        if (details.Namespace().IsHtml() && details.LocalName() == "details" &&
            FirstSummaryChild(details) == at) {
          if (details.HasAttribute("open")) {
            details.RemoveAttribute("open");
          } else {
            details.SetAttribute("open", "");
          }
          // The `toggle` event, which is what a page listens to rather than
          // polling the attribute. Through the same trusted entry point every
          // other browser-produced event uses.
          script_.DispatchMediaEvent(details, "toggle");
          activation.toggled_details = true;
          return activation;
        }
      }
    }
    if (at->TagName() == "a") {
      const std::string* href = at->GetAttribute("href");
      if (href != nullptr && !href->empty()) {
        activation.href = *href;
        return activation;
      }
    }
  }

  if (ToggleMediaPlaybackOn(*click_target)) {
    activation.toggled_media = true;
  }
  return activation;
}

}  // namespace microbrowser::engine

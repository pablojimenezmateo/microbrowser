#include "engine/Page.h"

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
      activation.toggled_checkable = ActivateCheckableInputOn(*at);
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

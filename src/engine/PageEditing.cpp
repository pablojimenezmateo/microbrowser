#include "engine/Page.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/FormAlgorithms.h"
#include "html/Focus.h"
#include "html/FormControl.h"
#include "util/PerformanceCounters.h"

// Focus, and what a key does to whatever has it.
//
// Split from Page.cpp because that file reached the module's line cap, and the
// cap means a missing translation unit rather than a bigger file. This is a
// real seam and not an arbitrary cut: everything in Page.cpp turns a document
// into boxes and pixels, and everything here is about the one element the user
// is acting on -- which element that is, what a keydown reaches, and what the
// character it carries does to the value.
//
// **Focus lives on `dom::Document` and there is no copy of it here.** ADR 0017
// §4: one focused element per document, the engine moves it, the binding layer
// reports it as `document.activeElement`. This file used to keep a
// `focused_text_control_` on Page, which was a second copy that only ever held
// a text field -- so `input.focus()` from a script and the element a click
// focused were two different facts, and a page that focused a field and
// expected to be typed into was broken by construction.
//
// The whole file is one caret's worth of editing: the caret is at the end of
// the value and nowhere else. That is why deleting backwards has to walk a
// UTF-8 boundary and deleting forwards does not exist. A real caret model is
// what would let it, and it is not here yet.

namespace microbrowser::engine {

using util::AddPerformanceCounter;
using util::PerfCounterId;

namespace {

bool IsUtf8Continuation(unsigned char byte) {
  return (byte & 0xC0u) == 0x80u;
}

std::size_t ExpectedUtf8ContinuationCount(unsigned char lead) {
  if ((lead & 0x80u) == 0u) {
    return 0;
  }
  if ((lead & 0xE0u) == 0xC0u) {
    return 1;
  }
  if ((lead & 0xF0u) == 0xE0u) {
    return 2;
  }
  if ((lead & 0xF8u) == 0xF0u) {
    return 3;
  }
  return 0;
}

std::size_t PreviousUtf8Boundary(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const std::size_t last = text.size() - 1;
  if (!IsUtf8Continuation(static_cast<unsigned char>(text[last]))) {
    return last;
  }
  std::size_t lead = last;
  while (lead > 0 && IsUtf8Continuation(static_cast<unsigned char>(text[lead]))) {
    --lead;
  }
  const std::size_t continuation_count = last - lead;
  if (!IsUtf8Continuation(static_cast<unsigned char>(text[lead])) &&
      ExpectedUtf8ContinuationCount(static_cast<unsigned char>(text[lead])) == continuation_count) {
    return lead;
  }
  return last;
}
}  // namespace

dom::Element* Page::FocusedElement() const {
  return document_ == nullptr ? nullptr : document_->Focus().element;
}

bool Page::FocusIsVisible() const {
  return document_ != nullptr && document_->Focus().visible;
}

// The focused element when it is one a key can type into, and null otherwise.
// Every editing routine below starts here, so "is this thing editable" is
// answered once rather than four times.
dom::Element* Page::MutableFocusedTextControl() const {
  dom::Element* focused = FocusedElement();
  return focused != nullptr && html::IsMutableTextControl(*focused) ? focused : nullptr;
}

bool Page::MoveFocus(dom::Element* target, bool visible) {
  if (document_ == nullptr) {
    return false;
  }
  // Through the binding layer when there is one, because it fires the four
  // events on the way; it writes the same document the branch below does.
  //
  // No interpreter means no handlers and no events -- but focus still moves, or
  // a page with no script would have no keyboard at all. The focusability rule
  // is repeated in that branch rather than skipped: a click on a `<div>` must
  // blur the field rather than focus the div, whether or not the page has
  // script.
  bool moved = false;
  if (script_.HasListeners()) {
    moved = script_.MoveFocus(target, visible);
  } else if (target == nullptr || html::IsFocusable(*target)) {
    moved = document_->Focus().element != target;
    document_->SetFocus(target, visible);
  }
  // Counted here and not in either branch, so the number means "focus moved"
  // rather than "focus moved on a page with script".
  if (moved) {
    AddPerformanceCounter(PerfCounterId::FocusMoves);
  }
  return moved;
}

bool Page::MoveFocusByTab(bool backwards) {
  if (document_ == nullptr) {
    return false;
  }
  // The one part of the focus model whose cost grows with the document, and
  // nothing caches it: the answer changes whenever the tree or an attribute
  // does, and a stale tab order sends a keystroke to the wrong element. The
  // counters are here so that "is an index worth its invalidation" is a
  // measurement rather than an argument -- candidates over walks is the
  // average document's answer, and on a page with none this costs one walk.
  AddPerformanceCounter(PerfCounterId::FocusTabWalks);
  // Everything Tab can stop on, in the specification's order: positive
  // `tabindex` first in increasing order, then everything else in tree order.
  // A stable sort is what keeps the second group in document order and what
  // keeps two elements with the same positive index in it.
  std::vector<dom::Element*> order;
  document_->ForEachDescendant([&order](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (html::IsTabReachable(element)) {
      order.push_back(&element);
    }
  });
  AddPerformanceCounter(PerfCounterId::FocusTabCandidates, order.size());
  std::stable_sort(order.begin(), order.end(), [](const dom::Element* a, const dom::Element* b) {
    const int left = html::TabIndex(*a).value_or(0);
    const int right = html::TabIndex(*b).value_or(0);
    if ((left > 0) != (right > 0)) {
      return left > 0;
    }
    return left > 0 && right > 0 ? left < right : false;
  });
  if (order.empty()) {
    return false;
  }

  const dom::Element* focused = FocusedElement();
  const auto at = std::find(order.begin(), order.end(), focused);
  std::size_t next = 0;
  if (at == order.end()) {
    // Nothing focused, or focus is on something Tab does not stop at. Tab goes
    // to the first, Shift+Tab to the last, which is what starting at either end
    // of the order means.
    next = backwards ? order.size() - 1 : 0;
  } else {
    const std::size_t index = static_cast<std::size_t>(at - order.begin());
    // Wrapping rather than stopping. The alternative is focus leaving the
    // document for the browser chrome, and ADR 0017 §4 keeps that decision in
    // `src/app`: the engine has no chrome to hand focus to and must not invent
    // one.
    next = backwards ? (index + order.size() - 1) % order.size() : (index + 1) % order.size();
  }
  return MoveFocus(order[next], true);
}

DispatchOutcome Page::DispatchKeyToFocus(const bindings::KeyInput& key) {
  // To the focused element, or to the document when nothing has focus. ADR
  // 0017 §4: focus is the input router, and hit testing is consulted only for
  // pointer events. That is the split that makes a text field work without a
  // second mechanism.
  DispatchOutcome outcome;
  outcome.ran = script_.HasListeners();
  outcome.prevented = script_.DispatchKey(FocusedElement(), key);
  return outcome;
}

bool Page::InsertTextIntoFocusedTextControl(std::string_view text) {
  dom::Element* control = MutableFocusedTextControl();
  if (control == nullptr || text.empty()) {
    return false;
  }
  std::string value = ControlValue(*control);
  const std::size_t limit = TextControlValueLimitBytes(*control);
  if (value.size() >= limit) {
    return false;
  }
  const std::size_t room = limit - value.size();
  value.append(text.substr(0, room));
  control->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

bool Page::DeleteBackwardFromFocusedTextControl() {
  dom::Element* control = MutableFocusedTextControl();
  if (control == nullptr) {
    return false;
  }
  std::string value = ControlValue(*control);
  if (value.empty()) {
    return false;
  }
  value.erase(PreviousUtf8Boundary(value));
  control->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

std::optional<FormSubmission> Page::FocusedFormSubmission() {
  // Any text control, not only a mutable one: implicit submission is what Enter
  // does in a field, and a `readonly` field is still a field. Asking for
  // mutability here would make Enter do nothing in one, which is a different
  // behaviour from not being able to type in it.
  dom::Element* control = FocusedElement();
  if (control == nullptr || document_ == nullptr || !html::IsTextControl(*control)) {
    return std::nullopt;
  }
  const dom::Element* form = html::FormOwner(*control, *document_);
  if (form == nullptr) {
    return std::nullopt;
  }
  return SubmitForm(*form, nullptr);
}
}  // namespace microbrowser::engine

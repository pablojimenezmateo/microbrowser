#include "engine/Page.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine/FormAlgorithms.h"
#include "html/FormControl.h"

// The focused text control, and what a key does to it.
//
// Split from Page.cpp because that file reached the module's line cap, and the
// cap means a missing translation unit rather than a bigger file. This is a
// real seam and not an arbitrary cut: everything in Page.cpp turns a document
// into boxes and pixels, and everything here is about the one element the user
// is typing into -- which element that is, what a keydown reaches, and what the
// character it carries does to the value.
//
// The whole file is one caret's worth of editing: the caret is at the end of
// the value and nowhere else. That is why deleting backwards has to walk a
// UTF-8 boundary and deleting forwards does not exist. A real caret model is
// what would let it, and it is not here yet.

namespace microbrowser::engine {

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

DispatchOutcome Page::DispatchKeyToFocus(const bindings::KeyInput& key) {
  // To the focused control, or to the document when nothing is focused. ADR
  // 0017 §4: focus is the input router, and hit testing is consulted only for
  // pointer events. The focus model itself -- `activeElement`, `focus()`, Tab
  // order -- is session 10; what exists here is the one element a click can
  // focus, which is what a text field already needed.
  DispatchOutcome outcome;
  outcome.ran = script_.HasListeners();
  outcome.prevented = script_.DispatchKey(focused_text_control_, key);
  return outcome;
}
bool Page::InsertTextIntoFocusedTextControl(std::string_view text) {
  if (focused_text_control_ == nullptr || text.empty() ||
      !html::IsMutableTextControl(*focused_text_control_)) {
    return false;
  }
  std::string value = ControlValue(*focused_text_control_);
  const std::size_t limit = TextControlValueLimitBytes(*focused_text_control_);
  if (value.size() >= limit) {
    return false;
  }
  const std::size_t room = limit - value.size();
  value.append(text.substr(0, room));
  focused_text_control_->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

bool Page::DeleteBackwardFromFocusedTextControl() {
  if (focused_text_control_ == nullptr || !html::IsMutableTextControl(*focused_text_control_)) {
    return false;
  }
  std::string value = ControlValue(*focused_text_control_);
  if (value.empty()) {
    return false;
  }
  value.erase(PreviousUtf8Boundary(value));
  focused_text_control_->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

std::optional<FormSubmission> Page::FocusedFormSubmission() {
  if (focused_text_control_ == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  const dom::Element* form = html::FormOwner(*focused_text_control_, *document_);
  if (form == nullptr) {
    return std::nullopt;
  }
  return SubmitForm(*form, nullptr);
}
}  // namespace microbrowser::engine

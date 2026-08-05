#include "html/Focus.h"

#include <limits>
#include <string>
#include <string_view>

#include "html/FormControl.h"
#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

// The specification's "rules for parsing integers", with a saturation the
// specification leaves to the implementation.
//
// The attribute is attacker-controlled text, so the arithmetic below cannot be
// allowed to wrap: `tabindex="99999999999999"` orders the element last, and an
// overflow that turned it negative would order it *out of the Tab order
// entirely*, which is a page controlling what the keyboard can reach.
std::optional<int> ParseTabIndex(std::string_view text) {
  text = util::TrimAscii(text);
  if (text.empty()) {
    return std::nullopt;
  }
  bool negative = false;
  if (text.front() == '-' || text.front() == '+') {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  constexpr std::int64_t kCeiling = std::numeric_limits<int>::max();
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    if (value <= kCeiling) {
      value = value * 10 + (c - '0');
    }
    if (value > kCeiling) {
      value = kCeiling;
    }
  }
  return static_cast<int>(negative ? -value : value);
}

bool IsEditingHost(const dom::Element& element) {
  const std::string* editable = element.GetAttribute("contenteditable");
  if (editable == nullptr) {
    return false;
  }
  // The attribute is an enumerated one, and the empty string means true --
  // `<div contenteditable>` is the form every editor writes.
  return editable->empty() || !util::EqualsAsciiCaseInsensitive(*editable, "false");
}

// Focusable because of what it is, before `tabindex` gets a say.
bool IsFocusableByDefault(const dom::Element& element) {
  const std::string_view tag = element.TagName();
  if (tag == "input") {
    return !IsHiddenInput(element);
  }
  if (tag == "textarea" || tag == "select" || tag == "button") {
    return true;
  }
  if (tag == "a" || tag == "area") {
    // A link without `href` is not a link. It is the anchor the same element
    // was used as before ids existed, and nothing about it is interactive.
    return element.HasAttribute("href");
  }
  return IsEditingHost(element);
}

}  // namespace

std::optional<int> TabIndex(const dom::Element& element) {
  const std::string* value = element.GetAttribute("tabindex");
  return value == nullptr ? std::nullopt : ParseTabIndex(*value);
}

bool IsFocusable(const dom::Element& element) {
  // Disabled first, and for every element rather than for form controls only:
  // a disabled control is not focusable by any route, and a `fieldset` around
  // it disables everything inside.
  if (IsDisabledFormControl(element)) {
    return false;
  }
  // `hidden` on the element *or on any ancestor*. A closed menu is a container
  // with `hidden` on it and its items still inside, so checking the element
  // alone let Tab walk into a menu the page had closed -- and focus is the
  // input router, so that is a keystroke delivered to something the user
  // cannot see. The walk is why this is not a one-line predicate; it costs the
  // depth of the tree per call, which Tab pays once per candidate and only
  // when a key is pressed.
  //
  // Only the attribute, not `display: none`: that is a computed style and this
  // module may name `util url dom` and nothing else. An element with no box is
  // the same bug through a different property, and closing it needs the
  // element state bits of ADR 0016 -- session 11.
  for (const dom::Node* at = &element; at != nullptr; at = at->Parent()) {
    if (at->IsElement() && static_cast<const dom::Element*>(at)->HasAttribute("hidden")) {
      return false;
    }
  }
  return TabIndex(element).has_value() || IsFocusableByDefault(element);
}

bool IsTabReachable(const dom::Element& element) {
  if (!IsFocusable(element)) {
    return false;
  }
  const std::optional<int> index = TabIndex(element);
  // A negative `tabindex` is focusable and not tab-reachable, which is the
  // whole reason the attribute takes negative values: a scripted menu focuses
  // its own items without putting every one of them in the Tab order.
  return !index.has_value() || *index >= 0;
}

}  // namespace microbrowser::html

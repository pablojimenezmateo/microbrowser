#include "html/FormControl.h"

#include <string>

#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

constexpr std::string_view kEditableTextTypes[] = {
    "text", "search", "password", "email", "url", "tel", "number",
};

}  // namespace

std::string_view InputType(const dom::Element& element) {
  const std::string* type = element.GetAttribute("type");
  return type == nullptr ? std::string_view("text") : std::string_view(*type);
}

bool IsInputType(const dom::Element& element, std::string_view expected) {
  return element.TagName() == "input" &&
         util::EqualsAsciiCaseInsensitive(InputType(element), expected);
}

bool IsHiddenInput(const dom::Element& element) {
  return IsInputType(element, "hidden");
}

bool IsSubmitInput(const dom::Element& element) {
  return IsInputType(element, "submit");
}

bool IsResetInput(const dom::Element& element) {
  return IsInputType(element, "reset");
}

bool IsCheckboxInput(const dom::Element& element) {
  return IsInputType(element, "checkbox");
}

bool IsRadioInput(const dom::Element& element) {
  return IsInputType(element, "radio");
}

bool IsCheckableInput(const dom::Element& element) {
  return !element.HasAttribute("disabled") && (IsCheckboxInput(element) || IsRadioInput(element));
}

bool IsEditableTextInput(const dom::Element& element) {
  if (element.HasAttribute("disabled")) {
    return false;
  }
  for (const std::string_view type : kEditableTextTypes) {
    if (IsInputType(element, type)) {
      return true;
    }
  }
  return false;
}

bool IsMutableTextInput(const dom::Element& element) {
  return IsEditableTextInput(element) && !element.HasAttribute("readonly");
}

bool IsPasswordInput(const dom::Element& element) {
  return IsInputType(element, "password");
}

}  // namespace microbrowser::html

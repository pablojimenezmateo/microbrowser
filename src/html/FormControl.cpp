#include "html/FormControl.h"

#include <optional>
#include <string>
#include <vector>

#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

constexpr std::string_view kEditableTextTypes[] = {
    "text", "search", "password", "email", "url", "tel", "number",
};

const dom::Element* SelectedOption(const dom::Element& select) {
  const dom::Element* first = nullptr;
  const dom::Element* selected = nullptr;
  select.ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() != "option") {
      return;
    }
    if (first == nullptr) {
      first = &element;
    }
    if (selected == nullptr && element.HasAttribute("selected")) {
      selected = &element;
    }
  });
  return selected != nullptr ? selected : first;
}

std::string OptionValue(const dom::Element& option) {
  if (const std::string* value = option.GetAttribute("value")) {
    return *value;
  }
  return option.TextContent();
}

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

bool IsSubmitControl(const dom::Element& element) {
  if (IsSubmitInput(element)) {
    return true;
  }
  if (element.TagName() != "button") {
    return false;
  }
  const std::string* type = element.GetAttribute("type");
  return type == nullptr || type->empty() ||
         (!util::EqualsAsciiCaseInsensitive(*type, "button") &&
          !util::EqualsAsciiCaseInsensitive(*type, "reset"));
}

bool IsResetControl(const dom::Element& element) {
  if (IsResetInput(element)) {
    return true;
  }
  if (element.TagName() != "button") {
    return false;
  }
  const std::string* type = element.GetAttribute("type");
  return type != nullptr && util::EqualsAsciiCaseInsensitive(*type, "reset");
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

bool IsTextInputType(const dom::Element& element) {
  if (element.TagName() != "input") {
    return false;
  }
  for (const std::string_view type : kEditableTextTypes) {
    if (IsInputType(element, type)) {
      return true;
    }
  }
  return false;
}

bool IsEditableTextInput(const dom::Element& element) {
  return !element.HasAttribute("disabled") && IsTextInputType(element);
}

bool IsMutableTextInput(const dom::Element& element) {
  return IsEditableTextInput(element) && !element.HasAttribute("readonly");
}

bool IsPasswordInput(const dom::Element& element) {
  return IsInputType(element, "password");
}

bool IsTextareaElement(const dom::Element& element) {
  return element.TagName() == "textarea";
}

bool IsTextControl(const dom::Element& element) {
  return IsTextInputType(element) || IsTextareaElement(element);
}

bool IsEditableTextControl(const dom::Element& element) {
  return !element.HasAttribute("disabled") && IsTextControl(element);
}

bool IsMutableTextControl(const dom::Element& element) {
  return IsEditableTextControl(element) && !element.HasAttribute("readonly");
}

bool IsSelectElement(const dom::Element& element) {
  return element.TagName() == "select";
}

std::optional<std::string> SelectedOptionText(const dom::Element& select) {
  if (!IsSelectElement(select)) {
    return std::nullopt;
  }
  const dom::Element* option = SelectedOption(select);
  return option == nullptr ? std::nullopt : std::optional<std::string>(option->TextContent());
}

std::optional<std::string> SelectedOptionValue(const dom::Element& select) {
  if (!IsSelectElement(select)) {
    return std::nullopt;
  }
  const dom::Element* option = SelectedOption(select);
  if (option == nullptr) {
    return std::nullopt;
  }
  return OptionValue(*option);
}

std::vector<std::string> SelectedOptionValues(const dom::Element& select) {
  std::vector<std::string> values;
  if (!IsSelectElement(select)) {
    return values;
  }
  const bool multiple = select.HasAttribute("multiple");
  const dom::Element* first = nullptr;
  select.ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() != "option") {
      return;
    }
    if (first == nullptr) {
      first = &element;
    }
    if (element.HasAttribute("selected")) {
      values.push_back(OptionValue(element));
    }
  });
  if (!multiple && values.empty() && first != nullptr) {
    values.push_back(OptionValue(*first));
  }
  return values;
}

}  // namespace microbrowser::html

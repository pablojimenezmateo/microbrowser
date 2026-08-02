#pragma once

#include <string_view>

#include "dom/Node.h"

namespace microbrowser::html {

std::string_view InputType(const dom::Element& element);
bool IsInputType(const dom::Element& element, std::string_view expected);
bool IsHiddenInput(const dom::Element& element);
bool IsSubmitInput(const dom::Element& element);
bool IsCheckboxInput(const dom::Element& element);
bool IsRadioInput(const dom::Element& element);
bool IsCheckableInput(const dom::Element& element);
bool IsEditableTextInput(const dom::Element& element);
bool IsMutableTextInput(const dom::Element& element);
bool IsPasswordInput(const dom::Element& element);

}  // namespace microbrowser::html

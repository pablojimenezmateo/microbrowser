#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Node.h"

namespace microbrowser::html {

std::string_view InputType(const dom::Element& element);
bool IsInputType(const dom::Element& element, std::string_view expected);
bool IsDisabledFormControl(const dom::Element& element);
bool IsHiddenInput(const dom::Element& element);
bool IsSubmitInput(const dom::Element& element);
bool IsResetInput(const dom::Element& element);
bool IsSubmitControl(const dom::Element& element);
bool IsResetControl(const dom::Element& element);
bool IsCheckboxInput(const dom::Element& element);
bool IsRadioInput(const dom::Element& element);
bool IsCheckableInput(const dom::Element& element);
bool IsTextInputType(const dom::Element& element);
bool IsEditableTextInput(const dom::Element& element);
bool IsMutableTextInput(const dom::Element& element);
bool IsPasswordInput(const dom::Element& element);
bool IsTextareaElement(const dom::Element& element);
bool IsTextControl(const dom::Element& element);
bool IsEditableTextControl(const dom::Element& element);
bool IsMutableTextControl(const dom::Element& element);
bool IsSelectElement(const dom::Element& element);
const dom::Element* FormOwner(const dom::Element& element, const dom::Document& document);
bool BelongsToForm(const dom::Element& element,
                   const dom::Element& form,
                   const dom::Document& document);
std::optional<std::string> SelectedOptionText(const dom::Element& select);
std::optional<std::string> SelectedOptionValue(const dom::Element& select);
std::vector<std::string> SelectedOptionValues(const dom::Element& select);

}  // namespace microbrowser::html

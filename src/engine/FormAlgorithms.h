#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine/Page.h"

namespace microbrowser::dom {
class Document;
class Element;
}  // namespace microbrowser::dom

namespace microbrowser::engine {

std::string ControlValue(const dom::Element& element);
std::size_t TextControlValueLimitBytes(const dom::Element& element);

std::optional<FormSubmission> BuildFormSubmission(const dom::Element& form,
                                                  const dom::Element* submitter,
                                                  const dom::Document& document,
                                                  std::string_view document_url);

}  // namespace microbrowser::engine

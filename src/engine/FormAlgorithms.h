#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine/Page.h"
#include "html/Encoding.h"

namespace microbrowser::dom {
class Document;
class Element;
}  // namespace microbrowser::dom

namespace microbrowser::engine {

std::string ControlValue(const dom::Element& element);
std::size_t TextControlValueLimitBytes(const dom::Element& element);

// `document_encoding` is what the form's data set is encoded in when the form does not name one
// itself: HTML's "select an encoding" step, which reads `accept-charset` first and falls back to the
// document's. It is a parameter rather than something read off the document because the document
// does not carry it -- the sniff happens once, where the bytes became text, and `engine::Page` is
// what remembers the answer.
//
// This matters more than it looks. A form on a Shift_JIS page submits Shift_JIS bytes, and a
// character the encoding cannot hold is sent as the literal text `&#1234;` -- so a field a user
// typed can arrive at the server as something a naive handler will echo back as markup. Getting the
// encoding wrong does not produce mojibake here; it produces *different bytes* than every other
// browser sends, which is a compatibility bug on the way in and an injection question on the way
// out.
std::optional<FormSubmission> BuildFormSubmission(const dom::Element& form,
                                                  const dom::Element* submitter,
                                                  const dom::Document& document,
                                                  std::string_view document_url,
                                                  html::Encoding document_encoding);

}  // namespace microbrowser::engine

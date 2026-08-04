#include "engine/FormAlgorithms.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "dom/Node.h"
#include "html/FormControl.h"
#include "util/Parse.h"
#include "util/StringUtil.h"
#include "util/UrlEncoded.h"

namespace microbrowser::engine {

namespace {

constexpr std::size_t kMaxInputValueBytes = 4096;
constexpr std::string_view kUrlEncodedFormContentType = "application/x-www-form-urlencoded";
constexpr std::string_view kTextPlainFormContentType = "text/plain";

enum class FormEncoding : std::uint8_t {
  UrlEncoded,
  TextPlain,
  Unsupported,
};

bool IsSuccessfulControl(const dom::Element& element, const dom::Element* submitter) {
  if (html::IsDisabledFormControl(element)) {
    return false;
  }
  const std::string* name = element.GetAttribute("name");
  if (name == nullptr || name->empty()) {
    return false;
  }
  if (element.TagName() == "button") {
    return html::IsSubmitControl(element) && &element == submitter;
  }
  if (element.TagName() == "input") {
    if (html::IsSubmitInput(element)) {
      return &element == submitter;
    }
    if (html::IsInputType(element, "button") || html::IsInputType(element, "reset") ||
        html::IsInputType(element, "file")) {
      return false;
    }
    if ((html::IsCheckboxInput(element) || html::IsRadioInput(element)) &&
        !element.HasAttribute("checked")) {
      return false;
    }
    return true;
  }
  if (html::IsTextareaElement(element)) {
    return true;
  }
  if (html::IsSelectElement(element)) {
    return !html::SelectedOptionValues(element).empty();
  }
  return false;
}

template <typename Callback>
void ForEachSuccessfulFormValue(const dom::Document& document,
                                const dom::Element& form,
                                const dom::Element* submitter,
                                Callback callback) {
  document.ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (!html::BelongsToForm(element, form, document)) {
      return;
    }
    if (!IsSuccessfulControl(element, submitter)) {
      return;
    }
    const std::string* name = element.GetAttribute("name");
    if (name == nullptr) {
      return;
    }
    if (html::IsSelectElement(element)) {
      for (const std::string& value : html::SelectedOptionValues(element)) {
        callback(*name, value);
      }
      return;
    }
    const std::string value = ControlValue(element);
    callback(*name, value);
  });
}

std::string UrlEncodedFormData(const dom::Document& document,
                               const dom::Element& form,
                               const dom::Element* submitter) {
  std::string out;
  ForEachSuccessfulFormValue(document, form, submitter,
                             [&](std::string_view name, std::string_view value) {
                               // The urlencoded serializer, shared with
                               // URLSearchParams. This used to be
                               // `PercentEncodeSet::Component`, which keeps
                               // `!'()~` -- close enough to look right, and
                               // wrong enough that a field with an apostrophe
                               // in it reached the server differently from
                               // every other browser.
                               util::AppendUrlEncodedPair(name, value, out);
                             });
  return out;
}

std::string TextPlainFormData(const dom::Document& document,
                              const dom::Element& form,
                              const dom::Element* submitter) {
  std::string out;
  ForEachSuccessfulFormValue(document, form, submitter,
                             [&](std::string_view name, std::string_view value) {
                               out += name;
                               out.push_back('=');
                               out += value;
                               out += "\r\n";
                             });
  return out;
}

std::string WithoutQueryOrFragment(std::string_view url) {
  const std::size_t cut = url.find_first_of("?#");
  return cut == std::string_view::npos ? std::string(url) : std::string(url.substr(0, cut));
}

std::string FormMethod(const dom::Element& form, const dom::Element* submitter) {
  const std::string* method =
      submitter != nullptr && submitter->HasAttribute("formmethod")
          ? submitter->GetAttribute("formmethod")
          : form.GetAttribute("method");
  if (method != nullptr && util::EqualsAsciiCaseInsensitive(*method, "post")) {
    return "POST";
  }
  return "GET";
}

FormEncoding FormEncodingFor(const dom::Element& form, const dom::Element* submitter) {
  const std::string* encoding =
      submitter != nullptr && submitter->HasAttribute("formenctype")
          ? submitter->GetAttribute("formenctype")
          : form.GetAttribute("enctype");
  if (encoding == nullptr || encoding->empty()) {
    return FormEncoding::UrlEncoded;
  }
  if (util::EqualsAsciiCaseInsensitive(*encoding, kTextPlainFormContentType)) {
    return FormEncoding::TextPlain;
  }
  if (util::EqualsAsciiCaseInsensitive(*encoding, "multipart/form-data")) {
    return FormEncoding::Unsupported;
  }
  return FormEncoding::UrlEncoded;
}

std::string WithoutFragment(std::string_view url) {
  const std::size_t cut = url.find('#');
  return cut == std::string_view::npos ? std::string(url) : std::string(url.substr(0, cut));
}

}  // namespace

std::string ControlValue(const dom::Element& element) {
  if (const std::string* value = element.GetAttribute("value")) {
    return *value;
  }
  if (html::IsTextareaElement(element)) {
    return element.TextContent();
  }
  if (const std::optional<std::string> selected = html::SelectedOptionValue(element)) {
    return *selected;
  }
  if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
    return "on";
  }
  if (html::IsSubmitInput(element)) {
    return "Submit";
  }
  return {};
}

std::size_t TextControlValueLimitBytes(const dom::Element& element) {
  const std::string* maxlength = element.GetAttribute("maxlength");
  if (maxlength == nullptr) {
    return kMaxInputValueBytes;
  }
  const std::optional<int> parsed = util::ParseInt(*maxlength);
  if (!parsed.has_value() || *parsed < 0) {
    return kMaxInputValueBytes;
  }
  return std::min(static_cast<std::size_t>(*parsed), kMaxInputValueBytes);
}

std::optional<FormSubmission> BuildFormSubmission(const dom::Element& form,
                                                  const dom::Element* submitter,
                                                  const dom::Document& document,
                                                  std::string_view document_url) {
  const std::string* action =
      submitter != nullptr && submitter->HasAttribute("formaction")
          ? submitter->GetAttribute("formaction")
          : form.GetAttribute("action");
  const std::string action_url =
      action == nullptr || action->empty() ? std::string(document_url) : *action;
  const FormEncoding encoding = FormEncodingFor(form, submitter);
  FormSubmission submission;
  submission.method = FormMethod(form, submitter);
  if (submission.method == "POST") {
    if (encoding == FormEncoding::Unsupported) {
      return std::nullopt;
    }
    submission.url = WithoutFragment(action_url);
    if (encoding == FormEncoding::TextPlain) {
      submission.body = TextPlainFormData(document, form, submitter);
      submission.content_type = std::string(kTextPlainFormContentType);
    } else {
      submission.body = UrlEncodedFormData(document, form, submitter);
      submission.content_type = std::string(kUrlEncodedFormContentType);
    }
  } else {
    const std::string query = UrlEncodedFormData(document, form, submitter);
    submission.url = WithoutQueryOrFragment(action_url);
    if (!query.empty()) {
      submission.url += '?';
      submission.url += query;
    }
  }
  return submission;
}

}  // namespace microbrowser::engine

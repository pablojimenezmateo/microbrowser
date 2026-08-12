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
                               const dom::Element* submitter,
                               html::Encoding encoding) {
  std::string out;
  ForEachSuccessfulFormValue(
      document, form, submitter, [&](std::string_view name, std::string_view value) {
        // Two steps, in this order, and the order is the specification's: the text becomes *bytes*
        // in the form's encoding first -- with anything that encoding cannot hold spelled `&#1234;`
        // -- and the urlencoded serializer then percent-encodes those bytes. Doing it the other way
        // round would percent-encode UTF-8 and hand the server the wrong bytes under the right
        // syntax, which is the failure no test of the syntax can see.
        //
        // The `&`, `#` and `;` of an escape are all outside the serializer's keep-set, so they come
        // out as `%26%23...%3B` without this having to spell them -- which is why this step can be a
        // plain string and the URL query's cannot.
        //
        // The serializer itself is not a percent-encode set from the URL standard: it keeps ASCII
        // alphanumerics and `*-._` and nothing else. This used to be `PercentEncodeSet::Component`,
        // which keeps `!'()~` -- close enough to look right, and wrong enough that a field with an
        // apostrophe in it reached the server differently from every other browser.
        util::AppendUrlEncodedPair(html::EncodeWithNumericEscapes(name, encoding),
                                   html::EncodeWithNumericEscapes(value, encoding), out);
      });
  return out;
}

std::string TextPlainFormData(const dom::Document& document,
                              const dom::Element& form,
                              const dom::Element* submitter,
                              html::Encoding encoding) {
  std::string out;
  ForEachSuccessfulFormValue(document, form, submitter,
                             [&](std::string_view name, std::string_view value) {
                               out += html::EncodeWithNumericEscapes(name, encoding);
                               out.push_back('=');
                               out += html::EncodeWithNumericEscapes(value, encoding);
                               out += "\r\n";
                             });
  return out;
}

// HTML's "select an encoding" for a form: the first label in `accept-charset` this browser has an
// encoding for, and the document's own otherwise.
//
// UTF-16 is replaced by UTF-8 -- the standard's rule, and there is a reason beyond tidiness: there
// is no UTF-16 encoder in the Encoding Standard at all, and a form body full of NUL bytes is one
// nothing between here and the server survives.
html::Encoding FormEncodingCharset(const dom::Element& form, html::Encoding document_encoding) {
  html::Encoding chosen = document_encoding;
  if (const std::string* accept = form.GetAttribute("accept-charset")) {
    // A space-separated list, and the first *supported* label wins rather than the first label --
    // a page that writes `accept-charset="x-made-up utf-8"` means UTF-8.
    chosen = html::Encoding::Utf8;
    std::size_t at = 0;
    while (at < accept->size()) {
      const std::size_t end = accept->find_first_of(" \t\n\f\r", at);
      const std::string_view label =
          std::string_view(*accept).substr(at, end == std::string::npos ? end : end - at);
      if (!label.empty()) {
        if (const std::optional<html::Encoding> found = html::EncodingFromLabel(label)) {
          chosen = *found;
          break;
        }
      }
      if (end == std::string::npos) {
        break;
      }
      at = end + 1;
    }
  }
  return chosen == html::Encoding::Utf16Le || chosen == html::Encoding::Utf16Be
             ? html::Encoding::Utf8
             : chosen;
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
                                                  std::string_view document_url,
                                                  html::Encoding document_encoding) {
  const std::string* action =
      submitter != nullptr && submitter->HasAttribute("formaction")
          ? submitter->GetAttribute("formaction")
          : form.GetAttribute("action");
  const std::string action_url =
      action == nullptr || action->empty() ? std::string(document_url) : *action;
  const FormEncoding encoding = FormEncodingFor(form, submitter);
  const html::Encoding charset = FormEncodingCharset(form, document_encoding);
  FormSubmission submission;
  submission.method = FormMethod(form, submitter);
  if (submission.method == "POST") {
    if (encoding == FormEncoding::Unsupported) {
      return std::nullopt;
    }
    submission.url = WithoutFragment(action_url);
    if (encoding == FormEncoding::TextPlain) {
      submission.body = TextPlainFormData(document, form, submitter, charset);
      submission.content_type = std::string(kTextPlainFormContentType);
    } else {
      submission.body = UrlEncodedFormData(document, form, submitter, charset);
      submission.content_type = std::string(kUrlEncodedFormContentType);
    }
    // **No `charset` parameter, deliberately.** HTML gives these two MIME types literally, and no
    // browser adds one: a server reads a form body in the encoding the *page* declared, which it
    // served. Adding one here would be this browser telling servers something none of them expect.
  } else {
    const std::string query = UrlEncodedFormData(document, form, submitter, charset);
    submission.url = WithoutQueryOrFragment(action_url);
    if (!query.empty()) {
      submission.url += '?';
      submission.url += query;
    }
  }
  return submission;
}

}  // namespace microbrowser::engine

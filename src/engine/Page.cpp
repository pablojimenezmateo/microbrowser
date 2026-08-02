#include "engine/Page.h"

#include <algorithm>
#include <utility>

#include "css/StyleSheet.h"
#include "gfx/PngDecoder.h"
#include "html/FormControl.h"
#include "html/TreeBuilder.h"
#include "url/PercentEncoding.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

constexpr std::size_t kMaxInputValueBytes = 4096;

// Splits an attribute on ASCII whitespace, per the HTML spec's
// "space-separated tokens".
std::vector<std::string_view> SplitTokens(std::string_view value) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' ||
                                value[i] == '\r' || value[i] == '\f')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < value.size() && value[i] != ' ' && value[i] != '\t' && value[i] != '\n' &&
           value[i] != '\r' && value[i] != '\f') {
      ++i;
    }
    if (i > start) {
      tokens.push_back(value.substr(start, i - start));
    }
  }
  return tokens;
}

// The text of an element's direct text children, concatenated. Enough for
// <title>, which is a text-only element by definition.
std::string DirectText(const dom::Element& element) {
  std::string text;
  for (const std::unique_ptr<dom::Node>& child : element.Children()) {
    if (child->IsText()) {
      text += static_cast<const dom::Text&>(*child).Data();
    }
  }
  return text;
}

bool Contains(const gfx::FloatRect& rect, gfx::FloatPoint point) {
  return point.x >= rect.x && point.x < rect.Right() && point.y >= rect.y &&
         point.y < rect.Bottom();
}

const std::string* AnchorHref(const dom::Element* element) {
  if (element == nullptr || element->TagName() != "a") {
    return nullptr;
  }
  const std::string* href = element->GetAttribute("href");
  return href != nullptr && !href->empty() ? href : nullptr;
}

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

std::string InputValue(const dom::Element& element) {
  if (const std::string* value = element.GetAttribute("value")) {
    return *value;
  }
  if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
    return "on";
  }
  if (html::IsSubmitInput(element)) {
    return "Submit";
  }
  return {};
}

std::size_t InputValueLimitBytes(const dom::Element& element) {
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

bool IsSuccessfulInput(const dom::Element& element, const dom::Element* submitter) {
  if (element.TagName() != "input" || element.HasAttribute("disabled")) {
    return false;
  }
  const std::string* name = element.GetAttribute("name");
  if (name == nullptr || name->empty()) {
    return false;
  }
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

bool IsRadioGroupPeer(const dom::Element& candidate, const dom::Element& activated) {
  if (&candidate == &activated || !html::IsRadioInput(candidate)) {
    return false;
  }
  const std::string* candidate_name = candidate.GetAttribute("name");
  const std::string* activated_name = activated.GetAttribute("name");
  if (candidate_name == nullptr || activated_name == nullptr || candidate_name->empty() ||
      *candidate_name != *activated_name) {
    return false;
  }
  return candidate.ClosestAncestor("form") == activated.ClosestAncestor("form");
}

void AppendFormComponent(std::string_view value, std::string& out) {
  for (const char c : value) {
    if (c == ' ') {
      out.push_back('+');
    } else {
      const std::string_view piece(&c, 1);
      url::PercentEncodeInto(piece, url::PercentEncodeSet::Component, out);
    }
  }
}

std::string FormQuery(const dom::Element& form, const dom::Element* submitter) {
  std::string out;
  form.ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (!IsSuccessfulInput(element, submitter)) {
      return;
    }
    if (!out.empty()) {
      out.push_back('&');
    }
    AppendFormComponent(*element.GetAttribute("name"), out);
    out.push_back('=');
    const std::string value = InputValue(element);
    AppendFormComponent(value, out);
  });
  return out;
}

std::string WithoutQueryOrFragment(std::string_view url) {
  const std::size_t cut = url.find_first_of("?#");
  return cut == std::string_view::npos ? std::string(url) : std::string(url.substr(0, cut));
}

std::optional<std::string> FormGetTarget(const dom::Element& form,
                                         const dom::Element* submitter,
                                         std::string_view document_url) {
  if (const std::string* method = form.GetAttribute("method")) {
    if (!util::EqualsAsciiCaseInsensitive(*method, "get")) {
      return std::nullopt;
    }
  }
  const std::string* action = form.GetAttribute("action");
  std::string target = action == nullptr || action->empty() ? std::string(document_url) : *action;
  target = WithoutQueryOrFragment(target);
  const std::string query = FormQuery(form, submitter);
  if (!query.empty()) {
    target += '?';
    target += query;
  }
  return target;
}

std::optional<const dom::Element*> HitTestSubmit(const layout::Box& box, gfx::FloatPoint point) {
  for (std::size_t i = box.Children().size(); i-- > 0;) {
    if (std::optional<const dom::Element*> hit = HitTestSubmit(*box.Children()[i], point)) {
      return hit;
    }
  }
  const dom::Element* element = box.Origin();
  if (element == nullptr || !html::IsSubmitInput(*element)) {
    return std::nullopt;
  }
  return Contains(box.Geometry().BorderBox(), point) ? std::optional<const dom::Element*>(element)
                                                     : std::nullopt;
}

std::optional<dom::Element*> HitTestCheckableInput(const layout::Box& box,
                                                   gfx::FloatPoint point) {
  for (std::size_t i = box.Children().size(); i-- > 0;) {
    if (std::optional<dom::Element*> hit = HitTestCheckableInput(*box.Children()[i], point)) {
      return hit;
    }
  }
  const dom::Element* element = box.Origin();
  if (element == nullptr || !html::IsCheckableInput(*element)) {
    return std::nullopt;
  }
  if (!Contains(box.Geometry().BorderBox(), point)) {
    return std::nullopt;
  }
  return const_cast<dom::Element*>(element);
}

std::optional<dom::Element*> HitTestEditableInput(const layout::Box& box, gfx::FloatPoint point) {
  for (std::size_t i = box.Children().size(); i-- > 0;) {
    if (std::optional<dom::Element*> hit = HitTestEditableInput(*box.Children()[i], point)) {
      return hit;
    }
  }
  const dom::Element* element = box.Origin();
  if (element == nullptr || !html::IsEditableTextInput(*element)) {
    return std::nullopt;
  }
  if (!Contains(box.Geometry().BorderBox(), point)) {
    return std::nullopt;
  }
  return const_cast<dom::Element*>(element);
}

std::optional<std::string> HitTestLink(const layout::Box& box, gfx::FloatPoint point,
                                       const std::string* active_href) {
  if (const std::string* href = AnchorHref(box.Origin())) {
    active_href = href;
  }

  for (std::size_t i = box.Children().size(); i-- > 0;) {
    if (std::optional<std::string> hit = HitTestLink(*box.Children()[i], point, active_href)) {
      return hit;
    }
  }

  if (active_href == nullptr) {
    return std::nullopt;
  }
  if (box.GetKind() == layout::Box::Kind::Text) {
    for (const layout::TextFragment& fragment : box.Fragments()) {
      if (Contains(fragment.rect, point)) {
        return *active_href;
      }
    }
    return std::nullopt;
  }
  if (Contains(box.Geometry().BorderBox(), point)) {
    return *active_href;
  }
  return std::nullopt;
}

}  // namespace

Page::Page(gfx::FontProvider& fonts) : text_(fonts), measurer_(text_) {}

void Page::Load(std::string_view html, std::string url) {
  util::PerformanceTrace::Scope scope("engine::Page::Load");

  url_ = std::move(url);
  // A fresh resolver per document. Author sheets belong to the document that
  // carried them, and keeping the old one would let the previous page's CSS
  // style this one.
  resolver_ = css::StyleResolver{};
  document_ = html::ParseDocument(html);
  boxes_.reset();
  focused_input_ = nullptr;
  content_height_ = 0.0f;
  images_.clear();

  CollectStyleSheets();
  CollectImages();
  ExtractTitle();
}

void Page::CollectStyleSheets() {
  pending_sheets_.clear();
  if (document_ == nullptr) {
    return;
  }
  for (const dom::Element* style : document_->ElementsByTagName("style")) {
    resolver_.AddStyleSheet(css::ParseStyleSheet(DirectText(*style)), css::Origin::Author);
  }
  for (const dom::Element* link : document_->ElementsByTagName("link")) {
    // `rel` is a space-separated set of tokens, and a sheet is only a sheet
    // when "stylesheet" is one of them: `rel="alternate stylesheet"` is not
    // applied, and `rel="preload"` is not a stylesheet at all.
    const std::string* rel = link->GetAttribute("rel");
    const std::string* href = link->GetAttribute("href");
    if (rel == nullptr || href == nullptr || href->empty()) {
      continue;
    }
    bool is_stylesheet = false;
    bool is_alternate = false;
    for (const std::string_view token : SplitTokens(*rel)) {
      is_stylesheet = is_stylesheet || util::EqualsAsciiCaseInsensitive(token, "stylesheet");
      is_alternate = is_alternate || util::EqualsAsciiCaseInsensitive(token, "alternate");
    }
    if (is_stylesheet && !is_alternate) {
      pending_sheets_.push_back(*href);
    }
  }
}

void Page::CollectImages() {
  pending_images_.clear();
  if (document_ == nullptr) {
    return;
  }
  for (const dom::Element* image : document_->ElementsByTagName("img")) {
    const std::string* src = image->GetAttribute("src");
    if (src != nullptr && !src->empty() &&
        std::find(pending_images_.begin(), pending_images_.end(), *src) ==
            pending_images_.end()) {
      // Deduplicated: a page that shows one icon forty times fetches and
      // decodes it once.
      pending_images_.push_back(*src);
    }
  }
}

void Page::AddImage(std::string src, std::shared_ptr<const gfx::Image> image) {
  if (image == nullptr || !image->IsValid()) {
    return;
  }
  images_[std::move(src)] = std::move(image);
  // The box tree sized its replaced boxes against what was available then.
  boxes_.reset();
}

std::shared_ptr<const gfx::Image> Page::ImageFor(std::string_view src) const {
  const auto found = images_.find(src);
  return found == images_.end() ? nullptr : found->second;
}

void Page::AddStyleSheet(std::string_view css) {
  resolver_.AddStyleSheet(css::ParseStyleSheet(css), css::Origin::Author);
  // The box tree was built against the old cascade, if it was built at all.
  boxes_.reset();
}

void Page::ExtractTitle() {
  title_.clear();
  if (document_ != nullptr) {
    if (const dom::Element* element = document_->FirstElementByTagName("title")) {
      title_ = DirectText(*element);
    }
  }
  if (title_.empty()) {
    // Never empty: a tab strip has to show something, and "" is not a title,
    // it is a missing one.
    title_ = url_.empty() ? std::string("New Tab") : url_;
  }
}

float Page::Layout(float width) {
  util::PerformanceTrace::Scope scope("engine::Page::Layout");
  if (document_ == nullptr) {
    content_height_ = 0.0f;
    return 0.0f;
  }
  const layout::LayoutEngine engine(resolver_, measurer_, this);
  // The box tree is rebuilt per layout for now. It depends only on the document
  // and the cascade, neither of which changes here, so this is the obvious
  // thing to cache -- and the split between BuildBoxTree and Layout is what
  // makes caching it a change to this function alone.
  boxes_ = engine.BuildBoxTree(*document_);
  content_height_ = engine.Layout(*boxes_, width);
  return content_height_;
}

void Page::Paint(gfx::DisplayList& out, float scroll_y) const {
  util::PerformanceTrace::Scope scope("engine::Page::Paint");
  if (boxes_ == nullptr) {
    return;
  }
  layout::BuildDisplayList(*boxes_, out, gfx::FloatPoint{0.0f, -scroll_y});
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);
}

std::optional<std::string> Page::LinkAt(gfx::FloatPoint document_point) const {
  if (boxes_ == nullptr) {
    return std::nullopt;
  }
  return HitTestLink(*boxes_, document_point, nullptr);
}

std::optional<std::string> Page::FormSubmissionAt(gfx::FloatPoint document_point) const {
  if (boxes_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<const dom::Element*> submitter = HitTestSubmit(*boxes_, document_point);
  if (!submitter.has_value()) {
    return std::nullopt;
  }
  const dom::Element* form = (*submitter)->ClosestAncestor("form");
  if (form == nullptr) {
    return std::nullopt;
  }
  return FormGetTarget(*form, *submitter, url_);
}

bool Page::FocusInputAt(gfx::FloatPoint document_point) {
  focused_input_ = nullptr;
  if (boxes_ == nullptr) {
    return false;
  }
  const std::optional<dom::Element*> hit = HitTestEditableInput(*boxes_, document_point);
  if (!hit.has_value()) {
    return false;
  }
  focused_input_ = *hit;
  return true;
}

bool Page::ActivateCheckableInputAt(gfx::FloatPoint document_point) {
  focused_input_ = nullptr;
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  const std::optional<dom::Element*> hit = HitTestCheckableInput(*boxes_, document_point);
  if (!hit.has_value()) {
    return false;
  }
  dom::Element& input = **hit;
  if (html::IsCheckboxInput(input)) {
    if (input.HasAttribute("checked")) {
      input.RemoveAttribute("checked");
    } else {
      input.SetAttribute("checked", "");
    }
    boxes_.reset();
    return true;
  }
  if (input.HasAttribute("checked")) {
    return false;
  }
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& candidate = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (IsRadioGroupPeer(candidate, input)) {
      candidate.RemoveAttribute("checked");
    }
  });
  input.SetAttribute("checked", "");
  boxes_.reset();
  return true;
}

bool Page::InsertTextIntoFocusedInput(std::string_view text) {
  if (focused_input_ == nullptr || text.empty()) {
    return false;
  }
  std::string value;
  if (const std::string* current = focused_input_->GetAttribute("value")) {
    value = *current;
  }
  const std::size_t limit = InputValueLimitBytes(*focused_input_);
  if (value.size() >= limit) {
    return false;
  }
  const std::size_t room = limit - value.size();
  value.append(text.substr(0, room));
  focused_input_->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

bool Page::DeleteBackwardFromFocusedInput() {
  if (focused_input_ == nullptr) {
    return false;
  }
  std::string value;
  if (const std::string* current = focused_input_->GetAttribute("value")) {
    value = *current;
  }
  if (value.empty()) {
    return false;
  }
  value.erase(PreviousUtf8Boundary(value));
  focused_input_->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

std::optional<std::string> Page::SubmitFocusedForm() const {
  if (focused_input_ == nullptr) {
    return std::nullopt;
  }
  const dom::Element* form = focused_input_->ClosestAncestor("form");
  if (form == nullptr) {
    return std::nullopt;
  }
  return FormGetTarget(*form, nullptr, url_);
}

}  // namespace microbrowser::engine

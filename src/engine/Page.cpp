#include "engine/Page.h"

#include <algorithm>
#include <utility>

#include "css/StyleSheet.h"
#include "gfx/SvgDecoder.h"
#include "engine/FormAlgorithms.h"
#include "html/FormControl.h"
#include "html/TreeBuilder.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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

bool IsLinkedStyleSheet(const dom::Element& link) {
  if (link.TagName() != "link") {
    return false;
  }
  // `rel` is a space-separated set of tokens, and a sheet is only a sheet
  // when "stylesheet" is one of them: `rel="alternate stylesheet"` is not
  // applied, and `rel="preload"` is not a stylesheet at all.
  const std::string* rel = link.GetAttribute("rel");
  if (rel == nullptr) {
    return false;
  }
  bool is_stylesheet = false;
  bool is_alternate = false;
  for (const std::string_view token : SplitTokens(*rel)) {
    is_stylesheet = is_stylesheet || util::EqualsAsciiCaseInsensitive(token, "stylesheet");
    is_alternate = is_alternate || util::EqualsAsciiCaseInsensitive(token, "alternate");
  }
  return is_stylesheet && !is_alternate;
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

bool IsValueResettableControl(const dom::Element& element) {
  return html::IsTextControl(element);
}

bool IsRadioGroupPeer(const dom::Element& candidate,
                      const dom::Element& activated,
                      const dom::Document& document) {
  if (&candidate == &activated || !html::IsRadioInput(candidate)) {
    return false;
  }
  const std::string* candidate_name = candidate.GetAttribute("name");
  const std::string* activated_name = activated.GetAttribute("name");
  if (candidate_name == nullptr || activated_name == nullptr || candidate_name->empty() ||
      *candidate_name != *activated_name) {
    return false;
  }
  return html::FormOwner(candidate, document) == html::FormOwner(activated, document);
}

using ElementPredicate = bool (*)(const dom::Element&);

// The one hit-test walk over form controls: submit, reset, checkbox, radio and
// text field differ only in the predicate.
//
// Last child first, because a later sibling paints over an earlier one and the
// topmost box under the point is the one that was clicked. A disabled control
// is never a target, which is a property of every control rather than of any
// one predicate -- so it is checked here, once, instead of being re-derived in
// each caller.
//
// The box tree is const because hit testing does not change layout, and
// `Origin()` hands out a const element to preserve that. The element itself is
// not const: activating a control mutates it -- a checkbox flips `checked`, a
// text field takes focus -- and the document it belongs to is mutable. That is
// what the cast crosses, and why it lives here rather than at four call sites.
dom::Element* HitTestFormControl(const layout::Box& box, gfx::FloatPoint point,
                                 ElementPredicate predicate) {
  for (std::size_t i = box.Children().size(); i-- > 0;) {
    if (dom::Element* hit = HitTestFormControl(*box.Children()[i], point, predicate)) {
      return hit;
    }
  }
  const dom::Element* element = box.Origin();
  if (element == nullptr || html::IsDisabledFormControl(*element) || !predicate(*element)) {
    return nullptr;
  }
  if (!Contains(box.Geometry().BorderBox(), point)) {
    return nullptr;
  }
  return const_cast<dom::Element*>(element);
}

// The innermost element whose box contains `point`, or null.
//
// Deepest-first, and the last child first within a level: a box painted over
// another is the one a click lands on, and the paint order is child-after-
// parent and later-sibling-after-earlier.
const dom::Element* HitTestElement(const layout::Box& box, gfx::FloatPoint point) {
  for (std::size_t i = box.Children().size(); i-- > 0;) {
    if (const dom::Element* hit = HitTestElement(*box.Children()[i], point)) {
      return hit;
    }
  }
  if (box.GetKind() == layout::Box::Kind::Text) {
    for (const layout::TextFragment& fragment : box.Fragments()) {
      if (Contains(fragment.rect, point)) {
        // A text box has no element of its own; the click belongs to whatever
        // generated it, which the caller finds by walking up from here.
        return box.Origin();
      }
    }
    return nullptr;
  }
  if (box.Origin() != nullptr && Contains(box.Geometry().BorderBox(), point)) {
    return box.Origin();
  }
  return nullptr;
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

const std::vector<std::string>& Page::ConsoleOutput() const { return script_.ConsoleOutput(); }

void Page::AddScript(std::size_t pending_index, std::string source) {
  script_.AddFetched(pending_index, std::move(source));
}

void Page::RunScripts() {
  if (document_ == nullptr) {
    return;
  }
  script_.Run(*document_, url_);
  // A script can change the tree, so anything derived from it is stale. The
  // box tree is dropped rather than patched: incremental layout is a later
  // decision and a wrong one made early here would be invisible.
  boxes_.reset();
  CollectImages();
}

void Page::Load(std::string_view html, std::string url) {
  util::PerformanceTrace::Scope scope("engine::Page::Load");

  url_ = std::move(url);
  // A fresh resolver per document. Author sheets belong to the document that
  // carried them, and keeping the old one would let the previous page's CSS
  // style this one.
  resolver_ = css::StyleResolver{};
  document_ = html::ParseDocument(html);
  boxes_.reset();
  focused_text_control_ = nullptr;
  content_height_ = 0.0f;
  resources_ = DocumentResources{};
  control_defaults_.clear();

  CollectStyleSheets();
  CollectImages();
  if (document_ != nullptr) {
    // Found now, run later: an external script has to arrive before anything
    // after it in the document may run, and what a URL turns into is the
    // loader's problem rather than this one's.
    script_.Collect(*document_);
  }
  if (document_ != nullptr) {
    document_->ForEachDescendant([&](const dom::Node& node) {
      if (!node.IsElement()) {
        return;
      }
      const auto& element = static_cast<const dom::Element&>(node);
      if (element.TagName() == "input" || element.TagName() == "textarea") {
        control_defaults_.emplace(&element,
                                  std::pair<std::string, bool>{ControlValue(element),
                                                               element.HasAttribute("checked")});
      }
    });
  }
  ExtractTitle();
}

void Page::CollectStyleSheets() {
  resources_.pending_sheets.clear();
  resources_.pending_sheet_slots.clear();
  resources_.author_sheet_slots.clear();
  if (document_ == nullptr) {
    return;
  }
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() == "style") {
      resources_.author_sheet_slots.emplace_back(DirectText(element));
      return;
    }
    if (!IsLinkedStyleSheet(element)) {
      return;
    }
    const std::string* href = element.GetAttribute("href");
    if (href == nullptr || href->empty()) {
      return;
    }
    resources_.pending_sheets.push_back(*href);
    resources_.pending_sheet_slots.push_back(resources_.author_sheet_slots.size());
    resources_.author_sheet_slots.push_back(std::nullopt);
  });
  RebuildAuthorStyleSheets();
}

void Page::RebuildAuthorStyleSheets() {
  resolver_ = css::StyleResolver{};
  for (const std::optional<std::string>& css : resources_.author_sheet_slots) {
    if (css.has_value()) {
      resolver_.AddStyleSheet(css::ParseStyleSheet(*css), css::Origin::Author);
    }
  }
  // A background image is named by the cascade, so the set of images a document
  // wants is not known until its stylesheets have arrived. Re-collected here
  // rather than only at load, or a page whose icons come from an external sheet
  // -- which is every page that has any -- would never fetch one.
  CollectImages();
  boxes_.reset();
}

void Page::CollectImages() {
  resources_.pending_images.clear();
  if (document_ == nullptr) {
    return;
  }
  // Deduplicated: a page that shows one icon forty times fetches and decodes
  // it once. That matters more for background images than for <img>, since a
  // sprite is by definition the same file behind every icon on the page.
  const auto want = [this](const std::string& src) {
    if (src.empty() || std::find(resources_.pending_images.begin(),
                                 resources_.pending_images.end(),
                                 src) != resources_.pending_images.end()) {
      return;
    }
    resources_.pending_images.push_back(src);
  };
  for (const dom::Element* image : document_->ElementsByTagName("img")) {
    if (const std::string* src = image->GetAttribute("src")) {
      want(*src);
    }
  }
  // Background images are named by the *cascade*, not by an attribute, so
  // finding them means resolving style -- which happens again at layout. The
  // duplicate resolve is the price of loading before the first layout, and the
  // alternative (laying out once with no backgrounds, then again) costs more
  // and shows the page twice.
  resolver_.ForEachStyledElement(*document_, [&want](const dom::Element&,
                                                     const css::ComputedStyle& style) {
    want(style.background.image);
  });
}

gfx::IntSize Page::RequestedImageSize(std::string_view src) const {
  gfx::IntSize size;
  if (document_ == nullptr) {
    return size;
  }
  // The largest request wins, and both axes are taken independently. One
  // resource may be drawn at several sizes, and a vector rasterized at the
  // smallest of them is blurry everywhere else; rasterizing at the largest
  // only ever scales down, which the painter resamples cleanly.
  for (const dom::Element* image : document_->ElementsByTagName("img")) {
    const std::string* source = image->GetAttribute("src");
    if (source == nullptr || *source != src) {
      continue;
    }
    for (const char* attribute : {"width", "height"}) {
      const std::string* text = image->GetAttribute(attribute);
      if (text == nullptr) {
        continue;
      }
      const std::optional<int> value = util::ParseInt(*text);
      if (!value.has_value() || *value <= 0 || *value > gfx::kMaxSvgEdge) {
        continue;
      }
      int& axis = attribute[0] == 'w' ? size.width : size.height;
      axis = std::max(axis, *value);
    }
  }
  return size;
}

void Page::AddImage(std::string src, std::shared_ptr<const gfx::Image> image) {
  if (image == nullptr || !image->IsValid()) {
    return;
  }
  resources_.images[std::move(src)] = std::move(image);
  // The box tree sized its replaced boxes against what was available then.
  boxes_.reset();
}

std::shared_ptr<const gfx::Image> Page::ImageFor(std::string_view src) const {
  const auto found = resources_.images.find(src);
  return found == resources_.images.end() ? nullptr : found->second;
}

void Page::AddStyleSheet(std::size_t pending_index, std::string_view css) {
  if (pending_index >= resources_.pending_sheet_slots.size()) {
    return;
  }
  const std::size_t slot = resources_.pending_sheet_slots[pending_index];
  if (slot >= resources_.author_sheet_slots.size()) {
    return;
  }
  resources_.author_sheet_slots[slot] = std::string(css);
  RebuildAuthorStyleSheets();
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

std::optional<FormSubmission> Page::FormSubmissionRequestAt(gfx::FloatPoint document_point) const {
  if (boxes_ == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  const dom::Element* submitter =
      HitTestFormControl(*boxes_, document_point, html::IsSubmitControl);
  if (submitter == nullptr) {
    return std::nullopt;
  }
  const dom::Element* form = html::FormOwner(*submitter, *document_);
  if (form == nullptr) {
    return std::nullopt;
  }
  return BuildFormSubmission(*form, submitter, *document_, url_);
}

bool Page::DispatchClickAt(gfx::FloatPoint document_point) {
  if (boxes_ == nullptr) {
    return false;
  }
  const dom::Element* target = HitTestElement(*boxes_, document_point);
  if (target == nullptr) {
    return false;
  }
  const bool prevented = script_.DispatchClick(*const_cast<dom::Element*>(target));
  if (prevented) {
    // A handler that changed the tree has invalidated everything derived from
    // it. Dropped rather than patched, for the reason RunScripts gives.
    boxes_.reset();
    CollectImages();
  }
  return prevented;
}

bool Page::FocusTextControlAt(gfx::FloatPoint document_point) {
  focused_text_control_ = nullptr;
  if (boxes_ == nullptr) {
    return false;
  }
  focused_text_control_ =
      HitTestFormControl(*boxes_, document_point, html::IsEditableTextControl);
  return focused_text_control_ != nullptr;
}

bool Page::ActivateCheckableInputAt(gfx::FloatPoint document_point) {
  focused_text_control_ = nullptr;
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  dom::Element* hit = HitTestFormControl(*boxes_, document_point, html::IsCheckableInput);
  if (hit == nullptr) {
    return false;
  }
  dom::Element& input = *hit;
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
    if (IsRadioGroupPeer(candidate, input, *document_)) {
      candidate.RemoveAttribute("checked");
    }
  });
  input.SetAttribute("checked", "");
  boxes_.reset();
  return true;
}

bool Page::ResetFormAt(gfx::FloatPoint document_point) {
  focused_text_control_ = nullptr;
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  const dom::Element* reset = HitTestFormControl(*boxes_, document_point, html::IsResetControl);
  if (reset == nullptr) {
    return false;
  }
  const dom::Element* form = html::FormOwner(*reset, *document_);
  if (form == nullptr) {
    return false;
  }
  bool changed = false;
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (!html::BelongsToForm(element, *form, *document_)) {
      return;
    }
    if (element.TagName() != "input" && element.TagName() != "textarea") {
      return;
    }
    const auto found = control_defaults_.find(&element);
    if (found == control_defaults_.end()) {
      return;
    }
    const auto& [value, checked] = found->second;
    if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
      if (checked) {
        changed = !element.HasAttribute("checked") || changed;
        element.SetAttribute("checked", "");
      } else {
        changed = element.RemoveAttribute("checked") || changed;
      }
    } else if (IsValueResettableControl(element)) {
      const std::string* current = element.GetAttribute("value");
      changed = current == nullptr || *current != value || changed;
      element.SetAttribute("value", value);
    }
  });
  if (!changed) {
    return false;
  }
  boxes_.reset();
  return true;
}

bool Page::InsertTextIntoFocusedTextControl(std::string_view text) {
  if (focused_text_control_ == nullptr || text.empty() ||
      !html::IsMutableTextControl(*focused_text_control_)) {
    return false;
  }
  std::string value = ControlValue(*focused_text_control_);
  const std::size_t limit = TextControlValueLimitBytes(*focused_text_control_);
  if (value.size() >= limit) {
    return false;
  }
  const std::size_t room = limit - value.size();
  value.append(text.substr(0, room));
  focused_text_control_->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

bool Page::DeleteBackwardFromFocusedTextControl() {
  if (focused_text_control_ == nullptr || !html::IsMutableTextControl(*focused_text_control_)) {
    return false;
  }
  std::string value = ControlValue(*focused_text_control_);
  if (value.empty()) {
    return false;
  }
  value.erase(PreviousUtf8Boundary(value));
  focused_text_control_->SetAttribute("value", std::move(value));
  boxes_.reset();
  return true;
}

std::optional<FormSubmission> Page::FocusedFormSubmission() const {
  if (focused_text_control_ == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  const dom::Element* form = html::FormOwner(*focused_text_control_, *document_);
  if (form == nullptr) {
    return std::nullopt;
  }
  return BuildFormSubmission(*form, nullptr, *document_, url_);
}

}  // namespace microbrowser::engine

#include "engine/Page.h"

#include <utility>

#include "css/StyleSheet.h"
#include "html/TreeBuilder.h"
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
  content_height_ = 0.0f;

  CollectStyleSheets();
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
  const layout::LayoutEngine engine(resolver_, measurer_);
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

}  // namespace microbrowser::engine

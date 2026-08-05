// The subresources a document names: its stylesheets, and its images.
//
// Split out of Page.cpp when that file reached its module cap, and the split is
// a real seam rather than an arbitrary cut. Everything here answers one
// question -- *what else does this document need fetching, and what does it do
// with the answer* -- and none of it turns a document into boxes, which is what
// the rest of Page.cpp does.
//
// The one thing worth knowing before changing anything here: an image is keyed
// by the URL **as the document wrote it**, not as it resolves. Resolving is the
// loader's job, and doing it here as well is how the two end up disagreeing and
// an <img> draws nothing.

#include "engine/Page.h"

#include <algorithm>
#include <utility>

#include "css/StyleSheet.h"
#include "gfx/SvgDecoder.h"
#include "engine/ImageSelection.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

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

// The text of an element's direct text children, concatenated.
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

}  // namespace

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
  resources_.selected_image_urls.clear();
  resources_.deferred_images.clear();
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
  // Which candidate an <img> wants is a question about the viewport as well as
  // about the element, so the answer is recorded here and read again at layout
  // rather than computed twice. A viewport that changes afterwards does not
  // re-select: the bytes for the new candidate were never fetched, so the only
  // thing re-selecting would achieve is an empty box where an image was.
  for (const dom::Element* image : document_->ElementsByTagName("img")) {
    std::string selected = SelectImageSource(*image, viewport_);
    if (selected.empty()) {
      continue;
    }
    // `loading="lazy"` holds the URL back rather than dropping it: the element
    // still knows which candidate it wants, so layout draws it the moment the
    // bytes arrive. Already-requested URLs are not re-deferred, or an image
    // fetched a screen ago would go back to being lazy every time a stylesheet
    // landed and re-collected.
    if (ImageLoadingIsLazy(*image) &&
        resources_.requested_images.find(selected) == resources_.requested_images.end()) {
      resources_.deferred_images[image] = selected;
      AddPerformanceCounter(PerfCounterId::EngineImagesDeferred);
    } else {
      want(selected);
    }
    resources_.selected_image_urls[image] = std::move(selected);
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

std::vector<std::string> Page::TakeUnrequestedImages() {
  std::vector<std::string> wanted;
  for (const std::string& src : resources_.pending_images) {
    if (resources_.requested_images.insert(src).second) {
      wanted.push_back(src);
    }
  }
  return wanted;
}

bool Page::RevealLazyImages() {
  if (resources_.deferred_images.empty() || boxes_ == nullptr) {
    return false;
  }
  // The scrollport, grown by one of itself in every direction. Not a number
  // chosen from a specification -- there is none -- but from what the feature
  // is for: far enough that the bytes are usually there by the time the image
  // is scrolled to, near enough that a feed of two hundred thumbnails fetches
  // a handful rather than all of them.
  const bindings::GeometryRect viewport = QueryViewport();
  const float margin_x = viewport.width;
  const float margin_y = viewport.height;
  bool revealed = false;
  for (auto it = resources_.deferred_images.begin(); it != resources_.deferred_images.end();) {
    const std::optional<bindings::BoxGeometry> box = QueryBox(*it->first);
    // No box is not "far away": a `display: none` image has no position to be
    // near anything, and it stays deferred until something gives it one.
    const bool near =
        box.has_value() && box->border_box.x - margin_x <= viewport.width &&
        box->border_box.x + box->border_box.width + margin_x >= 0.0f &&
        box->border_box.y - margin_y <= viewport.height &&
        box->border_box.y + box->border_box.height + margin_y >= 0.0f;
    if (!near) {
      ++it;
      continue;
    }
    resources_.pending_images.push_back(it->second);
    it = resources_.deferred_images.erase(it);
    revealed = true;
    AddPerformanceCounter(PerfCounterId::EngineImagesRevealed);
  }
  return revealed;
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
    // The selected candidate rather than the `src` attribute: an <img> whose
    // srcset chose a different URL is still the element that says how big the
    // thing at that URL should be drawn.
    const auto selected = resources_.selected_image_urls.find(image);
    if (selected == resources_.selected_image_urls.end() || selected->second != src) {
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

std::shared_ptr<const gfx::Image> Page::ImageForElement(const dom::Element& element) const {
  const auto selected = resources_.selected_image_urls.find(&element);
  if (selected == resources_.selected_image_urls.end()) {
    // An <img> a script created after the images were collected. Nothing was
    // fetched for it, so there is nothing to draw -- which is what the box
    // already looked like, rather than a new kind of failure.
    return nullptr;
  }
  return ImageFor(selected->second);
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

}  // namespace microbrowser::engine

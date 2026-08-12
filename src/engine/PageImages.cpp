// Document images: collect, fetch, attach, and deliver load events.
//
// Split from PageResources.cpp when CollectImages grew to walk shadow trees
// (youtube thumbnails) and AddImage learned to attach without a full box-tree
// rebuild. Stylesheets and fonts stay in PageResources; this file is every
// question about a decoded bitmap the page can draw.

#include "engine/Page.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/ImageSelection.h"
#include "gfx/SvgDecoder.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

void Page::CollectImages() {
  util::PerformanceTrace::Scope collect("engine::CollectImages");
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
    // `img-src`, at the one place every image URL in this document passes --
    // an `<img>`'s chosen candidate and a background the cascade named both
    // arrive here, and gating them separately is two chances to miss one.
    if (!policy_.AllowsUrl(csp::Directive::Img, src)) {
      return;
    }
    resources_.pending_images.push_back(src);
  };
  // Which candidate an <img> wants is a question about the viewport as well as
  // about the element, so the answer is recorded here and read again at layout
  // rather than computed twice. A viewport that changes afterwards does not
  // re-select: the bytes for the new candidate were never fetched, so the only
  // thing re-selecting would achieve is an empty box where an image was.
  //
  // Shadow trees too: youtube's thumbnails live inside `ytd-thumbnail`'s
  // shadow root, and `Document::ElementsByTagName` deliberately does not walk
  // those (a shadow root has no parent). Same shape as CollectShadowStyleSheets.
  const auto consider = [&](const dom::Element& image) {
    std::string selected = SelectImageSource(image, viewport_);
    if (selected.empty()) {
      return;
    }
    // `loading="lazy"` holds the URL back rather than dropping it: the element
    // still knows which candidate it wants, so layout draws it the moment the
    // bytes arrive. Already-requested URLs are not re-deferred, or an image
    // fetched a screen ago would go back to being lazy every time a stylesheet
    // landed and re-collected.
    if (ImageLoadingIsLazy(image) &&
        resources_.requested_images.find(selected) == resources_.requested_images.end()) {
      resources_.deferred_images[&image] = selected;
      AddPerformanceCounter(PerfCounterId::EngineImagesDeferred);
    } else {
      want(selected);
    }
    resources_.selected_image_urls[&image] = std::move(selected);
  };
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() == "img") {
      consider(element);
    }
    // Nested shadow roots: a host inside another shadow is unreachable from the
    // document walk, so collect from each shadow tree recursively.
    const auto walk_shadow = [&](const dom::DocumentFragment& root, auto& self) -> void {
      root.ForEachDescendant([&](const dom::Node& inner) {
        if (!inner.IsElement()) {
          return;
        }
        const auto& nested = static_cast<const dom::Element&>(inner);
        if (nested.TagName() == "img") {
          consider(nested);
        }
        if (const dom::DocumentFragment* deeper = nested.ShadowRoot()) {
          self(*deeper, self);
        }
      });
    };
    if (const dom::DocumentFragment* root = element.ShadowRoot()) {
      walk_shadow(*root, walk_shadow);
    }
  });
  // Background images are named by the *cascade*, not by an attribute, so
  // finding them means resolving style -- which happens in `EnsureBoxTree`
  // during the same pass that builds the box tree. TD-0005.
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
  const std::string key = src;
  resources_.images[std::move(src)] = std::move(image);
  const std::shared_ptr<const gfx::Image>& stored = resources_.images[key];

  // No laid-out tree: the next EnsureBoxTree picks the bitmap up through
  // ImageFor. Invalidating here only existed to force a rebuild of boxes that
  // had already measured at 0×0 — and there are none.
  if (boxes_ == nullptr) {
    util::AddPerformanceCounter(util::PerfCounterId::BoxTreeImagePaintOnly);
    DeliverImageLoad(key);
    return;
  }

  // Attach pixels to any laid-out box that already names this URL. When both
  // axes of a replaced box are definite without the bitmap (CSS length, HTML
  // attribute, or aspect-ratio + the other axis), layout size cannot change
  // and a full BuildBoxTree is wasted paint work — youtube search paid that
  // per thumbnail. Background images never size their box from the bitmap.
  //
  // If nothing in the current tree names this URL, fall back to invalidating:
  // the image may belong to a box built before CollectImages saw it, and
  // leaving the pixels only in the resource table would never draw them.
  bool layout_may_change = false;
  bool attached = false;
  const auto axis_definite = [](const layout::Box& box, bool horizontal) {
    const css::ComputedStyle& style = box.Style();
    const css::Length& declared = horizontal ? style.width : style.height;
    if (!declared.IsAuto() && !declared.IsPercent()) {
      return true;
    }
    if (box.Origin() != nullptr) {
      const std::string* attribute =
          box.Origin()->GetAttribute(horizontal ? "width" : "height");
      if (attribute != nullptr) {
        if (const std::optional<double> value = util::ParseDouble(*attribute)) {
          if (*value >= 0.0 && *value < 1e6) {
            return true;
          }
        }
      }
    }
    if (style.aspect_ratio > 0.0f) {
      const css::Length& other = horizontal ? style.height : style.width;
      if (!other.IsAuto() && !other.IsPercent()) {
        return true;
      }
      if (box.Origin() != nullptr) {
        const std::string* attribute =
            box.Origin()->GetAttribute(horizontal ? "height" : "width");
        if (attribute != nullptr) {
          if (const std::optional<double> value = util::ParseDouble(*attribute)) {
            if (*value >= 0.0 && *value < 1e6) {
              return true;
            }
          }
        }
      }
    }
    return false;
  };

  std::function<void(layout::Box&)> walk = [&](layout::Box& box) {
    if (box.GetKind() == layout::Box::Kind::Replaced && box.Origin() != nullptr) {
      const auto selected = resources_.selected_image_urls.find(box.Origin());
      if (selected != resources_.selected_image_urls.end() && selected->second == key) {
        const gfx::FloatRect& content = box.Geometry().content;
        const bool used_size_without_bitmap =
            box.Image() == nullptr && content.width > 0.0f && content.height > 0.0f;
        box.SetImage(stored);
        attached = true;
        // A non-zero used size established before any bitmap arrived came from
        // CSS, HTML attributes, aspect-ratio, or abspos fill — youtube's
        // thumbnails are the last of those. The decode cannot change geometry.
        if (!used_size_without_bitmap &&
            (!axis_definite(box, true) || !axis_definite(box, false))) {
          layout_may_change = true;
        }
      }
    }
    if (!box.Style().background.image.empty() && box.Style().background.image == key) {
      box.SetBackgroundImage(stored);
      attached = true;
    }
    for (std::unique_ptr<layout::Box>& child : box.MutableChildren()) {
      walk(*child);
    }
  };
  walk(*boxes_);

  if (!attached || layout_may_change) {
    InvalidateBoxTree();
    util::AddPerformanceCounter(util::PerfCounterId::BoxTreeInvalidatedByImage);
  } else {
    util::AddPerformanceCounter(util::PerfCounterId::BoxTreeImagePaintOnly);
  }
  DeliverImageLoad(key);
}

void Page::DeliverImageLoad(const std::string& src) {
  if (document_ == nullptr) {
    return;
  }
  // **Collected first, dispatched second, and the two must not be one loop.**
  //
  // `NotifyElementEvent` runs the page's own `load` handler, and a `load`
  // handler is allowed to change the tree -- `img.onload` that calls
  // `remove()` on anything is ordinary code. Doing that from inside
  // `ForEachDescendant` erases from the `children_` vector the range-`for` is
  // iterating, which is undefined behaviour and was a **segfault** reachable
  // from any page: `dom/nodes/moveBefore/relevant-mutations.html` crashed the
  // process here, and its handler does nothing more exotic than resolve a
  // promise whose continuation moves a `<source>` out of a `<picture>`.
  //
  // The second loop re-asks two questions per element rather than trusting the
  // list, because an earlier handler may have removed a later element or
  // changed which image it selected. The pointers themselves stay valid -- the
  // binding layer holds a removed subtree until navigation (ADR 0008) -- so
  // this is about not firing `load` at a node that has left the document, not
  // about the pointer.
  std::vector<const dom::Element*> loaded;
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() != "img") {
      return;
    }
    const auto selected = resources_.selected_image_urls.find(&element);
    if (selected != resources_.selected_image_urls.end() && selected->second == src) {
      loaded.push_back(&element);
    }
  });
  for (const dom::Element* element : loaded) {
    if (element->ConnectedDocument() != document_.get()) {
      continue;  // a previous handler took it out of the document
    }
    const auto selected = resources_.selected_image_urls.find(element);
    if (selected == resources_.selected_image_urls.end() || selected->second != src) {
      continue;
    }
    script_->NotifyElementEvent(*element, "load");
  }
}

std::optional<gfx::SurfaceId> Page::SurfaceForElement(const dom::Element& element) const {
  if (element.TagName() != "video") {
    return std::nullopt;
  }
  return video_.SurfaceFor(element);
}

void Page::WantImage(std::string_view src) const {
  if (src.empty()) {
    return;
  }
  Page& self = const_cast<Page&>(*this);
  if (std::find(self.resources_.pending_images.begin(), self.resources_.pending_images.end(),
                src) != self.resources_.pending_images.end()) {
    return;
  }
  if (!policy_.AllowsUrl(csp::Directive::Img, std::string(src))) {
    return;
  }
  self.resources_.pending_images.push_back(std::string(src));
}

std::shared_ptr<const gfx::Image> Page::ImageFor(std::string_view src) const {
  const auto found = resources_.images.find(src);
  return found == resources_.images.end() ? nullptr : found->second;
}

std::shared_ptr<const gfx::Image> Page::ImageForElement(const dom::Element& element, int css_width,
                                                       int css_height) const {
  // A `<canvas>` is its own image source (ADR 0029 §2): the bitmap the page drew, taken through the same
  // hook an `<img>` uses. That is the whole of what canvas cost layout -- a replaced element whose
  // pixels come from somewhere other than the network.
  if (element.TagName() == "canvas") {
    return canvases_.Snapshot(element);
  }
  // Inline `<svg>`: serialize the element and rasterize at the used size. Same
  // decoder an `<img src="….svg">` uses; without this the logo is a 0×0 box.
  if (element.TagName() == "svg") {
    const std::string markup = element.Serialize();
    const auto bytes = std::as_bytes(std::span<const char>(markup.data(), markup.size()));
    gfx::SvgDecodeResult decoded = gfx::DecodeSvg(bytes, css_width, css_height);
    if (!decoded.Ok() || !decoded.image.IsValid()) {
      return nullptr;
    }
    return std::make_shared<gfx::Image>(std::move(decoded.image));
  }
  const auto selected = resources_.selected_image_urls.find(&element);
  if (selected == resources_.selected_image_urls.end()) {
    // An <img> a script created after the images were collected. Nothing was
    // fetched for it, so there is nothing to draw -- which is what the box
    // already looked like, rather than a new kind of failure.
    return nullptr;
  }
  return ImageFor(selected->second);
}
}  // namespace microbrowser::engine

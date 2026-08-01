#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "css/StyleResolver.h"
#include "gfx/DisplayList.h"
#include "gfx/TextRenderer.h"
#include "layout/FontTextMeasurer.h"
#include "layout/LayoutEngine.h"

namespace microbrowser::engine {

// One loaded document: its DOM, its styles, its box tree, and the display list
// they produce.
//
// Separate from Engine because Engine must not become "the browser" -- see the
// note on its budget in src/engine/MODULE.deps. Engine routes messages; this is
// the thing a message is routed to. It is also the unit that a second tab
// duplicates, which a pile of members on Engine would not be.
//
// It has no window, no canvas, and no way to acquire one: painting produces a
// display list and stops there. Fonts arrive as a gfx::FontProvider from the
// caller, because *which* fonts exist is a property of the machine and the
// engine is the half of the seam that does not know what machine it is on.
class Page {
 public:
  explicit Page(gfx::FontProvider& fonts);

  // Replaces the document. `url` is recorded as the document's address; it is
  // not fetched here, because what a URL turns into is the loader's problem and
  // parsing is this one's.
  void Load(std::string_view html, std::string url);

  // Lays out at `width` CSS pixels and returns the content height, which is
  // what a scrollbar needs.
  float Layout(float width);

  // Records the page into `out`, translated by `scroll_y`. The scroll offset is
  // baked into the geometry rather than expressed as a transform command,
  // because the display list has no transform and adding one to move the page
  // would make every damage rect depend on replaying it.
  void Paint(gfx::DisplayList& out, float scroll_y) const;

  const std::string& Url() const { return url_; }
  // The document's <title>, or the URL when it has none -- which is what a tab
  // strip shows and is never empty.
  const std::string& Title() const { return title_; }
  float ContentHeight() const { return content_height_; }

  // The style sheets in effect. Exposed so a test can add one without a
  // <style> element and so the UI can eventually add a user sheet.
  css::StyleResolver& Styles() { return resolver_; }

 private:
  void ExtractTitle();
  // Collects <style> element text into the resolver. Called on load, before
  // any style is resolved, because a resolver consulted first would cache a
  // cascade that the sheet then changes.
  void CollectStyleSheets();

  gfx::TextRenderer text_;
  layout::FontTextMeasurer measurer_;
  css::StyleResolver resolver_;
  std::unique_ptr<dom::Document> document_;
  std::unique_ptr<layout::Box> boxes_;
  std::string url_;
  std::string title_;
  float content_height_ = 0.0f;
};

}  // namespace microbrowser::engine

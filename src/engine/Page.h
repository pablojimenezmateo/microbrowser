#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
class Page : private layout::ImageProvider {
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

  // Stylesheet URLs the document referenced, in document order, exactly as
  // written. Resolving them against the document is the loader's job, because
  // it is the loader that knows what a base URL is for.
  const std::vector<std::string>& PendingStyleSheets() const { return pending_sheets_; }

  // Adds a fetched stylesheet. Author origin, appended after the document's
  // own <style> elements -- which is wrong for a sheet that appeared earlier
  // in the document, and is the next thing to fix here. Document order within
  // an origin is the last tiebreaker in the cascade, so this only shows up
  // when two rules of equal specificity disagree.
  void AddStyleSheet(std::string_view css);

  // Image URLs the document referenced, in document order, exactly as written.
  const std::vector<std::string>& PendingImages() const { return pending_images_; }

  // Records a decoded image under the `src` the document wrote. Keyed by the
  // written form rather than the resolved one because that is what the element
  // says and what layout has to look up -- resolving is the loader's job, and
  // doing it twice in two places is how the two disagree.
  void AddImage(std::string src, std::shared_ptr<const gfx::Image> image);

  // The link whose laid-out box contains `document_point`, or nullopt.
  // Document coordinates, not viewport coordinates: scrolling is state owned
  // by Engine, and the page's box tree is laid out unscrolled.
  std::optional<std::string> LinkAt(gfx::FloatPoint document_point) const;

  // The GET form target activated at `document_point`, including the encoded
  // query string, or nullopt when no supported form control was activated.
  std::optional<std::string> FormSubmissionAt(gfx::FloatPoint document_point) const;

  // Focuses an editable input at `document_point`, clearing the focused input
  // when the point is not one. Returns true when an input was focused.
  bool FocusInputAt(gfx::FloatPoint document_point);

  // Inserts text into the focused input. Returns true when the document value
  // changed and layout/paint should run.
  bool InsertTextIntoFocusedInput(std::string_view text);

  // Deletes the final entered codepoint from the focused input. The first caret
  // model is end-of-text only; selection and arbitrary caret placement arrive
  // with real form editing.
  bool DeleteBackwardFromFocusedInput();

  // The GET form target for the currently focused input's owning form, or
  // nullopt when no supported form can be submitted.
  std::optional<std::string> SubmitFocusedForm() const;

  const std::string& Url() const { return url_; }
  // The document's <title>, or the URL when it has none -- which is what a tab
  // strip shows and is never empty.
  const std::string& Title() const { return title_; }
  float ContentHeight() const { return content_height_; }

  // The style sheets in effect. Exposed so a test can add one without a
  // <style> element and so the UI can eventually add a user sheet.
  css::StyleResolver& Styles() { return resolver_; }

 private:
  // layout::ImageProvider. Private inheritance: layout asks the page for an
  // image, and nobody else has business calling this.
  std::shared_ptr<const gfx::Image> ImageFor(std::string_view src) const override;

  void ExtractTitle();
  // Collects <style> element text into the resolver. Called on load, before
  // any style is resolved, because a resolver consulted first would cache a
  // cascade that the sheet then changes.
  void CollectStyleSheets();
  void CollectImages();

  gfx::TextRenderer text_;
  layout::FontTextMeasurer measurer_;
  css::StyleResolver resolver_;
  std::unique_ptr<dom::Document> document_;
  std::unique_ptr<layout::Box> boxes_;
  std::string url_;
  std::string title_;
  std::vector<std::string> pending_sheets_;
  std::vector<std::string> pending_images_;
  // Owns the decoded pixels for this document, and hands out shared_ptrs so
  // that a display list can outlive a relayout without copying a bitmap.
  std::map<std::string, std::shared_ptr<const gfx::Image>, std::less<>> images_;
  dom::Element* focused_input_ = nullptr;
  float content_height_ = 0.0f;
};

}  // namespace microbrowser::engine

#pragma once

#include <cstddef>
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
#include "engine/PageScript.h"
#include "layout/LayoutEngine.h"

namespace microbrowser::engine {

struct FormSubmission {
  std::string url;
  std::string method = "GET";
  std::string body;
  std::string content_type;
};

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

  // Anything the page's script wrote with `console.log`, in order. Collected
  // rather than printed: a page must not be able to write to the terminal the
  // browser was started from.
  const std::vector<std::string>& ConsoleOutput() const;

  // Records the page into `out`, translated by `scroll_y`. The scroll offset is
  // baked into the geometry rather than expressed as a transform command,
  // because the display list has no transform and adding one to move the page
  // would make every damage rect depend on replaying it.
  void Paint(gfx::DisplayList& out, float scroll_y) const;

  // Stylesheet URLs the document referenced, in document order, exactly as
  // written. Resolving them against the document is the loader's job, because
  // it is the loader that knows what a base URL is for.
  const std::vector<std::string>& PendingStyleSheets() const { return resources_.pending_sheets; }

  // The external scripts this document referenced, in document order. Fetched
  // by the caller for the same reason a stylesheet is: a fetch needs a privacy
  // verdict, and producing one is the loader's job.
  const std::vector<std::string>& PendingScripts() const { return script_.PendingUrls(); }
  void AddScript(std::size_t pending_index, std::string source);
  // Runs the document's scripts. Idempotent, so a caller that fetches
  // subresources first and one that does not can both end with it.
  void RunScripts();

  // Adds the fetched stylesheet for `PendingStyleSheets()[pending_index]`.
  // Author-origin cascade order is the document order of <style> and <link>,
  // so the index fills a slot rather than appending at load completion time.
  void AddStyleSheet(std::size_t pending_index, std::string_view css);

  // Image URLs the document referenced, in document order, exactly as written.
  const std::vector<std::string>& PendingImages() const { return resources_.pending_images; }

  // The size the document asks for `src` to be drawn at, or a zero extent for
  // an axis nothing states. Only a vector image needs it -- a bitmap has its
  // own size and this is the box it is scaled into -- which is why it is asked
  // for by the loader rather than applied here.
  gfx::IntSize RequestedImageSize(std::string_view src) const;

  // Records a decoded image under the `src` the document wrote. Keyed by the
  // written form rather than the resolved one because that is what the element
  // says and what layout has to look up -- resolving is the loader's job, and
  // doing it twice in two places is how the two disagree.
  void AddImage(std::string src, std::shared_ptr<const gfx::Image> image);

  // The link whose laid-out box contains `document_point`, or nullopt.
  // Document coordinates, not viewport coordinates: scrolling is state owned
  // by Engine, and the page's box tree is laid out unscrolled.
  std::optional<std::string> LinkAt(gfx::FloatPoint document_point) const;

  // The form submission activated at `document_point`, or nullopt when no
  // supported form control was activated.
  std::optional<FormSubmission> FormSubmissionRequestAt(gfx::FloatPoint document_point) const;

  // Focuses an editable text control at `document_point`.
  bool FocusTextControlAt(gfx::FloatPoint document_point);

  // Activates a checkbox or radio input at `document_point`. Returns true when
  // the document value changed and layout/paint should run.
  bool ActivateCheckableInputAt(gfx::FloatPoint document_point);

  // Resets the owning form of a reset input at `document_point`. Returns true
  // when the document value changed and layout/paint should run.
  bool ResetFormAt(gfx::FloatPoint document_point);

  // Inserts text into the focused text control.
  bool InsertTextIntoFocusedTextControl(std::string_view text);

  // Deletes the final entered codepoint from the focused text control.
  bool DeleteBackwardFromFocusedTextControl();

  // The form submission for the currently focused text control's owning form,
  // or nullopt when no supported form can be submitted.
  std::optional<FormSubmission> FocusedFormSubmission() const;

  const std::string& Url() const { return url_; }
  // The document's <title>, or the URL when it has none -- which is what a tab
  // strip shows and is never empty.
  const std::string& Title() const { return title_; }
  float ContentHeight() const { return content_height_; }

  // The style sheets in effect. Exposed so a test can add one without a
  // <style> element and so the UI can eventually add a user sheet.
  css::StyleResolver& Styles() { return resolver_; }

 private:
  struct DocumentResources {
    std::vector<std::string> pending_sheets;
    std::vector<std::size_t> pending_sheet_slots;
    std::vector<std::optional<std::string>> author_sheet_slots;
    std::vector<std::string> pending_images;
    std::map<std::string, std::shared_ptr<const gfx::Image>, std::less<>> images;
  };

  // layout::ImageProvider. Private inheritance: layout asks the page for an
  // image, and nobody else has business calling this.
  std::shared_ptr<const gfx::Image> ImageFor(std::string_view src) const override;

  void ExtractTitle();
  // Collects <style> elements and stylesheet links in document order.
  void CollectStyleSheets();
  void RebuildAuthorStyleSheets();
  void CollectImages();

  gfx::TextRenderer text_;
  layout::FontTextMeasurer measurer_;
  css::StyleResolver resolver_;
  std::unique_ptr<dom::Document> document_;
  // One member rather than an interpreter and a binding layer, which is what
  // the fan-out lint asked for the moment script arrived: Page coordinates,
  // and each thing it coordinates owns itself.
  PageScript script_;
  std::unique_ptr<layout::Box> boxes_;
  std::string url_;
  std::string title_;
  DocumentResources resources_;
  std::map<const dom::Element*, std::pair<std::string, bool>> control_defaults_;
  dom::Element* focused_text_control_ = nullptr;
  float content_height_ = 0.0f;
};

}  // namespace microbrowser::engine

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/Geometry.h"
#include "css/MediaQuery.h"
#include "css/StyleResolver.h"
#include "gfx/DisplayList.h"
#include "gfx/TextRenderer.h"
#include "layout/FontTextMeasurer.h"
#include "engine/PageScript.h"
#include "layout/LayoutEngine.h"

namespace microbrowser::engine {

// What a click did, which is two separate facts.
//
// A handler that changed the document needs a relayout whether or not it
// prevented anything, and a handler that prevented the default may have
// changed nothing at all. Reporting one bit conflated the two, and the visible
// symptom was a page whose handler ran and whose screen did not change.
struct ClickOutcome {
  bool ran = false;
  bool prevented = false;
};

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
class Page : private layout::ImageProvider, private bindings::GeometrySource {
 public:
  explicit Page(gfx::FontProvider& fonts);

  // Replaces the document. `url` is recorded as the document's address; it is
  // not fetched here, because what a URL turns into is the loader's problem and
  // parsing is this one's.
  void Load(std::string_view html, std::string url);

  // Lays out at `width` CSS pixels and returns the content height, which is
  // what a scrollbar needs.
  float Layout(float width);

  // Where the viewport sits over the document, which is what makes a geometry
  // answer viewport-relative rather than document-relative -- the coordinate
  // system `getBoundingClientRect` is defined in.
  //
  // The page owns it rather than the engine so that painting and measuring
  // cannot disagree about it: `Paint` translates the display list by the same
  // number this subtracts, and two copies of a scroll offset is exactly the
  // kind of pair that drifts. The scroll *model* -- a per-box offset, wheel
  // routing, `scrollTop` -- is ADR 0018 and session 8; this is only the
  // viewport's, moved to where both readers are.
  void SetScrollOffsetY(float y);
  float ScrollOffsetY() const { return layout_.scroll_y; }

  // Routes a wheel to the deepest scrolling box under `document_point` that can
  // still move in that direction, and moves it. ADR 0018 §4: when nothing
  // inside the page can take the delta, the answer is `viewport`, and the
  // caller -- which is the half that knows how tall the window is -- moves the
  // document instead. That chaining rule is one line of specification and the
  // difference between a menu that traps a wheel forever and one that hands it
  // on at its end.
  struct ScrollOutcome {
    // A box inside the page moved, and this is what it covers in viewport
    // coordinates. Empty when nothing moved.
    gfx::IntRect damage;
    bool moved = false;
    bool viewport = false;
  };
  ScrollOutcome ScrollAt(gfx::FloatPoint document_point, gfx::FloatPoint delta);

  // Every box that does not move with the document scroll -- `fixed` and
  // `sticky` -- in viewport coordinates. Appended rather than returned so the
  // caller can accumulate the rectangles from before and after a scroll, which
  // is what a blit has to repaint: ADR 0018 §2 names both as the two things
  // that break the blit, and both are known in advance rather than discovered.
  void AppendScrollInvariantRects(std::vector<gfx::IntRect>& out) const;

  // Anything the page's script wrote with `console.log`, in order. Collected
  // rather than printed: a page must not be able to write to the terminal the
  // browser was started from.
  const std::vector<std::string>& ConsoleOutput() const;
  // Every script on this page that ended on a throw. See PageScript.
  const std::vector<std::string>& ScriptErrors() const;

  // Records the page into `out`, translated by the scroll offset. That offset
  // is baked into the geometry rather than expressed as a transform command,
  // because the display list has no transform and adding one to move the page
  // would make every damage rect depend on replaying it.
  void Paint(gfx::DisplayList& out) const;

  // Stylesheet URLs the document referenced, in document order, exactly as
  // written. Resolving them against the document is the loader's job, because
  // it is the loader that knows what a base URL is for.
  const std::vector<std::string>& PendingStyleSheets() const { return resources_.pending_sheets; }

  // The external scripts this document referenced, in document order. Fetched
  // by the caller for the same reason a stylesheet is: a fetch needs a privacy
  // verdict, and producing one is the loader's job.
  const std::vector<std::string>& PendingScripts() const { return script_.PendingUrls(); }
  // Whether `PendingScripts()[index]` is one the page said it would not wait
  // for. The caller asks so it knows which outstanding scripts hold the first
  // paint and which do not -- see PageScript::Timing and ADR 0011.
  bool PendingScriptIsAsync(std::size_t index) const { return script_.IsAsync(index); }
  void AddScript(std::size_t pending_index, std::string source);
  // Runs any `async` script whose source arrived after RunScripts. True when
  // one did, which means the document may have changed.
  bool RunReadyAsyncScripts() { return script_.RunReadyAsync(); }
  // Runs the document's scripts. Idempotent, so a caller that fetches
  // subresources first and one that does not can both end with it.
  void RunScripts(std::int64_t now_ms);

  // Milliseconds until the page's soonest timer or animation frame, or nothing
  // when it has asked for neither. The loop asks this to decide how long it may
  // sleep.
  std::optional<std::uint32_t> NextWakeDelay(std::int64_t now_ms) const;
  // Runs every timer that is due and the animation frame if its boundary has
  // arrived. True when any ran and the page needs laying out again.
  bool RunDueWork(std::int64_t now_ms);

  // Adds the fetched stylesheet for `PendingStyleSheets()[pending_index]`.
  // Author-origin cascade order is the document order of <style> and <link>,
  // so the index fills a slot rather than appending at load completion time.
  void AddStyleSheet(std::size_t pending_index, std::string_view css);

  // The environment an image is selected for: the viewport in CSS pixels and
  // the device pixel ratio. Set by the engine, which is the half of the seam
  // that knows what screen this is; the page only knows what the document asked
  // for. Nothing is re-fetched when it changes -- see CollectImages.
  void SetViewport(const css::MediaContext& viewport);

  // Image URLs the document referenced, in document order, exactly as written.
  // One per <img>, already chosen from its `srcset` and any `<picture>` around
  // it, plus every background image the cascade names.
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
  // supported form control was activated -- or when a `submit` handler called
  // `preventDefault`, which is why this is not const.
  std::optional<FormSubmission> FormSubmissionRequestAt(gfx::FloatPoint document_point);

  // The submission this page's script asked for with `submit()` or
  // `requestSubmit()`, built. Taken after the script turn rather than during
  // it: a navigation replaces the document, and doing that with the
  // interpreter on the stack is a use-after-free. See ADR 0026 §3.
  std::optional<FormSubmission> TakeScriptFormSubmission();

  // Fires `load` and moves `readyState` to "complete". True when something was
  // listening and the document may therefore have changed.
  bool NotifyLoad() { return script_.NotifyLoad(); }

  // Runs the page's click handlers for whatever is at `document_point`, from
  // that element up to the root. Returns true when a handler called
  // `preventDefault`, which is the caller's signal not to follow the link or
  // submit the form it would otherwise have.
  // Deliberately does not invalidate the layout: the hit tests that follow a
  // click run against the tree as it was when the click landed, which is one
  // hit test per click rather than one per question asked about it. The caller
  // relays out afterwards.
  ClickOutcome DispatchClickAt(gfx::FloatPoint document_point);

  // Drops everything derived from the document, so the next Layout rebuilds
  // it. What a script changed is not knowable from here, so nothing is patched.
  void InvalidateLayout();

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
  std::optional<FormSubmission> FocusedFormSubmission();

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
    // Which candidate each <img> resolved to. Recorded rather than recomputed
    // because selection depends on the viewport and the fetch does not: an
    // element whose chosen URL changed after its image was fetched would
    // otherwise render as nothing at all.
    std::map<const dom::Element*, std::string> selected_image_urls;
  };

  // bindings::GeometrySource. Private inheritance for the reason ImageProvider
  // is private: the binding layer holds a reference to the interface, and
  // nothing else has business calling these. See ADR 0015 -- values out, never
  // pointers, and the answer is never stale.
  //
  // Both live in GeometryQueries.cpp, with the property table.
  std::optional<bindings::BoxGeometry> QueryBox(const dom::Node& node) override;
  std::optional<std::string> QueryUsedValue(const dom::Element& element,
                                            std::string_view property) override;
  void SetScrollOffset(const dom::Node& node, float x, float y) override;
  void ScrollIntoView(const dom::Node& node) override;
  // Records that `element` -- or the viewport, when null -- moved, so that one
  // `scroll` event fires at the next frame rather than one per wheel notch.
  // ADR 0018 §3: a page with twelve `scroll` listeners must not run them twelve
  // times for one notch, and the throttling belongs here rather than at each
  // caller because there are four callers and they must agree.
  void NoteScrolled(const dom::Element* element);
  // Whether `element` is the one whose scroll offset *is* the viewport's.
  // `document.documentElement.scrollTop` is the document's offset in every
  // standards-mode browser, and no markup expresses that -- so it is a rule
  // here rather than a property of a box.
  bool IsViewportScroller(const dom::Element& element) const;
  // Fires one `scroll` event per target that moved since the last frame. True
  // when anything was listening, which is the caller's signal that the document
  // may have changed.
  bool DispatchPendingScrollEvents();
  // Runs layout if anything has changed the document since the last one, and
  // counts it as forced. The one place a geometry question can cost a page
  // arbitrary work, which is why it is also the one place that counts it.
  void EnsureLayoutClean();
  // The cascade for an element that generated no box -- `display: none`, or a
  // subtree script has built and not inserted. Resolved down the ancestor
  // chain, because inheritance only runs that direction.
  css::ComputedStyle StyleWithoutBox(const dom::Element& element) const;

  // layout::ImageProvider. Private inheritance: layout asks the page for an
  // image, and nobody else has business calling this.
  std::shared_ptr<const gfx::Image> ImageFor(std::string_view src) const override;
  std::shared_ptr<const gfx::Image> ImageForElement(const dom::Element& element) const override;

  // One route from "this form is being submitted" to a submission: fire the
  // `submit` event, and build the data set only if nothing prevented it. A
  // click, the Enter key and a script all arrive here, so `preventDefault`
  // means the same thing to all three.
  std::optional<FormSubmission> SubmitForm(const dom::Element& form,
                                           const dom::Element* submitter);
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
  css::MediaContext viewport_;
  // What the last layout was for, so a query that forces one can repeat it.
  // One member rather than three: the width it ran at, the document version it
  // described, and where the viewport sits over the result are three facts
  // about one layout, and loose on Page they would say nothing about belonging
  // together.
  struct LayoutState {
    // What the next forced layout runs at. Set by SetViewport as well as by
    // Layout, because a script can ask for a rectangle before the first layout
    // of a document has happened -- and laying that one out at zero would
    // answer with a page one column wide.
    float width = 0.0f;
    // dom::Document::MutationVersion() as of the last layout. Anything that
    // changes the tree moves it, so a mismatch is the layout-clean flag of
    // ADR 0015 -- and it is a comparison rather than a bit because a bit that
    // each reader cleared would hide the change from the next.
    std::uint64_t document_version = 0;
    float scroll_y = 0.0f;
  };
  LayoutState layout_;
  // Everything about scrolling that is not the viewport's own offset: where
  // each scrolling element sits, and who owes a `scroll` event at the next
  // frame. One member and not two, for the reason LayoutState is one: they are
  // two facts about the same thing, and loose on Page they would say nothing
  // about belonging together.
  struct ScrollState {
    // Survives a relayout, which is the whole reason it is not on the box: the
    // box tree is rebuilt from scratch every time and a menu that was scrolled
    // down would jump back to the top on the next class change.
    layout::ScrollOffsets offsets;
    // Who moved since the last frame. Null stands for the viewport, which has
    // no element of its own -- and the list is deliberately small: it is empty
    // on a settled page, which is what keeps the loop asleep.
    std::vector<const dom::Element*> pending_events;
  };
  ScrollState scroll_;
  float content_height_ = 0.0f;
};

}  // namespace microbrowser::engine

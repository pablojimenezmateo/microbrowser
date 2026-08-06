#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/Geometry.h"
#include "bindings/Network.h"
#include "bindings/Media.h"
#include "engine/DocumentPolicy.h"
#include "bindings/Canvas.h"
#include "engine/Animations.h"
#include "engine/CanvasSurfaces.h"
#include "engine/MediaElements.h"
#include "css/MediaQuery.h"
#include "css/StyleResolver.h"
#include "gfx/DisplayList.h"
#include "gfx/TextRenderer.h"
#include "layout/FontTextMeasurer.h"
#include "engine/PageScript.h"
#include "layout/LayoutEngine.h"

namespace microbrowser::engine {

// What dispatching an event did, which is two separate facts.
//
// A handler that changed the document needs a relayout whether or not it
// prevented anything, and a handler that prevented the default may have
// changed nothing at all. Reporting one bit conflated the two, and the visible
// symptom was a page whose handler ran and whose screen did not change.
struct DispatchOutcome {
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
class Page : private layout::ImageProvider,
             private bindings::GeometrySource,
             private css::StyleAdjuster,
             public bindings::CanvasSurface,
             public bindings::MediaController {
 public:
  explicit Page(gfx::FontProvider& fonts);

  // Replaces the document. `url` is recorded as the document's address; it is
  // not fetched here, because what a URL turns into is the loader's problem and
  // parsing is this one's.
  //
  // `header_policy` is the `Content-Security-Policy` the response carried.
  // Passed in at Load rather than set afterwards because the policy has to be
  // in force *before* the document's own scripts and stylesheets are collected
  // -- a blocked inline script that was collected and then filtered would be
  // one refusal away from running.
  // `content_type` is the response header exactly as it arrived, and it is a parameter rather than
  // something this class digs out because the *encoding* is decided from it (ADR 0025 §2) and the
  // decision has to happen before a byte reaches the tokenizer.
  void Load(std::string_view html, std::string url, csp::PolicyList header_policy,
            std::string_view content_type = std::string_view());
  // The same for a document that arrived with no policy, which is most of them
  // and every test that does not care.
  void Load(std::string_view html, std::string url) {
    Load(html, std::move(url), csp::PolicyList{});
  }

  // This document's policy, and what it is relative to. The engine asks it the
  // two questions this class cannot answer for itself: whether a `fetch` may go
  // out (`connect-src`) and whether a form may be submitted (`form-action`).
  const DocumentPolicy& Policy() const { return policy_; }

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
  const std::vector<SubresourceRequest>& PendingStyleSheets() const {
    return resources_.pending_sheets;
  }

  // The external scripts this document referenced, in document order. Fetched
  // by the caller for the same reason a stylesheet is: a fetch needs a privacy
  // verdict, and producing one is the loader's job.
  const std::vector<SubresourceRequest>& PendingScripts() const { return script_.PendingUrls(); }
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

  // Samples this page's `IntersectionObserver`s and `ResizeObserver`s against
  // the layout about to be painted, and runs the callbacks whose answers
  // changed. True when one ran and the page therefore needs painting again.
  //
  // Called at the frame and nowhere else -- ADR 0018 §5. That is the whole
  // design: an observer that fired from inside a scroll would run its callback
  // once per wheel notch and could see a layout part-way through being
  // rebuilt, and one that fired from a timer would be a 60Hz wakeup on a page
  // nothing is happening to.
  bool DeliverObservations(std::int64_t now_ms);

  // The `navigation` entry for this document and one `resource` entry per
  // subresource, from the engine -- the only thing that knows when a request
  // started. A `PerformanceObserver` with nothing behind it is the stub ADR 0012
  // forbids, which is why these exist rather than the observer alone.
  void SetNavigationTiming(double dom_content_loaded_ms, double load_event_ms,
                           double duration_ms) {
    script_.SetNavigationTiming(dom_content_loaded_ms, load_event_ms, duration_ms);
  }
  void AddResourceTiming(const std::string& name, const std::string& initiator, double start_ms,
                         double response_end_ms, std::size_t encoded_size,
                         std::size_t decoded_size) {
    script_.AddResourceTiming(name, initiator, start_ms, response_end_ms, encoded_size,
                              decoded_size);
  }

  // Where this page's own requests go, set once by the engine that owns both
  // this and the loader. Null in a page with no network behind it, which is an
  // absence rather than a stub: `fetch` is then not declared at all. See
  // bindings/Network.h and ADR 0012.
  void SetNetworkSource(bindings::NetworkSource* network);
  // The same, for `window.history`. Borrowed and set before any script runs, for
  // the reason the network source is: a source that arrived later would leave the
  // first script of a document without one, and `history` is declared or not
  // declared at construction.
  void SetHistorySource(bindings::HistorySource* history);
  // ADR 0021. Handed over for the life of the engine like the other two: a store that
  // arrived later would leave the first script of the first document without one, and
  // that first script is exactly where Plex looks for `sessionStorage`.
  void SetStorageSource(bindings::StorageSource* storage);
  // Hands one answer to the script that asked for it. False when nothing was
  // waiting -- an aborted request, or a second delivery -- which the caller
  // drops rather than repainting for.
  bool DeliverFetchResponse(std::uint64_t id, const bindings::ScriptResponse& response);

  // A socket's events. ADR 0020 §5, and they take the same road a fetch response does:
  // the engine read the bytes, the page owns the script, and nothing between them can
  // fabricate one.
  bool DeliverSocketOpen(std::uint64_t id);
  bool DeliverSocketMessage(std::uint64_t id, const std::string& data, bool text);
  bool DeliverSocketClose(std::uint64_t id, std::uint16_t code, const std::string& reason,
                          bool clean, bool failed);
  bool DeliverEventSourceOpen(std::uint64_t id);
  bool DeliverEventSourceMessage(std::uint64_t id, const std::string& type,
                                 const std::string& data, const std::string& last_id);
  // True when a handler ran. `permanent` says whether a reconnect will follow, which is
  // what lets a page show "reconnecting" rather than "failed".
  bool DeliverEventSourceError(std::uint64_t id, bool permanent);

  // Where a page's own sockets are answered, handed over for the life of the engine like
  // the other sources.
  void SetSocketSource(bindings::SocketSource* sockets);

  // --- web fonts, ADR 0024 --------------------------------------------------

  // One `@font-face` the document declared and the URL to fetch for it. The
  // *chosen* source: the first one whose declared format this browser can decode,
  // which is what the author's source order means. A face whose every source is
  // undecodable is not here at all -- fetching a WOFF2 to fail on it is a
  // request that buys nothing.
  struct PendingFontFace {
    std::string url;
    std::string family;
    int weight = 400;
    bool italic = false;
  };
  // The ones nobody has been handed yet, marked as handed out -- the same take
  // the image list uses and for the same reason: the face list is rebuilt from the
  // sheets every time one lands.
  std::vector<PendingFontFace> TakeUnrequestedFontFaces();
  // A face's bytes. True when the provider took them, which is false for a format
  // it cannot decode -- and a refused face is a page rendering in the next family
  // of its stack, which is what a stack is for.
  bool AddWebFont(const PendingFontFace& face, std::vector<std::byte> bytes);

  // --- modules -------------------------------------------------------------
  // The engine fetches; the page decides what needs fetching. See
  // engine/ModuleLoader.h for why the two are split.
  std::vector<std::string> TakeModuleFetches() { return script_.TakeModuleFetches(); }
  void AddModuleSource(std::string url, std::string source) {
    script_.AddModuleSource(std::move(url), std::move(source));
  }
  // Settles every dynamic import whose graph has closed. True when one did, which
  // means a page's code ran.
  bool AdvanceModules() { return script_.AdvanceModules(); }
  bool HasPendingModules() const { return script_.HasPendingModules(); }

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
  // it, plus every background image the cascade names. An `<img loading="lazy">`
  // that is not near the scrollport yet is *not* here -- see RevealLazyImages.
  const std::vector<std::string>& PendingImages() const { return resources_.pending_images; }

  // The ones nobody has been handed yet, marked as handed out.
  //
  // A take rather than a read, because the list above is rebuilt from the
  // document every time anything changes it and the loader must not be told to
  // fetch the same URL again on the next stylesheet that lands. The bookkeeping
  // is here rather than on the caller so that the two callers -- the initial
  // subresource pass and the frame that reveals a lazy image -- cannot disagree
  // about what has been asked for.
  std::vector<std::string> TakeUnrequestedImages();

  // Moves every deferred `<img loading="lazy">` whose box has come within reach
  // of the scrollport into the pending list. True when one did, which is the
  // caller's signal to go and fetch it.
  //
  // Reach is one viewport height and width beyond the scrollport in every
  // direction, which is the number this browser chose rather than one any
  // specification states: it is far enough that an image is usually there
  // before it is scrolled to, and near enough that a page of two hundred
  // thumbnails fetches a handful. ADR 0018 §5.
  bool RevealLazyImages();

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

  // Moves this document's address without replacing the document: a
  // `pushState`, a `replaceState`, or a traversal between two entries that
  // belong to it. `:target` is recomputed, because it comes from the fragment
  // and one copy of it is what stops the address bar and the cascade from
  // disagreeing -- and the base URL moves with it, unless a `<base href>` claimed
  // it, which wins.
  void UpdateUrl(std::string url);
  // Fires `popstate` at the window. True when something was listening.
  bool NotifyPopState() { return script_.NotifyPopState(); }
  // Fires `hashchange`. True when something was listening.
  bool NotifyHashChange(const std::string& old_url, const std::string& new_url) {
    return script_.NotifyHashChange(old_url, new_url);
  }

  // Runs the page's click handlers for whatever is at `document_point`, from
  // that element up to the root. Returns true when a handler called
  // `preventDefault`, which is the caller's signal not to follow the link or
  // submit the form it would otherwise have.
  // Deliberately does not invalidate the layout: the hit tests that follow a
  // click run against the tree as it was when the click landed, which is one
  // hit test per click rather than one per question asked about it. The caller
  // relays out afterwards.
  DispatchOutcome DispatchClickAt(gfx::FloatPoint document_point,
                               const bindings::PointerInput& pointer);

  // Fires `keydown` or `keyup` at whatever has focus. `prevented` is the
  // caller's signal not to run the key's default action; `ran` says a handler
  // may have changed the document, which is a different question and the reason
  // this is not one bool.
  DispatchOutcome DispatchKeyToFocus(const bindings::KeyInput& key);

  // Drops everything derived from the document, so the next Layout rebuilds
  // it. What a script changed is not knowable from here, so nothing is patched.
  void InvalidateLayout();

  // --- dynamic state, ADR 0016 §2-3 -----------------------------------------

  // Whether any rule in this page's cascade depends on `state` at all. The
  // caller asks *first*, and this is the whole of the ADR's headline property:
  // a pointer crossing a page with no `:hover` rules must not cost a hit test,
  // let alone a cascade. It is a question about the stylesheets rather than
  // about the document, so it is answerable before anything is looked up.
  bool StyleDependsOn(dom::ElementState state) const;

  // Moves `:hover` to the element under `document_point` and its ancestors, and
  // `:active` to the same chain while a button is held. Returns every state
  // that actually changed on any element -- empty when the pointer moved within
  // the same chain, which is most pointer moves.
  dom::ElementState UpdateHoverChain(gfx::FloatPoint document_point, bool active);
  // Drops the hover and active chain, for a pointer that left the window.
  dom::ElementState ClearHoverChain();

  // What re-resolving the cascade would cost, given that `changed` changed.
  css::StyleChangeEffect StateChangeEffect(dom::ElementState changed) const;

  // Re-resolves the cascade over the box tree that is already laid out, and
  // leaves every geometry alone.
  //
  // Correct only when the caller has established through StateChangeEffect that
  // every rule keyed on what changed affects paint alone -- which is why the
  // two are next to each other. It is the second of ADR 0016 §3's two
  // properties: a `:hover` that changes a colour is a repaint of one damage
  // rectangle, not a relayout of the document.
  void RestyleWithoutLayout();

  // Moves focus to whatever a click at `document_point` landed on: the nearest
  // focusable ancestor of the element under it, or nothing when there is none
  // -- which blurs whatever had focus, the way a click on the background does
  // in every browser. True when focus moved, which is the caller's signal that
  // handlers ran and the screen may need repainting.
  bool FocusFromClickAt(gfx::FloatPoint document_point);

  // Moves focus to the next or previous tab-reachable element in the order
  // ADR 0017 §4 names: positive `tabindex` first in increasing order, then
  // everything else in tree order. True when it moved. This is Tab's default
  // action, so it runs after dispatch and only if nothing cancelled it.
  bool MoveFocusByTab(bool backwards);

  // The element with focus, or null. Every key goes here, and hit testing is
  // consulted only for pointer events -- ADR 0017 §4, and the split that makes
  // a text field work without a second mechanism.
  dom::Element* FocusedElement() const;

  // --- media, in PageMedia.cpp ---------------------------------------------
  // ADR 0028 §1's `bindings::MediaController`. Implemented here rather than in the engine
  // because the state is per *element* and the elements are this object's, and public rather
  // than private inheritance would let anything holding a Page drive playback.
  PlayResult Play(dom::Element& element) override;
  void Pause(dom::Element& element) override;
  void Seek(dom::Element& element, double seconds) override;
  void SetMuted(dom::Element& element, bool muted) override;
  void SetVolume(dom::Element& element, double volume) override;
  double CurrentTime(const dom::Element& element) const override;
  double Duration(const dom::Element& element) const override;
  double Volume(const dom::Element& element) const override;
  int ReadyState(const dom::Element& element) const override;
  int NetworkState(const dom::Element& element) const override;
  bool Paused(const dom::Element& element) const override;
  bool Ended(const dom::Element& element) const override;
  bool Muted(const dom::Element& element) const override;
  bool IsMedia(const dom::Element& element) const override;
  // --- MSE, in PageMediaSource.cpp (ADR 0028 §3) -----------------------------
  //
  // Every one of these is a lookup in `MediaElements`' tables plus a call into `media`. Nothing here
  // decides anything about a stream: the append algorithm, the quota and the codec allowlist all live
  // in `src/media`, and this is the layer that turns an id into an object and an object into an id.
  std::uint64_t CreateMediaSource() override;
  std::string CreateObjectUrl(std::uint64_t source_id) override;
  void RevokeObjectUrl(const std::string& url) override;
  bool AttachMediaSource(dom::Element& element, const std::string& url) override;
  std::uint64_t SourceForObjectUrl(const std::string& url) const override;
  int SourceReadyState(std::uint64_t source_id) const override;
  double SourceDuration(std::uint64_t source_id) const override;
  void SetSourceDuration(std::uint64_t source_id, double seconds) override;
  void EndOfStream(std::uint64_t source_id) override;
  std::uint64_t AddSourceBuffer(std::uint64_t source_id, const std::string& mime_type,
                                bindings::MediaController::AddBufferError& error) override;
  void RemoveSourceBuffer(std::uint64_t source_id, std::uint64_t buffer_id) override;
  int AppendToSourceBuffer(std::uint64_t buffer_id, std::string_view bytes) override;
  void RemoveFromSourceBuffer(std::uint64_t buffer_id, double start, double end) override;
  void AbortSourceBuffer(std::uint64_t buffer_id) override;
  void SetTimestampOffset(std::uint64_t buffer_id, double seconds) override;
  double TimestampOffset(std::uint64_t buffer_id) const override;
  void SetAppendWindow(std::uint64_t buffer_id, double start, double end) override;
  bool SourceBufferUpdating(std::uint64_t buffer_id) const override;
  std::vector<double> SourceBufferBuffered(std::uint64_t buffer_id) const override;
  bool IsLiveSourceBuffer(std::uint64_t buffer_id) const override;
  std::vector<std::string> TakeSourceBufferEvents(std::uint64_t buffer_id) override;
  std::vector<std::string> TakeMediaSourceEvents(std::uint64_t source_id) override;
  // What an append changed about the *element*: MSE reports what it holds and the element's state
  // machine decides what that means. One direction, and one number across the seam.
  void UpdateMediaReadinessFromSource(std::uint64_t source_id);

 public:
  // --- `bindings::CanvasSurface` (ADR 0029 §2), in PageCanvas.cpp --------------
  //
  // Every one is a lookup plus a call into `CanvasSurfaces`. Public because the binding layer holds the
  // interface, like `MediaController`'s half.
  bool IsCanvas(const dom::Element& element) const override;
  void SetCanvasSize(dom::Element& element, int width, int height) override;
  int CanvasWidth(const dom::Element& element) const override;
  int CanvasHeight(const dom::Element& element) const override;
  void ExecuteCanvasOp(dom::Element& element, const bindings::CanvasOp& op) override;
  std::vector<std::uint8_t> ReadCanvasPixels(const dom::Element& element, int x, int y, int width,
                                             int height) const override;
  bool CanvasIsTainted(const dom::Element& element) const override;
  void WriteCanvasPixels(dom::Element& element, int x, int y, int width, int height,
                         const std::vector<std::uint8_t>& rgba) override;
  double MeasureCanvasText(const dom::Element& element, const std::string& text) const override;


  // `css::StyleAdjuster`: the animation pass over a resolved style. Private, and reached only through
  // the resolver -- so nothing can apply an animated value without going through the cascade, which is
  // what keeps layout and `getComputedStyle` from disagreeing mid transition.
  void AdjustStyle(const dom::Element& element, css::ComputedStyle& style) const override;
  // The `@keyframes` a sheet defined, added to what this document knows. Accumulated across sheets
  // rather than replaced per sheet, because a page routinely defines its animations in one file and
  // uses them from another -- and a later definition of the same name replaces the earlier one, which
  // is the cascade rule for a named animation.
  void CollectKeyframes(const css::StyleSheet& sheet);
  // A fresh resolver, **with the animation pass re-registered**. One function because there are two
  // places that need a fresh one -- a navigation and a re-parse of the sheets after a resize -- and the
  // second of them silently dropped the adjuster when it was two lines. A pointer set in one place and
  // clobbered by an assignment in another is the same shape of bug as two lists of which properties
  // inherit: it produces no error, and the feature just stops.
  void ResetResolver();

  // What the loader will drive as bytes arrive, and what a test drives directly. Public because
  // the engine half of session 25 is not built yet and this is the seam it will use.
  media::MediaState* MediaStateFor(const dom::Element& element);
  const media::MediaState* MediaStateFor(const dom::Element& element) const;
  MediaElements& MediaElementStates() { return media_; }
  // The running transitions and animations (ADR 0014 §5). Exposed so the engine can ask for the frame
  // deadline -- which is the one number that decides whether the loop blocks or wakes.
  // What time an animation pass believes it is.
  //
  // **Every restyle uses it, and a restyle happens for more reasons than a frame does** -- a hover, a
  // script write, a resize, and `InvalidateLayout`, which resolves the cascade again to collect
  // background images. So this has to be set before *any* of them, not before painting: a transition
  // that started during a pass whose clock was stale begins at the wrong instant and is already over by
  // the time anything looks at it. That is exactly how the first version of this failed, and the
  // symptom was a transition that showed only its final value.
  //
  // One number per turn, for the reason the animation-frame callbacks share a timestamp: two elements
  // animating on one frame must be at the same instant.
  void SetAnimationTime(std::int64_t now_ms) { animation_time_ms_ = now_ms; }
  Animations& RunningAnimations() { return animations_; }
  // The `<canvas>` backing stores (ADR 0029 §2). `mutable` for the reason `animations_` is: the paint
  // path reads a snapshot from a const method, and taking one is a read of what is already there.
  CanvasSurfaces& Canvases() { return canvases_; }
  const Animations& RunningAnimations() const { return animations_; }
  // Fires whatever the state machine has queued, in order.
  void FlushMediaEvents(dom::Element& element);

  // The box tree, read-only, or null before the first layout. Public for the same reason
  // `CurrentDocument` is: a test has to be able to ask what layout produced -- and the question this
  // answers, "how many lines did that paragraph break into", has no other observer.
  const layout::Box* Boxes() const { return boxes_.get(); }

  // The document, read-only. Public because ADR 0017's user activation lives on it and a test
  // has to be able to ask whether a *page's own* click set it -- which is the property that
  // makes autoplay refusable at all. Const, so the only writer stays this class.
  const dom::Document* CurrentDocument() const { return document_.get(); }
  dom::Document* MutableDocument() { return document_.get(); }
  // The style the cascade *and the animation pass* produce for an element, which is the only way to
  // assert that layout sees an interpolated value rather than that an animation object holds one. Named
  // for what it is: nothing in the browser calls it.
  css::ComputedStyle StyleOfForTesting(const dom::Element& element) const;

  // Whether the current focus came from the keyboard: the `:focus-visible`
  // heuristic every browser converged on. Set by Tab, cleared by a click or by
  // `element.focus()`. The state, and not the selector -- matching
  // `:focus-visible` needs ADR 0016's element state bits, which is session 11.
  bool FocusIsVisible() const;

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
  // What a relative URL in this document resolves against: `<base href>` when
  // the document has one the policy allowed, and the address otherwise. The
  // loader uses this rather than re-parsing `Url()`, because two answers to
  // "what is this document's base" is how a `<base>` ends up applying to the
  // stylesheets and not to the images.
  const std::optional<url::Url>& BaseUrl() const { return policy_.Base(); }
  // The document's <title>, or the URL when it has none -- which is what a tab
  // strip shows and is never empty.
  const std::string& Title() const { return title_; }
  float ContentHeight() const { return content_height_; }

  // The style sheets in effect. Exposed so a test can add one without a
  // <style> element and so the UI can eventually add a user sheet.
  css::StyleResolver& Styles() { return resolver_; }

 private:
  struct DocumentResources {
    std::vector<SubresourceRequest> pending_sheets;
    // The `<style>` text inside each shadow root, with the root it belongs to.
    // Kept as the pair rather than added straight to the resolver because
    // RebuildAuthorStyleSheets throws the resolver away and rebuilds it, and a
    // component's styles have to come back with it. ADR 0019 §3.
    std::vector<std::pair<const dom::Node*, std::string>> shadow_sheets;
    // The `@font-face` blocks the author sheets declared, and the URLs already
    // asked for. Kept because RebuildAuthorStyleSheets throws the parsed sheet
    // away, and a face declared in the first sheet must survive the second
    // arriving.
    std::vector<css::FontFace> font_faces;
    std::set<std::string, std::less<>> requested_fonts;
    std::vector<std::size_t> pending_sheet_slots;
    std::vector<std::optional<std::string>> author_sheet_slots;
    std::vector<std::string> pending_images;
    std::map<std::string, std::shared_ptr<const gfx::Image>, std::less<>> images;
    // The lazy images this document has not asked for yet, and their chosen
    // URL. Keyed by element because "is it near the scrollport" is a question
    // about a box, and two `<img loading="lazy">` sharing a URL are two boxes.
    std::map<const dom::Element*, std::string> deferred_images;
    // Every image URL the loader has already been told to fetch. It survives
    // CollectImages, which rebuilds `pending_images` from the document from
    // scratch -- without this, every stylesheet that lands would re-request the
    // whole page's images.
    std::set<std::string, std::less<>> requested_images;
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
  bindings::GeometryRect QueryViewport() override;
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
  // Moves focus and fires the four events, or -- on a page with no script --
  // writes the document's focus directly. One private helper so that the three
  // callers (a click, Tab, and losing the document) cannot disagree about
  // which of the two paths a page without an interpreter takes.
  bool MoveFocus(dom::Element* target, bool visible);
  // The focused element when a key can type into it, and null otherwise. Every
  // editing routine starts here, so "is this thing editable" is answered once
  // rather than once per routine.
  dom::Element* MutableFocusedTextControl() const;
  void ExtractTitle();
  // Recomputes the dynamic states that are facts about the document rather than
  // about the pointer: `:checked`, `:disabled`, `:required`,
  // `:placeholder-shown`. Run before every cascade, because they are defined in
  // `src/html`'s vocabulary and `src/css` may not see that module -- so the bit
  // is how the answer crosses. Recomputing rather than maintaining means there
  // is nothing to forget at each of the dozen places that can change one.
  void RefreshDocumentStates();
  // Sets `:target` from the URL's fragment. A document property, like focus,
  // and set where the URL arrives so that it cannot disagree with the address.
  void RefreshTargetState();
  // Puts `state` on exactly the elements in `on` and takes it off every other
  // element in the document. Returns the states that actually changed, which is
  // empty for the pointer move that stayed inside the same chain -- and that is
  // most pointer moves.
  //
  // A set rather than a deepest element, because the two states written this
  // way have different shapes: `:hover` is on an element and every ancestor of
  // it, and `:target` is on one element and none of its ancestors.
  dom::ElementState SetStateOn(const std::vector<const dom::Element*>& on,
                               dom::ElementState state);
  // The innermost element whose box contains `document_point`, or null. The
  // same question a click asks, answered in one place so that what a click
  // focuses and what a pointer hovers cannot be two different elements.
  const dom::Element* ElementAt(gfx::FloatPoint document_point) const;
  // Collects <style> elements and stylesheet links in document order.
  void CollectStyleSheets();
  void RebuildAuthorStyleSheets();
  void CollectImages();
  // Reads the document's `<meta http-equiv="Content-Security-Policy">` elements
  // and its `<base href>`, in that order, because a `<base>` is subject to the
  // `base-uri` a `<meta>` may have just declared.
  void ApplyDocumentHeadPolicy();
  // Collects every `<style>` inside every shadow root, as scoped sheets.
  //
  // Separate from CollectStyleSheets because a shadow root is deliberately
  // unreachable from the document -- that is the point of it -- so the document
  // walk cannot find these. Returns true when the set *changed*, which is what
  // decides whether the cascade has to be rebuilt: a page that mutates a
  // component's contents forty times a second must not re-parse its stylesheet
  // forty times a second.
  bool CollectShadowStyleSheets();

  gfx::TextRenderer text_;
  layout::FontTextMeasurer measurer_;
  css::StyleResolver resolver_;
  std::unique_ptr<dom::Document> document_;
  // ADR 0028 §1's per-element state, in its own class. It was two maps here and the architecture
  // lint refused it -- five modules' worth of members on one class -- which was the right call:
  // what this class does with media is coordinate, and what MediaElements does is own the map.
  // Mutable because a *read* creates it. `video.networkState` on an element nobody has touched
  // has to answer LOADING when it has a `src` -- that is what the attribute means -- and
  // creating on first use is what makes fifty untouched `<video>` elements in a feed cost
  // nothing. A const getter that answered defaults instead was the first version, and it read as
  // "no source" on an element that had one.
  mutable MediaElements media_;
  // Running transitions and animations. `mutable` because `AdjustStyle` is const -- it is called from
  // the cascade, which is const by construction -- and reading the current value of a running
  // transition is a read. Nothing here *starts* one from a const path: that is `ObserveStyle`, called
  // from the layout pass.
  mutable Animations animations_;
  mutable CanvasSurfaces canvases_;
  // What time the animation pass believes it is. One number for the whole frame, for the reason the
  // animation-frame callbacks share a timestamp: two elements animating on one frame must be at the
  // same instant, and reading a clock per element is how two halves of one transition desynchronise.
  std::int64_t animation_time_ms_ = 0;
  // Every `@keyframes` this document has seen, kept so that a second stylesheet's arrival does not drop
  // the first one's animations. The copy in `animations_` is what a frame reads; this is what a merge
  // starts from.
  std::vector<css::KeyframesRule> keyframes_;
  // One member rather than an interpreter and a binding layer, which is what
  // the fan-out lint asked for the moment script arrived: Page coordinates,
  // and each thing it coordinates owns itself.
  PageScript script_;
  std::unique_ptr<layout::Box> boxes_;
  std::string url_;
  std::string title_;
  DocumentResources resources_;
  std::map<const dom::Element*, std::pair<std::string, bool>> control_defaults_;
  css::MediaContext viewport_;
  // The page's own Content-Security-Policy, and the base its URLs resolve
  // against. One member and not three: see DocumentPolicy.h.
  DocumentPolicy policy_;
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

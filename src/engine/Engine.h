#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bindings/History.h"
#include "bindings/Network.h"
#include "engine/Loader.h"
#include "engine/Page.h"
#include "engine/PendingLoad.h"
#include "engine/SessionHistory.h"
#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "ipc/Message.h"
#include "ipc/Transport.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::engine {

// The engine half of the seam.
//
// It loads a URL, parses it into a document, resolves its styles, lays it out,
// and paints it into a display list. The properties that matter are structural:
//
//   * It talks to the outside world only through ipc::EngineEndpoint. It has no
//     window handle, no renderer, no canvas, and no way to acquire one.
//   * It is driven, never driving. HandlePendingMessages() and Advance() run to
//     completion and return; neither owns a loop or a thread. A future process
//     split gives it its own loop without changing anything above.
//   * Painting is producing a display list. It never touches a pixel.
//
// Since ADR 0011 a navigation *starts* rather than happens: `Navigate` sends the
// document request and returns, and the load moves forward one turn at a time
// through `Advance()`. What that buys is a browser whose loop is not blocked for
// the length of a fetch, and what it costs is the state below -- which a
// navigation has to be able to throw away, because a response for a document
// that is gone must be dropped.
//
// The temptation this class must resist for the next year is becoming the place
// where "the browser" lives. Document, navigation history, network, and script
// each get their own type; Engine stays the thing that routes messages to them.
// Its budget in src/engine/MODULE.deps is the tripwire.
class Engine : private bindings::NetworkSource, private bindings::HistorySource {
 public:
  // Fonts arrive from the caller because which fonts exist is a property of
  // the machine, and the engine is the half of the seam that does not know
  // what machine it is on. That is the same reason it has no window.
  Engine(ipc::EngineEndpoint& endpoint, gfx::FontProvider& fonts);

  // Drain and act on everything the UI has queued. Returns true when the engine
  // produced any outgoing message, which is what tells the host loop a repaint
  // may be pending.
  bool HandlePendingMessages();

  // Carries the load in flight as far as it can go without blocking, and acts
  // on whatever arrived. True when anything happened, which is the host loop's
  // signal that there may be a frame to show.
  bool Advance();

  // Milliseconds until the engine's soonest deadline: a page timer, an
  // animation frame, or the point at which a silent server is given up on.
  // Nothing when it has none -- which is the answer that lets the loop block.
  std::optional<std::uint32_t> NextDeadlineMs() const;
  // Runs every timer that is due and the animation frame if its boundary has
  // arrived, and repaints when one changed the page. True when anything ran.
  bool RunDueWork();

  // What the loop's single blocking wait must watch for this engine to make
  // progress. Appends rather than assigns, because the loop waits on more than
  // one source.
  void AppendWaitDescriptors(util::WaitDescriptorList& out) const;
  // True when something can move with no wait at all. A socket is almost never
  // in this state and a canned transport always is; without the question the
  // loop would block on input while a test's load stood still.
  bool HasRunnableWork() const;
  // True while a navigation has not finished, or while an image the document
  // asked for after it is still in flight. The snapshot tool and the tests
  // drive the loop until this goes false.
  //
  // The second half is not padding: an `<img loading="lazy">` on screen is
  // requested at the *first frame*, which is after the navigation is over as
  // far as everything else is concerned, and a browser that stopped turning
  // there would show a page with holes where its visible images go.
  bool IsLoading() const {
    return load_.active || !late_images_.empty() || !script_fetches_.empty() ||
           !module_fetches_.empty() || page_.HasPendingModules();
  }

  // What the page's script threw, so a host that is debugging one can say why
  // a document rendered the way it did. Forwarded rather than exposing the
  // Page, which would put the whole engine on the wrong side of the seam.
  const std::vector<std::string>& ScriptErrors() const { return page_.ScriptErrors(); }
  // The other half of the same question, and forwarded for the same reason. A
  // page that ran correctly and said something is not distinguishable from one
  // that threw, from outside, without both.
  const std::vector<std::string>& ConsoleOutput() const { return page_.ConsoleOutput(); }

  // What has focus, as one line: its tag, its `id` or `name`, and whether the
  // keyboard put it there. Forwarded rather than exposing the Page, for the
  // reason ScriptErrors is -- and here at all because every check from ADR
  // 0017 onwards is phrased as an interaction, and "where did that click send
  // focus" was otherwise a question only a debugger could answer. A wrong
  // answer to it looks exactly like a working browser until a key is pressed.
  std::string FocusDescription() const;

  const std::string& Title() const { return page_.Title(); }
  const std::string& Url() const { return page_.Url(); }
  gfx::IntSize ViewportSize() const { return viewport_size_; }

  // The loader, so a caller can install a transport or adjust privacy settings
  // before the first navigation. Tests serve canned bytes through it; there is
  // no other way to exercise a navigation without a network.
  Loader& PageLoader() { return loader_; }

 private:
  void Navigate(const std::string& url);
  void Navigate(const std::string& url, const net::FetchOptions& options);
  void Navigate(const std::string& url, const net::FetchOptions& options,
                const url::Url* referrer_document);
  void NavigateFromCurrentDocument(const std::string& url, const net::FetchOptions& options);
  bool Navigate(const FormSubmission& submission);
  // Acts on the submission a script asked for, if it asked for one. Called
  // after every point where a page's script can have run -- its scripts, a
  // timer, an animation frame, a click handler -- because a navigation started
  // from inside one would tear down the interpreter running it. True when a
  // navigation started, which also means `load_` is now a different load.
  bool FollowScriptNavigation();
  void SetViewport(const gfx::IntSize& size, float device_scale);
  void ScrollBy(const ipc::ScrollMessage& scroll);
  // The band a document scroll of `delta` newly exposes, plus the boxes that
  // did not move with it. See ADR 0018 §2.
  std::vector<gfx::IntRect> ScrollDamage(gfx::IntPoint delta) const;
  // Where the viewport sits over the document. Kept on the Page rather than
  // here, because painting and a script's `getBoundingClientRect` both have to
  // subtract it and two copies of a scroll offset drift. See Page and ADR 0015.
  int ScrollY() const;
  bool HandlePointer(const ipc::PointerInputMessage& pointer);
  // Moves `:hover` and `:active` to whatever the pointer is over, and reports
  // what re-resolving the cascade would now cost. ADR 0016 §3 -- the answer is
  // `None` for every pointer event on a page whose rules do not mention either
  // state, and reaching that answer costs a bitmask test rather than a hit
  // test. Separate from applying it because a click that ends in a layout has
  // already done everything the restyle would have.
  css::StyleChangeEffect UpdatePointerState(const ipc::PointerInputMessage& pointer);
  // Does whatever that answer asks for: nothing, a repaint over the box tree
  // that is already laid out, or a full relayout. True when a frame went out.
  bool ApplyStyleChange(css::StyleChangeEffect effect);
  // Dispatches the key at whatever has focus and then, only if nothing
  // cancelled it, runs its default action. The two halves are separate on
  // purpose: ADR 0017 §2 makes the default action a step after dispatch, which
  // is the only thing that makes `preventDefault` on a keydown mean anything.
  bool HandleKey(const ipc::KeyInputMessage& key);
  // The arrow and page keys, as a keydown's default action. True when the key
  // was one of them. It is here rather than in the browser chrome because a
  // default action is something the page gets to cancel, and the chrome
  // handling the key meant a page never saw it at all -- ADR 0017 §2.
  bool ScrollByKey(const bindings::KeyInput& key);
  // What to do when a handler ran and nothing else happened: the document may
  // have moved under the layout, so drop it and paint again -- and only then,
  // because a key on a page with no handlers must not cost a relayout.
  bool HandleScriptSideEffects(bool ran);

  // Acts on one thing that arrived.
  void OnCompletion(Loader::Completion completion);
  void OnDocument(Loader::Result result);
  // How one subresource is fetched, or nothing when it must not be fetched at
  // all -- which is `integrity` on a cross-origin resource with no
  // `crossorigin`. See ADR 0020 §4.
  std::optional<net::FetchOptions> OptionsForSubresource(
      const SubresourceRequest& request) const;
  // Whether the bytes that arrived are the ones the document named. Static
  // because it is a question about the pair (element, bytes) and nothing else,
  // and shared by the stylesheet and the script paths so that "refuse to apply"
  // and "refuse to execute" cannot come to mean two different things.
  static bool IntegrityHolds(const std::vector<SubresourceRequest>& requests,
                             std::size_t index, std::string_view body);
  // One `resource` entry for a subresource that finished, however it finished.
  // A page computing a cache hit rate counts what it asked for, so a failure is
  // an entry too.
  void RecordResourceTiming(const PendingResource& resource, const Loader::Result& result);
  // Starts every subresource the parsed document referenced, all at once.
  // Concurrency is bounded per partition key inside the request queue, which is
  // where that bound belongs -- see ADR 0005 for why it is per key.
  void StartSubresources();
  // Fetches every image the page wants and has not been given. Called from the
  // initial subresource pass and again at each frame, because an
  // `<img loading="lazy">` becomes wanted when it is scrolled towards -- which
  // may be long after the navigation that carried the document is over.
  void StartImageRequests();
  // Decodes one image's bytes into the page. Shared by the load's batch and by
  // an image that arrived after it, so that sniffing, the bounds and the
  // failure counter exist once rather than twice.
  void DecodeImage(const std::string& src, const std::string& bytes);
  // One image that arrived with no navigation behind it. True when the page
  // changed and a frame should go out.
  bool OnLateImage(Loader::Completion completion);

  // Fetches whatever the module graph is missing, and settles the dynamic imports
  // whose graph has closed. True when a promise settled, which means a page's
  // `then` ran and the document may have changed.
  //
  // A module fetch is like a late image and unlike a subresource: it happens
  // *after* the navigation that carried the document, because `import()` is
  // reached whenever the page reaches it. So it follows the same two rules -- a
  // navigation clears it, and one in flight keeps the loop turning.
  bool AdvanceModules();
  // One module's source. True when the completion was one.
  bool OnModuleFetch(Loader::Completion completion);

  // bindings::HistorySource. ADR 0026 §1-2, implemented in EngineHistory.cpp.
  // Private for the reason NetworkSource is, and the interesting one is
  // PushHistoryState: `src/bindings` may not see `url`, so the same-origin check
  // -- the only thing between a page and a perfect address-bar spoof -- is on
  // this side of the seam, and the binding turns the refusal into a
  // `SecurityError`.
  std::size_t HistoryLength() const override;
  const js::SerializedValue& HistoryState() const override;
  std::uint64_t HistoryStateGeneration() const override;
  UrlOutcome PushHistoryState(const js::SerializedValue& state, std::string_view url,
                              bool replace) override;
  void RequestHistoryTraversal(int delta) override;

  // Moves by `delta`: a load when the target entry belongs to another document,
  // and a paint plus `popstate` when it belongs to this one. True when anything
  // moved.
  bool Traverse(int delta);
  // The traversal a script asked for, taken at the turn boundary. True when one
  // happened, which is the caller's signal that everything below it belongs to a
  // document that may be gone.
  bool FollowPendingTraversal();
  // A navigation that differs from the current URL only in its fragment: a new
  // entry, the fragment applied, `hashchange`, and no request. True when `url`
  // was one, which is the caller's signal not to load it.
  bool NavigateToFragment(const std::string& url);
  // Tells the chrome what its two buttons should look like, and nothing else.
  void SendHistoryState();

  // bindings::NetworkSource. Private inheritance for the reason Page's
  // GeometrySource is private: the binding layer holds a reference to the
  // interface and nothing else has business calling these.
  //
  // This is where a page's own request becomes a real one -- resolved against
  // the document, put through `privacy::Verdict` like everything else, and
  // handed the CORS parameters that decide what comes back. The engine is the
  // implementation rather than the Page because a fetch needs the loader, and
  // the loader is here.
  std::uint64_t StartFetch(const bindings::ScriptRequest& request) override;
  void AbortFetch(std::uint64_t id) override;
  // One response for a request a script made. True when the page's script ran,
  // which is the caller's signal that the document may have changed under it.
  bool OnScriptFetch(Loader::Completion completion);
  // Runs the scripts once every render-blocking resource has resolved, puts
  // the page on screen, and lets the navigation go once even the scripts the
  // page said it would not wait for have landed.
  void AdvanceLoad();
  // Decodes the images and sends the first frame of this document.
  void Paint();
  void DecodePendingImages();

  // Lays out at the current viewport width, then paints. Separate from
  // PaintAndSend because scrolling repaints without relaying out, and a
  // scroll that ran layout would be the classic reason scrolling is slow.
  void LayoutAndPaint();

  // Rebuild the display list from current state and send it with full-viewport
  // damage. Incremental damage arrives with the paint system in M6; reporting
  // the truth (everything changed) is the correct placeholder, and is why the
  // damage field is not simply omitted.
  void PaintAndSend();
  // The same, for the two frames whose damage is known rather than derived. A
  // `scroll_delta` says this frame is the previous one moved, which the UI may
  // blit; `only` is the single rectangle a box that scrolled inside the page
  // changed. Both exist because the display-list diff answers "everything" for
  // a scroll -- every command in the list moved -- and repainting the window
  // for a wheel notch is the cost ADR 0018 is written to avoid.
  void PaintAndSend(gfx::IntPoint scroll_delta, const gfx::IntRect* only);

  // Renders `message` as the page, for a load that failed. A blank window is
  // indistinguishable from a hung browser.
  void ShowError(std::string_view url, std::string_view message);

  // Clamped so that scrolling stops at the end of the document rather than
  // running off into blank space.
  int MaxScroll() const;

  ipc::EngineEndpoint& endpoint_;
  Loader loader_;
  Page page_;
  // The frame most recently sent, kept so the next one can be diffed against
  // it. This is what the display list being a comparable value buys: damage is
  // computed from two frames rather than trusted from every call site that
  // invalidated something.
  gfx::DisplayList display_list_;
  // Reused rather than reallocated per frame; painting is the hot path.
  gfx::DisplayList pending_;
  gfx::IntSize viewport_size_;
  float device_scale_ = 1.0f;
  PendingLoad load_;
  // Images requested after the navigation that carried the document finished:
  // an `<img loading="lazy">` the user scrolled towards. They cannot live in
  // `load_`, which is cleared the moment a navigation is over and exists to
  // make a response for a document that is gone undeliverable.
  //
  // This is the first resource this browser fetches outside a navigation, and
  // it is the seam `fetch` and `XMLHttpRequest` arrive on -- so the two rules
  // it establishes are worth stating here. A navigation clears it, for the
  // reason it clears `load_`. And a request in it keeps the loop turning:
  // HasRunnableWork says so, or a canned transport would hand the answer to
  // nobody.
  std::map<Loader::RequestId, std::string> late_images_;
  // The requests this page's own script made and has not been answered for.
  //
  // A set rather than a map: everything about a `fetch` -- the promise waiting
  // on it, the signal that may cancel it -- lives in the JavaScript heap where
  // the collector can see it, and all the engine needs is to recognise the id
  // when the answer arrives. Same two rules as `late_images_`: a navigation
  // clears it, and something in it keeps the loop turning.
  std::set<Loader::RequestId> script_fetches_;
  // The modules in flight, and which URL each is. A map rather than a set because
  // the graph is keyed by URL and the completion only carries an id.
  std::map<Loader::RequestId, std::string> module_fetches_;
  // Back and forward, for this tab. ADR 0026 §1: it is here rather than in
  // `src/ui` because a `pushState` entry is a URL *plus a state object owned by a
  // document*, and the chrome cannot see a document.
  SessionHistory history_;
  // Which document is current. Incremented per committed load, and compared
  // rather than a URL: two loads of the same URL are two documents, and two
  // `pushState` entries on one document are not.
  std::uint64_t document_id_ = 0;
  // A traversal a script asked for and has not had yet. Taken at the turn
  // boundary for the reason a form submission is: a traversal can replace the
  // document, and doing that with the interpreter on the stack is a
  // use-after-free.
  int pending_traversal_ = 0;
  // Whether the load in flight is a traversal rather than a new navigation. A
  // traversal's entry is already in the list at its own index, so committing one
  // must not push.
  bool traversing_ = false;
};

}  // namespace microbrowser::engine

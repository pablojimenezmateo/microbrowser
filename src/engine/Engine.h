#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bindings/History.h"
#include "bindings/Cookies.h"
#include "bindings/IndexedDb.h"
#include "bindings/Network.h"
#include "bindings/Sockets.h"
#include "bindings/Storage.h"
#include "engine/Loader.h"
#include "net/EventSourceConnection.h"
#include "net/WebSocketConnection.h"
#include "engine/Page.h"
#include "engine/PendingLoad.h"
#include "engine/SessionHistory.h"
#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"
#include "ipc/Message.h"
#include "ipc/Transport.h"
#include "storage/PartitionedIndexedDb.h"
#include "storage/PartitionedStorage.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::engine {

// The engine half of the seam.
class Engine : private bindings::NetworkSource,
               private bindings::HistorySource,
               private bindings::StorageSource,
               private bindings::SocketSource,
               private bindings::CookieSource,
               private bindings::IndexedDbSource {
  friend const std::vector<std::string>& CspViolations(const Engine&);
  friend void SettleForSnapshot(Engine& engine);

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
  //
  // `IsDocumentLoading` is the navigation half alone. A page `fetch()` /
  // XHR and outstanding `@font-face` downloads stay in `IsLoading` so tests
  // and `RunEngineToIdle` wait for them, but youtube's long-lived innertube
  // requests and stuck gstatic font fetches would otherwise hold the
  // snapshot's load loop for the full settle deadline (session 13 note;
  // TD-0031). The tool asks `IsDocumentLoading`; the loop still watches the
  // sockets through `AppendWaitDescriptors`.
  bool IsDocumentLoading() const {
    // A child document a script asked for is on this list for the same reason a late image is: it
    // is part of what the document *is*, and a loop that stopped turning would leave an `<iframe>`
    // holding an empty box and a `load` handler that never ran. HTML agrees -- a frame blocks the
    // embedder's `load` event, which is the whole difference between it and a `fetch`.
    return load_.active || !post_load_.images.empty() || !post_load_.scripts.empty() ||
           !post_load_.frames.empty() || !module_fetches_.empty() || page_.HasPendingModules() ||
           page_.ScriptHalf()->HasOutstandingScriptFetches();
  }
  bool IsLoading() const {
    return IsDocumentLoading() || !script_fetches_.empty() || !font_fetches_.empty();
  }
  // Page `fetch` / XHR in flight (not navigation, not fonts). Snapshot waits on
  // these after a trusted click so Accept's save→reload can finish without
  // re-entering the unbounded font/innertube hang TD-0031 removed from the
  // load loop (TD-0032).
  bool HasInFlightScriptFetches() const { return !script_fetches_.empty(); }
  // Why `IsLoading` is still true — for `MICROBROWSER_LOAD_TURN_TRACE` and
  // snapshot hang diagnosis. Empty when nothing is outstanding.
  std::string LoadingReason() const;

  // What the page's script threw, so a host that is debugging one can say why
  // a document rendered the way it did. Forwarded rather than exposing the
  // Page, which would put the whole engine on the wrong side of the seam.
  const std::vector<std::string>& ScriptErrors() const { return page_.ScriptErrors(); }
  // A diagnostic probe against the loaded page's own interpreter, for a host
  // that has no console -- `microbrowser_snapshot -eval` is the one caller. See
  // PageScript::Evaluate.
  std::string EvaluateScript(std::string_view source);
  // Script that runs once, after the document exists and before the page's own
  // scripts. Needed to hook APIs before youtube's player can poison itself
  // (TD-0020); `-eval` is too late. Cleared after the first RunScripts turn.
  void SetScriptPrelude(std::string source) { script_prelude_ = std::move(source); }
  // The other half of the same question, and forwarded for the same reason. A
  // page that ran correctly and said something is not distinguishable from one
  // that threw, from outside, without both.
  const std::vector<std::string>& ConsoleOutput() const { return page_.ConsoleOutput(); }

  // Surfaces backing `<video>` DrawSurface holes. The presenter composites them after Execute;
  // snapshot does the same so a watch page's first frame is in the PPM.
  const gfx::SurfaceRegistry& VideoSurfaces() const { return page_.VideoSurfaces(); }

  // Borrowed device from `src/app`. Null (tests, snapshot) keeps playback silent.
  // Must outlive every Start; clear before the sink is destroyed (ADR 0028 §4).
  void SetAudioSink(media::AudioSink* sink) { page_.SetAudioSink(sink); }

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
  void DrainReadyLoaderCompletions();
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
  // Fetches `<script>` elements a page injected after the parse-time walk.
  void StartPendingScriptRequests();
  // Collects, fetches, and runs scripts a page added after `RunScripts`.
  bool ProcessDynamicScripts();
  // Fetches every image the page wants and has not been given. Called from the
  // initial subresource pass and again at each frame, because an
  // `<img loading="lazy">` becomes wanted when it is scrolled towards -- which
  // may be long after the navigation that carried the document is over.
  void StartImageRequests();
  // Fetches every `@font-face` source the page wants and has not been given.
  // Called from the subresource pass and again whenever a stylesheet lands, since
  // a face is declared *in* a sheet and the sheet arrives after the document.
  void StartFontRequests();
  // Fetches the document for every child browsing context that has none yet.
  // ADR 0027 §1. Called from the subresource pass, and again whenever the
  // frames are re-collected -- a script that appends an `<iframe>` creates a
  // context the same way the parser does.
  void StartFrameRequests();
  // Re-collects the child contexts, fetches the new ones, and fires the `load` events owed to
  // those whose documents arrived. True when a handler ran, so the document may have moved.
  bool ProcessDynamicFrames();
  // Gives every child context that has a document somewhere to run script, and runs it. ADR 0042
  // §5, and the recursion is the point: a frame inside a frame is a realm of the same interpreter,
  // because same-origin is transitive and so is the heap they share.
  //
  // Here rather than in `Page` because deciding *whose* interpreter a child borrows is a
  // same-origin question, and `Page` may not see `src/url` -- the same inversion that put
  // `Frame::same_origin` on this side of the seam (ADR 0027 §2). True when any script ran, which
  // is the caller's signal that some document in the tree may have moved.
  //
  // `top_window` is the root of the whole tree, null at the outermost call. Threaded rather than
  // re-derived, because `top` is the *first* window in the chain and every level below the second
  // would otherwise answer with its own parent.
  //
  // **`run_scripts` splits the two halves, and they happen at different moments.** A child's
  // *window* has to exist as soon as its element is in the document, because the embedder's own
  // first script reads `iframe.contentWindow` -- but its *scripts* must not have run by then, or a
  // child would see the embedder's globals before the embedder's own script had set them. Browsers
  // get this for free because a frame's document arrives asynchronously; here it is a parameter,
  // false on the pass before the page's scripts and true on the passes after.
  bool RunFrameScripts(Page& parent, bool run_scripts, js::Object* top_window = nullptr);
  // Gives every `<iframe>` in the tree a context *now*, without dispatching the `load` events they
  // are owed. Called back from `iframe.contentWindow` when the element has none, because HTML
  // creates a nested context on insertion and a page reads the window in the same script turn.
  // See bindings::FrameGlobals::SetSettleHook, which is where the `load` half is explained.
  void SettleFrameContexts();
  // Every child context's timers, animation frames and queued activations. Called from the same
  // places the top-level page's are, because a frame that runs script and never has its queues
  // drained is worse than one that runs none: what it asked for is recorded and never happens.
  bool RunFrameDueWork(Page& parent, std::int64_t now_ms);
  // The above, plus what a handler having run implies: the navigation it asked for, or a relayout
  // and a paint. `navigated` says the document is gone and the caller must touch nothing else.
  bool SettleFrameLoads(bool& navigated);
  // Every completion the loader has ready, routed to whichever table its id is in. Sets `moved`
  // when anything happened; true when the caller must return at once, because a handler navigated
  // and the rest of the batch belongs to a document that is gone.
  bool DrainCompletionBatch(bool& moved);
  // One child document's bytes, into the frame that asked for them. Decides
  // *here* whether the child is same-origin, because this is the module that
  // understands URLs -- see Page::SetFrameDocument and ADR 0027 §2.
  bool OnFrameFetch(Loader::Completion completion, const PendingResource& resource);
  // A child document that arrived after the navigation was over -- most of them, since a script
  // appending an `<iframe>` is the common case. False when the completion is not one. Does not
  // fire `load`; see the body.
  bool OnLateFrame(Loader::Completion& completion);
  void StartWorkerScriptRequests();
  bool OnWorkerScriptFetch(Loader::Completion completion);
  // One face's bytes. True when the provider took them and the page therefore
  // needs laying out again -- text measured before a face arrived was measured in
  // a different font, which is what `font-display: swap` looks like from inside.
  bool OnFontFetch(Loader::Completion completion);
  // Decodes one image's bytes into the page. Shared by the load's batch and by
  // an image that arrived after it, so that sniffing, the bounds and the
  // failure counter exist once rather than twice.
  void DecodeImage(const std::string& src, const std::string& bytes);
  // One image that arrived with no navigation behind it. True when the page
  // changed and a frame should go out.
  bool OnLateImage(Loader::Completion completion);
  bool OnLateScript(Loader::Completion completion);

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
  void RequestNavigation(std::string_view url, bool replace) override;

  // Moves by `delta`: a load when the target entry belongs to another document,
  // and a paint plus `popstate` when it belongs to this one. True when anything
  // moved.
  bool Traverse(int delta);
  // The traversal a script asked for, taken at the turn boundary. True when one
  // happened, which is the caller's signal that everything below it belongs to a
  // document that may be gone.
  bool FollowPendingTraversal();
  // `location.assign` / `replace` / `href=`, taken at the same turn boundary.
  bool FollowPendingLocationNavigation();
  // A navigation that differs from the current URL only in its fragment: a new
  // entry, the fragment applied, `hashchange`, and no request. True when `url`
  // was one, which is the caller's signal not to load it.
  bool NavigateToFragment(const std::string& url);
  // Tells the chrome what its two buttons should look like, and nothing else.
  void SendHistoryState();

  // bindings::SocketSource. ADR 0020 §5, in EngineSockets.cpp, and private for the
  // reason the others are. Everything a policy decides -- the scheme, `connect-src`, the
  // privacy verdict -- is on this side, because `src/bindings` may see none of `net`,
  // `csp` or `url`.
  std::uint64_t OpenSocket(std::string_view url) override;
  bool SendSocket(std::uint64_t id, std::string_view data, bool text) override;
  void CloseSocket(std::uint64_t id, std::uint16_t code, std::string_view reason) override;
  std::uint64_t SocketBufferedAmount(std::uint64_t id) override;
  std::uint64_t OpenEventSource(std::string_view url) override;
  void CloseEventSource(std::uint64_t id) override;

  // The loop's three questions about a long-lived connection. A socket with nothing
  // queued is *not* runnable work -- it is a descriptor in the wait -- which is the
  // whole of how an open connection costs nothing while idle.
  void AppendSocketDescriptors(util::WaitDescriptorList& out) const;
  bool SocketsHaveWork() const;
  // Everything readable, and the events that follow. True when script ran, which is the
  // caller's signal that the document may have changed under it.
  bool AdvanceSockets();
  // The streams, and the one deadline this file needs: a stream *waiting* to reconnect.
  // An open one contributes nothing, so an idle page with a stream still blocks.
  bool AdvanceEventSources();
  std::optional<std::uint32_t> NextEventSourceDeadlineMs(std::int64_t now_ms) const;
  // A navigation. Erasing is closing, because the connection's destructor closes its
  // transport.
  void CloseAllSockets();

  // bindings::StorageSource. ADR 0021, in EngineStorage.cpp, and private for the
  // reason the other two are.
  //
  // **This is where the partition key is derived, and it is the only place.**
  // `src/bindings` may not see `url` or `storage`, so a binding cannot name a
  // partition even by accident: it picks Session or Local and this side decides whose
  // data that is, from the document's own URL. ADR 0021 §1 requires the key on every
  // store; giving the caller no way to spell one is how that is enforced against a
  // caller rather than merely checked.
  bool Available(bindings::StorageSource::Kind kind) override;
  std::size_t Length(bindings::StorageSource::Kind kind) override;
  std::optional<std::string> KeyAt(bindings::StorageSource::Kind kind,
                                   std::size_t index) override;
  std::optional<std::string> GetItem(bindings::StorageSource::Kind kind,
                                     std::string_view key) override;
  WriteResult SetItem(bindings::StorageSource::Kind kind, std::string_view key,
                      std::string_view value) override;
  bool RemoveItem(bindings::StorageSource::Kind kind, std::string_view key) override;
  bool Clear(bindings::StorageSource::Kind kind) override;

  // bindings::CookieSource. ADR 0005, in EngineCookies.cpp, and private for the
  // reason the other seams are: the binding layer holds a reference to the
  // interface and nothing else has business calling these.
  //
  // **This is where the partition key is derived for script-visible cookies,**
  // and it is the only place. `src/bindings` may not see `url` or `net`.
  std::string DocumentCookie() override;
  bool SetDocumentCookie(std::string_view assignment) override;

  // The area this document's script reads and writes. Null when the document has no
  // URL a partition key can be built from -- `about:blank`, a document built by a
  // test -- and then every storage operation above answers as if the store were empty
  // rather than crashing. An opaque origin genuinely has no keyed storage.
  storage::StorageArea* AreaFor(bindings::StorageSource::Kind kind);

  // bindings::IndexedDbSource. ADR 0038, in EngineIndexedDb.cpp, and private for the
  // reason StorageSource is: **this is where the partition key is derived**, the same
  // way `AreaFor` derives one for `sessionStorage`, and `src/bindings` cannot see
  // `url::PartitionKey` at all.
  bool Available() override;
  OpenResult OpenDatabase(const std::string& name, std::uint64_t version) override;
  void DeleteDatabase(const std::string& name) override;
  bool CreateObjectStore(const std::string& db, const std::string& store,
                         const bindings::IndexedDbKeyPath& key_path) override;
  bool CreateIndex(const std::string& db, const std::string& store, const std::string& index,
                   bool unique) override;
  std::vector<std::string> ObjectStoreNames(const std::string& db) override;
  std::vector<std::string> IndexNames(const std::string& db, const std::string& store) override;
  bindings::IndexedDbKeyPath ObjectStoreKeyPath(const std::string& db,
                                                const std::string& store) override;
  PutResult Put(const std::string& db, const std::string& store,
               const bindings::IndexedDbKeyValue& key, std::vector<std::uint8_t> value,
               std::vector<IndexKeyEntry> index_keys) override;
  std::optional<std::vector<std::uint8_t>> Get(const std::string& db, const std::string& store,
                                               const bindings::IndexedDbKeyValue& key) override;
  bool Delete(const std::string& db, const std::string& store,
             const bindings::IndexedDbKeyValue& key) override;
  std::vector<CursorEntry> Query(const std::string& db, const std::string& store,
                                 const std::string& index,
                                 const std::optional<bindings::IndexedDbKeyValue>& only_key) override;
  // The partition's databases, or null when the document has no URL a partition key
  // can be built from -- the same `about:blank` / opaque-origin case `AreaFor` refuses.
  storage::PartitionedIndexedDb::Databases* DatabasesFor();

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
  // `new URL(...)`'s resolve, through the one parser in `src/url`. See bindings/Network.h.
  std::string ResolveUrl(std::string_view relative, std::string_view base) const override;
  std::string ResolveDocumentUrl(std::string_view relative) const override;
  std::string RegisterBlobUrl(std::string body, std::string mime_type) override;
  void RevokeBlobUrl(const std::string& url) override;
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
  // ADR 0021 §2's two lifetimes, as two objects rather than one flag: a session store
  // dies with the tab and a local store with the browser session, and today this
  // object is both -- so what makes them different is *only* that a navigation within
  // the tab keeps session storage and, when tabs exist, a second tab will get its own.
  // Writing them as two fields now is what makes that a one-line change rather than an
  // audit of every write.
  //
  // Neither reaches a disk. ADR 0021 §2 makes persistence a per-site user act that
  // lands together with encryption at rest, and a sign-in token in a plaintext file is
  // the worst outcome available here.
  // The page's WebSockets, by id. The first thing this engine owns whose lifetime is a
  // *document* rather than a request: a navigation clears it (ADR 0020 §5), and until
  // then a connection sits in the idle wait costing nothing.
  std::map<std::uint64_t, std::unique_ptr<net::WebSocketConnection>> sockets_;
  std::uint64_t next_socket_id_ = 0;
  // The page's event streams, in the same table shape and with the same lifetime. Separate
  // from `sockets_` because the *reconnect* is theirs alone: it is the one request in this
  // browser the user did not cause, so it has a deadline in the loop that a socket has not.
  std::map<std::uint64_t, std::unique_ptr<net::EventSourceConnection>> event_sources_;
  storage::PartitionedStorage session_storage_;
  storage::PartitionedStorage local_storage_;
  // ADR 0038. In memory only, like the two above -- and, like them, cleared by
  // nothing this engine does today, because there is no second document yet to
  // navigate away and free the first one's.
  storage::PartitionedIndexedDb indexed_db_;
  Page page_;
  // Once, before the document's own scripts. See SetScriptPrelude.
  std::string script_prelude_;
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
  // Subresources and script phase that outlive `load_`. A navigation clears it.
  struct PostLoad {
    bool document_interactive = false;
    std::map<Loader::RequestId, std::string> images;
    std::map<Loader::RequestId, std::size_t> scripts;
    // Child documents a script asked for after the load was over -- which is
    // most of them, because `iframe.onload = f; document.body.appendChild(f)`
    // is how a page (and almost every WPT test) makes a second document. The
    // frame index rather than a URL, for the same reason `scripts` holds one:
    // the completion carries an id and the tree is addressed by position.
    std::map<Loader::RequestId, std::size_t> frames;
    void Clear() {
      document_interactive = false;
      images.clear();
      scripts.clear();
      frames.clear();
    }
  } post_load_;
  // The requests this page's own script made and has not been answered for.
  //
  // A set rather than a map: everything about a `fetch` -- the promise waiting
  // on it, the signal that may cancel it -- lives in the JavaScript heap where
  // the collector can see it, and all the engine needs is to recognise the id
  // when the answer arrives. Same two rules as `post_load_`: a navigation
  // clears it, and something in it keeps the loop turning.
  std::set<Loader::RequestId> script_fetches_;
  // The modules in flight, and which URL each is. A map rather than a set because
  // the graph is keyed by URL and the completion only carries an id.
  std::map<Loader::RequestId, std::string> module_fetches_;
  // The faces in flight. A map because the completion carries an id and
  // registering needs the family, weight and slant the descriptors declared -- the
  // file cannot be asked, since what a `font-family` stack names is the descriptor.
  std::map<Loader::RequestId, Page::PendingFontFace> font_fetches_;
  // Worker scripts in flight, by request. ADR 0022 §1: a worker's script is fetched like any other
  // subresource, and the worker's thread starts when it arrives -- so a page can construct a worker and
  // post to it before either has happened.
  std::map<Loader::RequestId, std::uint64_t> worker_fetches_;
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
  // A `location.assign` / `replace` / `reload` / `href=` that script asked for.
  // First one in a turn wins, for the same reason as PendingSubmit: the first
  // navigation is what tears the document down.
  struct PendingLocationNavigation {
    std::string url;
    bool replace = false;
  };
  std::optional<PendingLocationNavigation> pending_location_;
  // `location.replace` rewrites the current history entry on commit rather than
  // pushing — same OnDocument shape as a traversal restore (`traversing_`).
  bool replacing_document_ = false;
  // Whether the load in flight is a traversal rather than a new navigation. A
  // traversal's entry is already in the list at its own index, so committing one
  // must not push.
  bool traversing_ = false;
};

}  // namespace microbrowser::engine

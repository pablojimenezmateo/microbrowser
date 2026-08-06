#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "bindings/AnimationFrames.h"
#include "bindings/IdleCallbacks.h"
#include "bindings/Canvas.h"
#include "bindings/Workers.h"
#include "bindings/DomBindings.h"
#include "bindings/Geometry.h"
#include "bindings/History.h"
#include "bindings/Performance.h"
#include "bindings/Network.h"
#include "bindings/Timers.h"
#include "dom/Node.h"
#include "engine/DocumentPolicy.h"
#include "engine/ModuleLoader.h"
#include "engine/Subresource.h"
#include "js/Interpreter.h"

namespace microbrowser::engine {

// The script half of a page: one interpreter, one binding layer, one document.
//
// Its own type rather than two more members on Page, because Page was already
// the class most at risk of becoming "the browser" and the fan-out lint said
// so the moment script arrived. Page coordinates; this owns the coordination
// of one thing.
//
// Rebuilt per load. A fresh global scope per document is the same rule the
// fresh style resolver follows and for a stronger reason: leaving the previous
// page's globals in place would let one document's script see another's state,
// which is a same-origin violation rather than a stale stylesheet.
class PageScript {
 public:
  // When a script runs, relative to the rest of the load.
  //
  // ADR 0011 decided these are three points in a document's lifecycle rather
  // than three attributes to ignore, and this enum is where that decision
  // lives. What is *not* implemented is running a blocking script during the
  // parse: everything here still runs after it, which is the deviation
  // recorded on `Run` below.
  enum class Timing : std::uint8_t {
    // Classic, no `defer` and no `async`. Document order, first.
    Blocking,
    // `defer`, and a module -- which is deferred by definition. Document order,
    // after every blocking script.
    Deferred,
    // `async`. Whenever it arrives, and its order is nobody's promise. It does
    // not hold the page up: a page whose analytics tag is slow paints without
    // waiting for it, which is the entire reason the attribute exists.
    Async,
  };

  // Where the binding layer's geometry questions go. Set once, by the Page
  // that owns this, before any script runs. Borrowed, not owned -- and a
  // pointer rather than a reference because it is set after construction: this
  // is a member of Page, so Page cannot hand itself over in an initializer
  // list before its own bases exist.
  void SetGeometrySource(bindings::GeometrySource* geometry) { geometry_ = geometry; }

  // The same, for the requests a page makes itself. Borrowed and set before
  // any script runs, for the reason the geometry source is: a source that
  // arrived later would leave the first script of a document without one, and
  // `fetch` is declared or not declared at construction.
  void SetNetworkSource(bindings::NetworkSource* network) { network_ = network; }
  // The same, for `window.history`.
  void SetHistorySource(bindings::HistorySource* history) { history_ = history; }
  // And for `sessionStorage`/`localStorage`. Null leaves both names undeclared, which
  // is ADR 0012's rule and ADR 0021 §6's answer for a document with no keyed storage.
  void SetStorageSource(bindings::StorageSource* storage) { storage_ = storage; }
  // And for `document.cookie`. Null leaves the accessor answering an empty string,
  // which is what a test with a bare document needs.
  void SetCookieSource(bindings::CookieSource* cookies) { cookies_ = cookies; }
  // And for `WebSocket`. Null leaves the name undeclared, which is what a page with no
  // socket source must see rather than a constructor that never opens.
  void SetSocketSource(bindings::SocketSource* sockets) { sockets_ = sockets; }
  // ADR 0028 §1. Null leaves `<video>` with no media API, which is what a page with nothing
  // behind it must see rather than a `play()` whose promise never settles.
  void SetMediaController(bindings::MediaController* media) { media_ = media; }
  // The canvas commands (ADR 0029 §2). Same lifetime as the others: a surface handed over after the
  // first script would leave a page whose whole rendering is a canvas with a blank one.
  void SetCanvasSurface(bindings::CanvasSurface* canvas) { canvas_ = canvas; }
  void SetWorkerHost(bindings::WorkerHost* workers) { workers_ = workers; }
  void SetTrustedInsertionFlush(std::function<void()> hook);
  // A worker's message or error, forwarded to the bindings. False without an interpreter, which is the
  // case on the turn a navigation replaced the document a worker was posting to.
  bool DeliverWorkerMessage(std::uint64_t id, const std::string& serialized,
                            const std::string& error, bool is_error) {
    return bindings_ != nullptr &&
           bindings_->DeliverWorkerMessage(id, serialized, error, is_error);
  }

  // A socket's events, forwarded to the bindings when there are any. False without an
  // interpreter: a socket cannot outlive its document, but a completion can arrive on the
  // same turn a navigation replaced it.
  bool DeliverSocketOpen(std::uint64_t id) {
    return bindings_ != nullptr && bindings_->DeliverSocketOpen(id);
  }
  bool DeliverSocketMessage(std::uint64_t id, const std::string& data, bool text) {
    return bindings_ != nullptr && bindings_->DeliverSocketMessage(id, data, text);
  }
  bool DeliverSocketClose(std::uint64_t id, std::uint16_t code, const std::string& reason,
                          bool clean, bool failed) {
    return bindings_ != nullptr &&
           bindings_->DeliverSocketClose(id, code, reason, clean, failed);
  }
  // A media event at an element. ADR 0028 §1's event set is what a page listens to, and this is
  // a C++ entry point for the reason click dispatch is: an event the *browser* produced is
  // trusted, and a page that could fire `canplay` at its own element could make a player believe
  // data arrived.
  bool DispatchMediaEvent(dom::Element& element, const std::string& type) {
    return bindings_ != nullptr && bindings_->DispatchMediaEvent(element, type);
  }
  bool DeliverEventSourceOpen(std::uint64_t id) {
    return bindings_ != nullptr && bindings_->DeliverEventSourceOpen(id);
  }
  bool DeliverEventSourceMessage(std::uint64_t id, const std::string& type,
                                 const std::string& data, const std::string& last_id) {
    return bindings_ != nullptr &&
           bindings_->DeliverEventSourceMessage(id, type, data, last_id);
  }
  bool DeliverEventSourceError(std::uint64_t id, bool permanent) {
    return bindings_ != nullptr && bindings_->DeliverEventSourceError(id, permanent);
  }
  // Fires `popstate`, or `hashchange`, at the window. False before this page has
  // an interpreter, which is a traversal on a document that never ran a script.
  // Moves the address the binding layer answers with. Nothing before this page
  // has an interpreter, which is a `pushState` on a document with no script --
  // impossible, since `pushState` comes from script.
  void SetDocumentUrl(const std::string& url);
  bool NotifyPopState();
  bool NotifyHashChange(const std::string& old_url, const std::string& new_url);

  // Settles the promise `fetch` handed out for `id`. False when nothing was
  // waiting -- the request was aborted, or this is a second delivery -- and
  // false too before this page has an interpreter, which is a response for a
  // document that never ran a script.
  bool DeliverFetchResponse(std::uint64_t id, const bindings::ScriptResponse& response);

  // Lets go of the document this was bound to, which is about to be replaced.
  //
  // A fresh global scope per document is the rule this class exists to keep:
  // leaving the previous page's globals in place would let one document's
  // script see another's, which is a same-origin violation rather than a stale
  // cache. Keeping the *binding layer* would be worse than that -- it holds a
  // reference to the document, so the next page's first tree read would be a
  // use-after-free.
  //
  // Called before the document is replaced rather than after, because by then
  // the reference this drops is already dangling.
  void Detach();

  // Finds the document's scripts and records them in document order, inline
  // text filled in and external ones left as a URL for the caller to fetch.
  //
  // Two steps rather than one because ordering is the whole problem: a page's
  // scripts must run in the order they appear whether each is inline or
  // external, so nothing can run until every external one has arrived. The
  // same shape the stylesheets already use.
  // `policy` is the document's Content-Security-Policy. A script it refuses is
  // never recorded, which is where "enforced rather than logged" has to happen:
  // a slot that existed and was skipped at run time would still have been
  // fetched, and a fetch the policy forbids is the request enforcement is for.
  void Collect(dom::Document& document, const DocumentPolicy& policy);
  // Scripts a page injected after the initial walk. Polyfill loaders append
  // `<script type=module src=…>` tags; reddit's concat bundle is among them.
  bool CollectInserted(dom::Document& document, const DocumentPolicy& policy);
  // External scripts added since the last fetch batch started.
  std::vector<SubresourceRequest> TakeUnrequestedScripts();
  void MarkScriptsRequested() { scripts_requested_ = pending_urls_.size(); }
  bool HasOutstandingScriptFetches() const { return scripts_requested_ < pending_urls_.size(); }
  // Runs any script whose source is ready after `CollectInserted`, even when
  // the first pass of `Run` already happened.
  bool RunPendingScripts();
  // The external scripts, in the order they were found. The caller fetches
  // them, because what a URL turns into is the loader's problem -- and because
  // a fetch needs a privacy verdict, which this layer has no business
  // producing.
  const std::vector<SubresourceRequest>& PendingUrls() const { return pending_urls_; }
  // Whether `PendingUrls()[index]` is one the page said it would not wait for.
  // The engine asks so it knows which outstanding scripts hold the first paint
  // and which do not.
  bool IsAsync(std::size_t index) const;
  // Supplies the source for `PendingUrls()[index]`.
  void AddFetched(std::size_t index, std::string source);

  // Runs the document's scripts: every blocking one in document order, then
  // every deferred one, then whatever `async` scripts have arrived so far.
  //
  // After parsing rather than during it, which is a real difference from the
  // specification: a script that runs while the parser is still working sees a
  // half-built tree, and `document.write` depends on exactly that. Nothing
  // here has `document.write` -- ADR 0011 decided it stays unimplemented,
  // because supporting it properly means re-entering the tokenizer mid-parse
  // for a feature the web is actively removing -- and running after is the
  // version that is easy to be sure about.
  //
  // Idempotent: calling it twice runs nothing the second time, so a caller
  // that fetches subresources and a caller that does not can both end with it.
  //
  // `now_ms` is the epoch a timer's delay is measured from. Passed in for the
  // reason the loader takes a time: two decisions inside one turn must not
  // disagree about what time it is.
  void Run(dom::Document& document, const std::string& url, std::int64_t now_ms);

  // Runs any `async` script whose source arrived after `Run`. True when one
  // did, which is the caller's signal that the document may have changed.
  // Nothing before `Run`: an async script is still a script on this document,
  // and running one before the page had an interpreter would be a second way
  // to build one.
  bool RunReadyAsync();

  // Milliseconds until the soonest thing this page has asked to be woken for:
  // a timer, or an animation frame. Nothing when it has asked for neither --
  // which is the answer that lets the loop block rather than wake, and the
  // reason both live behind one question instead of the loop having to
  // remember to ask twice.
  std::optional<std::uint32_t> NextWakeDelay(std::int64_t now_ms) const;
  // Runs every timer that is due and, if the frame boundary has arrived, the
  // animation frame. True when any ran, which is the caller's signal that the
  // document may have changed.
  bool RunDueWork(std::int64_t now_ms);

  // Runs the click handlers registered on `target` and its ancestors. True
  // when one called `preventDefault`.
  bool DispatchClick(dom::Element& target, const bindings::PointerInput& pointer);
  // Fires `keydown` or `keyup` at `target`, or at the document when it is null.
  // True when a handler called `preventDefault`, which is the caller's signal
  // not to run the key's default action -- ADR 0017 §2 makes that action a step
  // after dispatch rather than something dispatch performs.
  bool DispatchKey(dom::Node* target, const bindings::KeyInput& key);
  // Moves focus to `target`, or clears it when null, firing `blur`/`focusout`
  // and `focus`/`focusin`. True when it moved, which is the caller's signal
  // that handlers ran and the document may have changed.
  //
  // Through the binding layer even though the state lives on the document,
  // because the events are half the algorithm and the binding layer is the only
  // module allowed to run a page's handlers. A page with no script still moves
  // focus -- see Page::MoveFocus, which writes the document directly when there
  // is no interpreter to run handlers in.
  bool MoveFocus(dom::Element* target, bool visible);
  // Fires `submit` at `form`. True when a handler called `preventDefault`,
  // which is the caller's signal not to submit.
  bool DispatchSubmit(dom::Element& form);
  // Fires `scroll` at `target`, or at the document when it is null. True when
  // something was listening. Not cancelable and dispatched after the fact: a
  // scroll has already happened by the time a page hears about it, which is why
  // there is no `preventDefault` to report. See ADR 0018 §3.
  bool DispatchScroll(dom::Element* target);
  // Samples the page's `IntersectionObserver`s and `ResizeObserver`s against
  // the layout that is about to be painted, and runs the callbacks whose
  // answers changed. True when one ran, which is the caller's signal that the
  // document may have moved under it. `now_ms` is the same steady clock a timer
  // and a frame take; the record's `time` is measured from the page's origin,
  // which is why this converts rather than passing it through. ADR 0018 §5.
  bool DeliverViewObservations(std::int64_t now_ms);
  // The `navigation` entry for this document, and one `resource` entry per
  // subresource. From the engine, because it is the only thing that knows when a
  // request started -- and a `PerformanceObserver` that answered with nothing is
  // the stub ADR 0012 forbids, which is why these exist rather than the observer
  // alone.
  void SetNavigationTiming(double dom_content_loaded_ms, double load_event_ms,
                           double duration_ms);
  // The legacy `performance.timing`, whose fields are Unix timestamps -- hence
  // the wall clock, which is the only place in this engine one reaches a page.
  // Called when the document's bytes are complete and therefore before the first
  // script runs, which is when youtube.com reads `timing.responseStart`.
  void SetDocumentTiming(std::int64_t navigation_start_wall_ms, double response_end_ms);
  void AddResourceTiming(const std::string& name, const std::string& initiator, double start_ms,
                         double response_end_ms, std::size_t encoded_size,
                         std::size_t decoded_size);
  // The page's clock, published so `performance.now()` can answer with it.
  void TickClock(std::int64_t now_ms);
  // The submission this page's script asked for through `submit()` or
  // `requestSubmit()` and has not had yet. Taken after the script turn ends
  // rather than performed during it: a navigation tears down the interpreter,
  // and doing that while it is on the stack is the use-after-free ADR 0026 §3
  // is written to prevent.
  std::optional<bindings::PendingSubmit> TakePendingSubmit();
  // Fires `load` at the window and moves `readyState` to "complete". True when
  // something was listening, which is the caller's signal that the document
  // may have changed. A page with no `load` handler must not cost a relayout
  // for having finished loading.
  bool NotifyLoad();
  // Whether this page ran any script at all. A page that did cannot be assumed
  // not to have changed the tree from a handler, and a page that did not
  // cannot have handlers to run -- which is what keeps a click on a static
  // document from costing a relayout.
  bool HasListeners() const { return interpreter_ != nullptr; }

  // --- modules, in PageModules.cpp ------------------------------------------
  //
  // The engine fetches; this decides what needs fetching and what to do when it
  // arrives. Split that way because a fetch needs a privacy verdict and a
  // connection pool, and because the *resolver* the interpreter asks is
  // synchronous -- so the graph has to be closed before evaluation, which is a
  // thing only this side knows how to determine. See ModuleLoader.h.

  // URLs the module graph needs and nobody has been asked for yet, marked as
  // asked. A take rather than a read, for the reason the image list is one: it is
  // recomputed from the graph after every arrival and the engine must not be told
  // to fetch the same URL twice.
  // What a relative specifier resolves against. Set when the document is parsed
  // and *not* when the interpreter is built: a module script's source reaches the
  // graph before there is an interpreter, and asking what that module imports
  // needs a base for the answer.
  void SetModuleDocumentUrl(const std::string& url);
  std::vector<std::string> TakeModuleFetches();
  // One module's source, by the URL it was fetched from. An empty source for a
  // fetch that failed, which the module will fail to parse -- a failure reported
  // once at evaluation beats a graph that never closes.
  void AddModuleSource(std::string url, std::string source);
  // Settles every dynamic import whose graph is now closed, and works out what
  // still has to be fetched. True when a promise settled, which is the caller's
  // signal that a page's code ran and the document may have changed.
  bool AdvanceModules();
  // Whether anything is waiting on a module. The loop asks so it keeps turning
  // while a graph is still arriving.
  bool HasPendingModules() const;

  // Anything the page wrote with `console.log`, in order. Collected rather
  // than printed: a page must not be able to write to the terminal the browser
  // was started from.
  const std::vector<std::string>& ConsoleOutput() const;
  // Every script that ended on a throw, in the order they ran, each named by
  // the script it came from.
  //
  // Kept rather than dropped because `Run` deliberately continues past a
  // throw -- which is what a browser does, and which without this makes a page
  // that fails nine scripts in a row indistinguishable from one that ran none.
  // A blank render then has no signal at all behind it, and finding out why
  // means adding this line by hand. It is the same reasoning as ConsoleOutput:
  // collected, never printed, because a page must not be able to write to the
  // terminal the browser was started from.
  const std::vector<std::string>& ScriptErrors() const { return errors_; }

  // Runs `source` in the *page's own* interpreter and answers what it
  // evaluated to, or the thrown value prefixed with "throw ". Empty when no
  // script has run, because there is no interpreter until one does.
  //
  // **A diagnostic entry point, and it earns its place by what it costs to be
  // without one.** Three sessions of this repo's log end at a page that
  // renders wrong with no way to ask it anything: did the custom elements
  // upgrade, is that fetch's response in the tree, what does this shadow root
  // contain. Each is one line of JavaScript and every one of them was
  // previously answered by adding an `fprintf` and rebuilding.
  //
  // It is reachable only from C++ -- `microbrowser_snapshot -eval` is the one
  // caller -- so it widens nothing a page can see. The page's own interpreter
  // rather than a fresh one is the whole point: a probe against a new
  // interpreter would answer about a page that never ran.
  std::string Evaluate(std::string_view source);

 private:
  // One script in document order.
  //
  // A struct rather than three parallel vectors, which is what the timings
  // would otherwise have made this: the source, when it runs, and whether it
  // is a module are three facts about one thing.
  struct Slot {
    // Empty until an external script is fetched, and emptied again once it has
    // run. Those two states want the same treatment -- do not run it -- which
    // is why one `optional` says both.
    std::optional<std::string> source;
    Timing timing = Timing::Blocking;
    // A module is linked and evaluated rather than run: different scoping, a
    // different top level, and `import` means something. It cannot reach the
    // network yet -- the host half of the module loader is what ADR 0011
    // unblocks rather than what it builds -- so an `import` fails with the
    // engine saying there is no resolver, which is a legible answer and not a
    // parse error.
    bool module = false;
    // The `<script>` this slot came from, for `load`/`error` after a fetch.
    const dom::Element* element = nullptr;
  };

  // Installs the resolver and the dynamic-import starter, and resets the graph.
  // Called once per document with the interpreter: a source kept across a
  // navigation would let one document's code be evaluated in another's scope.
  void InstallModuleHost(const std::string& document_url);
  // Records that `url` has to be fetched, once.
  void Want(const std::string& url);
  // Asks the graph what is missing and queues it.
  void RefreshModuleFetches();
  // Builds the interpreter and the binding layer, once. Kept apart from `Run`
  // because an `async` script that lands after the main pass still needs them.
  void EnsureInterpreter(dom::Document& document, const std::string& url,
                         std::int64_t now_ms);
  // Runs every arrived script with this timing, in document order, and empties
  // its slot. True when any ran.
  bool RunTiming(Timing timing);
  // How `ScriptErrors()` names the script in slot `slot`: its URL when it came
  // from one, its position when it was inline.
  std::string SourceName(std::size_t slot) const;

  std::unique_ptr<js::Interpreter> interpreter_;
  std::unique_ptr<bindings::DomBindings> bindings_;
  std::vector<Slot> slots_;
  std::vector<SubresourceRequest> pending_urls_;
  std::vector<std::size_t> pending_slots_;
  // Every `<script>` that has been queued, so a later walk skips it.
  std::unordered_set<const dom::Element*> collected_scripts_;
  // How many of `pending_urls_` the loader has already been asked to fetch.
  std::size_t scripts_requested_ = 0;
  bool ran_ = false;
  bindings::TimerQueue timers_;
  // Not folded into the timers. A timer is a deadline the page chose; a frame
  // is one the browser chose, shared by every callback, and existing only
  // while something has asked for it. See AnimationFrames.
  bindings::AnimationFrames frames_;
  // Not folded into the timers or the frames. An idle callback runs in the gap
  // after those, or when its timeout expires -- see IdleCallbacks.
  bindings::IdleCallbacks idle_;
  // `performance` and `PerformanceObserver`. Not folded into AnimationFrames even
  // though they share an epoch: a frame is a deadline the browser chose and a
  // measurement is a fact the page recorded, and the only thing they have in
  // common is the clock.
  bindings::Performance performance_;
  // What a specifier means and the sources the synchronous resolver answers
  // from. ADR 0011's unanswered question; see ModuleLoader.h.
  ModuleLoader modules_;
  // A dynamic `import()` handed a promise nobody has settled.
  //
  // The promise is a raw pointer *and that is safe*, because the interpreter
  // keeps it in a JavaScript array on the global for exactly this reason -- a raw
  // pointer is worse than invisible to a collector, since it survives the sweep
  // that freed its target.
  struct PendingImport {
    std::string specifier;
    std::string referrer;
    js::Object* promise = nullptr;
  };
  std::vector<PendingImport> pending_imports_;
  // Asked for and not yet arrived, plus what has been handed to the engine, so a
  // URL is fetched once.
  std::vector<std::string> module_fetches_;
  std::set<std::string, std::less<>> requested_modules_;
  std::vector<std::string> errors_;
  bindings::GeometrySource* geometry_ = nullptr;
  bindings::NetworkSource* network_ = nullptr;
  bindings::HistorySource* history_ = nullptr;
  bindings::StorageSource* storage_ = nullptr;
  bindings::CookieSource* cookies_ = nullptr;
  bindings::SocketSource* sockets_ = nullptr;
  bindings::MediaController* media_ = nullptr;
  bindings::CanvasSurface* canvas_ = nullptr;
  bindings::WorkerHost* workers_ = nullptr;
  bool script_strict_dynamic_ = false;
  std::function<void()> trusted_insertion_flush_;
};

}  // namespace microbrowser::engine

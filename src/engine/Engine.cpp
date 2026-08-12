#include "engine/Engine.h"

#include "engine/Clock.h"

#include "engine/LinkResolution.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "csp/SubresourceIntegrity.h"
#include "gfx/DisplayListDiff.h"
#include "gfx/JpegDecoder.h"
#include "gfx/PngDecoder.h"
#include "gfx/SvgDecoder.h"
#include "util/LoadTimeline.h"
#include "util/Env.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"

#include <cstdio>

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Escapes text for placement inside an HTML document.
//
// The error page is built by string concatenation, and the URL in it comes from
// whoever asked for the navigation. Interpolating it raw would make a URL
// containing markup a script injection into the browser's own error page --
// the classic self-XSS in the one document a browser writes itself.
std::string EscapeHtml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

// The blank document. Not an empty string: "" parses to a document with a body
// too, but saying it here means about:blank is a real page rather than a
// failure that happens to look like one.
constexpr std::string_view kBlankDocument =
    "<!DOCTYPE html><html><head><title>New Tab</title></head><body></body></html>";

std::vector<std::byte> BodyBytes(std::string_view body) {
  std::vector<std::byte> out;
  out.reserve(body.size());
  for (const char c : body) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

net::FetchOptions FetchOptionsForSubmission(const FormSubmission& submission) {
  net::FetchOptions options;
  options.method = submission.method;
  if (!submission.body.empty()) {
    options.body = BodyBytes(submission.body);
  }
  if (!submission.content_type.empty()) {
    options.headers.Add("Content-Type", submission.content_type);
  }
  return options;
}

}  // namespace

Engine::Engine(ipc::EngineEndpoint& endpoint, gfx::FontProvider& fonts)
    : endpoint_(endpoint), page_(fonts) {
  // A page's own requests come back here, because a fetch needs the loader and
  // the loader is on this side. Handed over in the constructor rather than per
  // navigation for the reason the geometry source is: it is this object for the
  // life of the engine, and a source that arrived later would leave the first
  // script of the first document without a `fetch`.
  page_.SetNetworkSource(this);
  // And its history questions, for the same reason and with the same lifetime.
  page_.SetHistorySource(this);
  // And its storage. The lifetime argument is the same and the case is the sharpest of
  // the three: Plex's very first inline script, before any bundle loads, reads
  // `sessionStorage`.
  page_.SetStorageSource(this);
  // And `indexedDB`, ADR 0038, same lifetime and same reason as sessionStorage.
  page_.SetIndexedDbSource(this);
  // And its sockets, same lifetime and same reason.
  page_.SetSocketSource(this);
  // And its cookies, same lifetime and same reason. Reddit's GQL reads
  // `csrf_token` from here; Wikipedia's inline script calls `.match` on it.
  page_.SetCookieSource(this);
  loader_.SetBlobRegistry(&page_.BlobUrls());
  // `iframe.contentWindow` on a frame the running script just appended. ADR 0027 §1: the context
  // exists on insertion, so the answer cannot wait for the next turn of the loop.
  page_.ScriptHalf()->FrameWindows().SetSettleHook([this]() { SettleFrameContexts(); });
  page_.SetTrustedInsertionFlush([this]() {
    if (load_.scripts_ran || post_load_.document_interactive) {
      ProcessDynamicScripts();
    }
  });
}

std::string Engine::EvaluateScript(std::string_view source) {
  const std::string answer = page_.EvaluateScript(source);
  if (post_load_.document_interactive) {
    ProcessDynamicScripts();
  }
  // A script turn, same boundary as Advance / RunDueWork / HandleKey: a
  // `requestSubmit`, `location.assign`, or `history.back` queued above is taken
  // now. Without this, a settled page's `-eval` that submits a form records a
  // PendingSubmit that nothing ever drains — `RunLoadToCompletion` only
  // Advances while `IsLoading()`, so the navigation is lost (TD-0026).
  (void)FollowScriptNavigation();
  return answer;
}

bool Engine::HandlePendingMessages() {
  bool produced_output = false;

  while (std::optional<ipc::UiToEngine> message = endpoint_.TryReceive()) {
    if (const auto* navigate = std::get_if<ipc::NavigateMessage>(&*message)) {
      Navigate(navigate->url);
      produced_output = true;
    } else if (const auto* resize = std::get_if<ipc::ResizeViewportMessage>(&*message)) {
      SetViewport(resize->size, resize->device_scale);
      produced_output = true;
    } else if (const auto* scroll = std::get_if<ipc::ScrollMessage>(&*message)) {
      ScrollBy(*scroll);
      produced_output = true;
    } else if (const auto* reload = std::get_if<ipc::ReloadMessage>(&*message)) {
      net::FetchOptions options;
      options.bypass_cache = reload->bypass_cache;
      Navigate(page_.Url(), options);
      produced_output = true;
    } else if (const auto* pointer = std::get_if<ipc::PointerInputMessage>(&*message)) {
      produced_output = HandlePointer(*pointer) || produced_output;
    } else if (const auto* key = std::get_if<ipc::KeyInputMessage>(&*message)) {
      produced_output = HandleKey(*key) || produced_output;
    } else if (const auto* traverse = std::get_if<ipc::TraverseHistoryMessage>(&*message)) {
      produced_output = Traverse(traverse->delta) || produced_output;
    } else if (std::holds_alternative<ipc::StopLoadMessage>(*message)) {
      // Now a real thing to do: the queue drops every outstanding request and
      // their connections close with them. The page keeps whatever had already
      // arrived, which is what a browser does.
      if (load_.active) {
        loader_.CancelAll();
        load_ = PendingLoad{};
        endpoint_.Send(ipc::LoadProgressMessage{1.0f});
        produced_output = true;
      }
    }
  }

  return produced_output;
}

bool Engine::Advance() {
  // The turn boundary, and the one safe place to act on a navigation a script
  // asked for. An observer callback or a resize callback can submit a form, and
  // both run inside the frame -- where navigating would tear down the document
  // being painted and, worse, could be re-entered by the paint of the page it
  // replaced it with. Taking it here is what makes that a queue rather than a
  // recursion a page controls the depth of.
  if (FollowScriptNavigation()) {
    return true;
  }
  // Nothing is loading and nothing is owed: the loader still has to turn, since
  // connections kept between requests time out and a socket the user did not
  // ask to keep open is not something to leave until the next navigation
  // happens along. It promotes nothing and starts nothing here.
  loader_.Advance(NowMilliseconds());
  // The page's sockets, before the early return: a socket has no completion in the
  // loader's queue, so nothing below this line would ever look at one. Its events run
  // script, which is why it is here and not inside the paint.
  bool socket_activity = AdvanceSockets();
  socket_activity = AdvanceEventSources() || socket_activity;
  if (!load_.active && post_load_.images.empty() && post_load_.scripts.empty() &&
      post_load_.frames.empty() && script_fetches_.empty() &&
      module_fetches_.empty() && font_fetches_.empty() && worker_fetches_.empty() &&
      !page_.HasPendingModules()) {
    if (socket_activity) {
      // A message ran a handler, which may have changed the document. Paint, for the
      // reason a timer callback paints: the page a user is looking at is now stale.
      LayoutAndPaint();
    }
    return socket_activity;
  }
  // Everything the loader answered this turn, routed to whoever asked for it. In its own
  // translation unit with the rest of the subresource routing: which table an id is in *is* the
  // question that file exists to answer, and Advance's job is the turn rather than the answer.
  bool moved = false;
  if (DrainCompletionBatch(moved)) {
    return true;
  }

  // The module graph, after the completions and before the load is carried
  // forward: a settled `import()` runs a page's `then`, and that is a script turn
  // like any other.
  if (AdvanceModules()) {
    if (FollowScriptNavigation()) {
      return true;
    }
    page_.InvalidateLayout();
    LayoutAndPaint();
    moved = true;
  }
  if (load_.active) {
    AdvanceLoad();
  }
  return moved;
}

void Engine::AppendWaitDescriptors(util::WaitDescriptorList& out) const {
  loader_.AppendDescriptors(out);
  // An open socket is a descriptor here and nothing else -- no timer, no poll. That is
  // the whole of how ADR 0020 §5's long-lived connection keeps the zero-idle-CPU
  // invariant: the loop blocks on it, and a server that says nothing costs nothing.
  AppendSocketDescriptors(out);
  // A worker's wake pipe, for the same reason and by the same mechanism (ADR 0022 §1). A worker with
  // nothing to say costs nothing; one that posts writes a byte and the loop wakes. No polling, and a
  // page with no workers adds no descriptors at all.
  page_.AppendWorkerDescriptors(out);
  page_.AppendVideoDecoderDescriptors(out);
}

bool Engine::HasRunnableWork() const {
  // Late images count: a canned transport answers instantly and nothing else
  // would ever come back to collect it. Not simply `loader_.HasRunnableWork()`
  // -- with nothing owed, that is always true for a canned transport and the
  // loop would spin instead of blocking, which is the zero-idle invariant.
  // A socket with something *queued* is work; an open socket waiting on a server is not.
  // Answering true for the second would make the loop spin for as long as a page held a
  // connection, which is exactly what ADR 0020 §5 says this feature must not cause. It is
  // outside the `loader_` conjunction because a socket is not a request and the loader
  // knows nothing about it.
  if (SocketsHaveWork()) {
    return true;
  }
  // A worker message already queued is work now. Without this, a message that arrived between the wait
  // returning and the drain would sit until the next unrelated wakeup -- which for an idle page is
  // never.
  if (page_.WorkersHaveWork()) {
    return true;
  }
  return (load_.active || !post_load_.images.empty() || !post_load_.scripts.empty() ||
          !post_load_.frames.empty() || !script_fetches_.empty() ||
          !module_fetches_.empty() || !font_fetches_.empty() || !worker_fetches_.empty() ||
          page_.HasPendingModules()) &&
         loader_.HasRunnableWork();
}

std::string Engine::LoadingReason() const {
  std::ostringstream out;
  auto note = [&](bool on, const char* label) {
    if (!on) {
      return;
    }
    if (out.tellp() > 0) {
      out << ',';
    }
    out << label;
  };
  note(load_.active, "load");
  note(!post_load_.images.empty(), "late_images");
  note(!post_load_.scripts.empty(), "late_scripts");
  note(!post_load_.frames.empty(), "late_frames");
  if (!script_fetches_.empty()) {
    note(true, "script_fetches");
    out << "(n=" << script_fetches_.size() << ')';
  }
  note(!module_fetches_.empty(), "module_fetches");
  note(!font_fetches_.empty(), "font_fetches");
  note(page_.HasPendingModules(), "pending_modules");
  note(page_.ScriptHalf()->HasOutstandingScriptFetches(), "outstanding_scripts");
  return out.str();
}

std::optional<std::uint32_t> Engine::NextDeadlineMs() const {
  const std::int64_t now_ms = NowMilliseconds();
  const std::optional<std::uint32_t> timers = page_.NextWakeDelay(now_ms);
  // Not gated on `load_.active` any more: with nothing loading the loader still
  // answers when it holds an idle connection, and that deadline is the only
  // thing that will ever close it.
  std::optional<std::uint32_t> network = loader_.NextDeadlineMs(now_ms);
  // A stream waiting to reconnect. The only deadline a long-lived connection contributes:
  // an *open* one contributes none, which is what lets an idle page with a stream block.
  // A worker with a message already queued is *work now*, not a deadline: returning zero here would
  // spin, and the loop's own "is there work" question is what `WorkersHaveWork` answers.
  if (const std::optional<std::uint32_t> retry = NextEventSourceDeadlineMs(now_ms)) {
    network = network.has_value() ? std::min(*network, *retry) : retry;
  }
  if (!timers.has_value()) {
    return network;
  }
  if (!network.has_value()) {
    return timers;
  }
  return std::min(*timers, *network);
}

bool Engine::RunDueWork() {
  // Before the work rather than after: running a timer may write to the tree, and the restyle that
  // follows starts transitions against *this* instant.
  page_.SetAnimationTime(NowMilliseconds());
  // Messages workers sent back. Drained before the page's own timers, because a worker's answer is why
  // the loop woke -- and because a handler may post again, which the same turn should carry out.
  bool from_workers = page_.DeliverWorkerMessages();
  // And any worker a handler just constructed. Started here rather than only during a load, because
  // `new Worker` inside a message handler is how a page builds a pool.
  StartWorkerScriptRequests();
  if (from_workers) {
    LayoutAndPaint();
  }
  bool script_ran = false;
  const Page::DueWorkKind due = page_.RunDueWork(NowMilliseconds(), &script_ran);
  // A timer that appended an `<iframe>` made a browsing context. Gated on `script_ran` because
  // nothing else can have: a video frame and an animation tick cannot append an element.
  if (script_ran) {
    (void)ProcessDynamicFrames();
  }
  if (due == Page::DueWorkKind::None) {
    return from_workers;
  }
  if (FollowScriptNavigation()) {
    return true;
  }
  // Paint-only due work (WAAPI style ticks, video frames) must not enter Layout:
  // MutationVersion has moved, so Layout would reflow every frame (TD-0021).
  if (due == Page::DueWorkKind::Paint) {
    // While a load is outstanding, skip PaintAndSend: restyle already updated
    // the boxes; the next LayoutAndPaint from a completion shows them.
    if (!IsLoading()) {
      PaintAndSend();
    }
  } else {
    LayoutAndPaint();
  }
  // Snapshot load/drain loops treat `true` as "skip the wait". Animation,
  // video, and attr-restyle ticks must not do that — they keep NextDeadlineMs
  // armed forever (infinite Element.animate) and would LayoutAndPaint-spin
  // (TD-0021). Script timers/rAF/tasks still return true so host-task storms
  // drain without sleeping on a 16ms frame boundary (TD-0018).
  if (IsLoading() || (!script_ran && !from_workers)) {
    return from_workers;
  }
  return true;
}

void Engine::OnCompletion(Loader::Completion completion) {
  if (!load_.active) {
    return;
  }
  if (completion.id == load_.document && !load_.document_arrived) {
    load_.document_arrived = true;
    OnDocument(std::move(completion.result));
    return;
  }
  const auto found = load_.resources.find(completion.id);
  if (found == load_.resources.end()) {
    // A completion for a request this load did not make. It cannot happen while
    // a navigation cancels everything, and dropping it is the right answer if
    // it ever does.
    return;
  }
  const PendingResource resource = found->second;
  load_.resources.erase(found);
  ++load_.finished_resources;

  // One `resource` entry per subresource, whatever became of it. Recorded even
  // for a failure, because a page computing a cache hit rate or a total transfer
  // size counts what it asked for rather than what worked -- and an entry list
  // that quietly omitted the failures would make every such number wrong in a
  // direction nobody could see.
  RecordResourceTiming(resource, completion.result);

  switch (resource.kind) {
    case ResourceKind::StyleSheet:
      --load_.sheets_outstanding;
      if (completion.result.ok &&
          !IntegrityHolds(page_.PendingStyleSheets(), resource.index, completion.result.body)) {
        // A sheet whose bytes are not the ones the page named is not applied.
        // Counted as a failure rather than as an absence: from the page's point
        // of view a CDN that served something else and a CDN that served
        // nothing are the same, and that is the whole point of asking.
        AddPerformanceCounter(PerfCounterId::EngineStyleSheetsFailed);
        break;
      }
      if (completion.result.ok) {
        page_.AddStyleSheet(resource.index, completion.result.body);
        // A sheet can declare an `@font-face`, and the sheet arrives after the
        // document -- so the font pass runs again here rather than only once.
        StartFontRequests();
        // And a sheet is what *names* a background image: `AddStyleSheet` has
        // just re-collected them, and until this line nothing turned the new
        // ones into requests until the next paint. On Hacker News that put
        // `triangle.svg` -- named by `news.css`, which arrived at 726ms -- on
        // the wire at 1104ms, after the first frame, for a round trip the page
        // then had to wait out. Measured with the load timeline; the same shape
        // costs every page whose icons come from a stylesheet, which is every
        // page that has any.
        StartImageRequests();
        AddPerformanceCounter(PerfCounterId::EngineStyleSheetsLoaded);
      } else {
        // A stylesheet that does not load is a page rendered without it, which
        // is what every browser does. It is not a navigation failure.
        AddPerformanceCounter(PerfCounterId::EngineStyleSheetsFailed);
      }
      break;
    case ResourceKind::Script: {
      // Which counter this came off decides whether the first paint was
      // waiting for it. `async` says the page is not, and honouring that is
      // the whole of what the attribute means.
      const bool is_async = page_.ScriptHalf()->IsAsync(resource.index);
      --(is_async ? load_.async_scripts_outstanding : load_.scripts_outstanding);
      if (completion.result.ok &&
          !IntegrityHolds(page_.ScriptHalf()->PendingUrls(), resource.index, completion.result.body)) {
        // Refused to execute. The slot stays empty and the scripts after it
        // still run, which is what a failed script load already did.
        AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
        break;
      }
      if (completion.result.ok) {
        page_.AddScript(resource.index, std::move(completion.result.body));
        AddPerformanceCounter(PerfCounterId::EngineScriptsLoaded);
        if (load_.scripts_ran) {
          if (ProcessDynamicScripts()) {
            if (FollowScriptNavigation()) {
              return;
            }
          }
        } else if (is_async && page_.ScriptHalf()->RunReadyAsync()) {
          // It landed after the page was already up. Running it can have
          // changed the tree, so the page is laid out again -- which is what
          // an async script arriving late looks like in every browser.
          if (FollowScriptNavigation()) {
            return;
          }
          page_.InvalidateLayout();
          LayoutAndPaint();
        }
      } else {
        // A script that does not load leaves its slot empty and the ones after
        // it still run. A page whose analytics tag is blocked is a page, which
        // is the whole reason the blocking engine can be pointed at one.
        AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
      }
      break;
    }
    case ResourceKind::Image:
      --load_.images_outstanding;
      if (completion.result.ok) {
        // Held, not decoded. See PendingLoad::image_bytes.
        load_.image_bytes.emplace_back(resource.src, std::move(completion.result.body));
      } else {
        AddPerformanceCounter(PerfCounterId::EngineImagesFailed);
      }
      break;
    case ResourceKind::Frame:
      // The child document goes straight into its context, which parses it, collects *its*
      // subresources and -- because a frame may hold a frame -- collects its own frames too. The
      // decrement is inside OnFrameFetch so that every exit from it agrees about the count.
      OnFrameFetch(std::move(completion), resource);
      break;
  }

  if (load_.total_resources > 0) {
    endpoint_.Send(ipc::LoadProgressMessage{load_.Progress()});
  }
}

void Engine::RecordResourceTiming(const PendingResource& resource,
                                  const Loader::Result& result) {
  const char* initiator = "other";
  switch (resource.kind) {
    case ResourceKind::StyleSheet:
      initiator = "css";
      break;
    case ResourceKind::Script:
      initiator = "script";
      break;
    case ResourceKind::Image:
      initiator = "img";
      break;
    case ResourceKind::Frame:
      initiator = "iframe";
      break;
  }
  const double start = 0.0;
  const double end = static_cast<double>(NowMilliseconds() - load_.started_ms);
  // The name is the URL the page wrote, resolved -- which is what
  // `getEntriesByName` is asked with and what a page matches against its own
  // `<script src>`. `decodedBodySize` is what arrived; `encodedBodySize` is the
  // same, because the loader has already undone any content coding by here and a
  // number this layer invented would be worse than one that is honestly equal.
  const std::string name = result.final_url.empty() ? resource.src : result.final_url;
  page_.AddResourceTiming(name, initiator, start, end, result.body.size(), result.body.size());
}

void Engine::OnDocument(Loader::Result result) {
  if (!result.ok) {
    const std::string url = load_.url;
    load_ = PendingLoad{};
    ShowError(url, result.error.empty() ? "the load failed" : result.error);
    return;
  }

  // Every `Content-Security-Policy` header, in the order they arrived, and
  // *all* of them have to allow -- so a second header cannot loosen the first.
  // Report-Only is deliberately not read: it is neither enforced nor reported.
  csp::PolicyList policies;
  for (const auto& [name, value] : result.headers) {
    if (util::EqualsAsciiCaseInsensitive(name, "content-security-policy")) {
      policies.AddFromHeader(value);
      AddPerformanceCounter(PerfCounterId::CspPolicies);
    }
  }

  page_.Load(result.body, result.final_url.empty() ? load_.url : result.final_url,
             std::move(policies), result.content_type);

  util::LoadTimeline::MarkWith("document.arrived",
                               std::to_string(result.body.size()) + " bytes " +
                                   (result.final_url.empty() ? load_.url : result.final_url));

  // The document's bytes are complete, which is the last moment
  // `performance.timing` needs before a page can read it: the next thing that
  // runs a script is `AdvanceLoad`, and youtube.com's first inline script reads
  // `timing.responseStart` before it does anything else. Recorded here rather
  // than with the `navigation` entry at the end of the load for that reason --
  // that one is produced after the last paint, which is far too late.
  //
  // *After* `Page::Load`, which detaches the previous document's script half and
  // with it the object this is stored on.
  page_.SetDocumentTiming(load_.started_wall_ms,
                          static_cast<double>(NowMilliseconds() - load_.started_ms));
  if (traversing_ || replacing_document_) {
    // A traversal's entry is already in the list, at its own index. Its URL is
    // rewritten rather than pushed, because a redirect can land somewhere else
    // and the entry has to say where the document actually is.
    // `location.replace` uses the same shape: rewrite the current entry with the
    // new document rather than growing history (youtube consent save redirect).
    traversing_ = false;
    replacing_document_ = false;
    history_.SetCurrentUrl(page_.Url());
    ++document_id_;
    if (HistoryEntry* entry = history_.MutableCurrent()) {
      // A fresh document, so the entry it belongs to is a fresh one too: coming
      // back to it must be a load rather than a `popstate` on a document that no
      // longer exists.
      entry->document = document_id_;
      entry->state = js::SerializedValue{};
    }
  } else {
    history_.PushDocument(page_.Url(), ++document_id_);
  }
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  history_.SetCurrentTitle(page_.Title());
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  SendHistoryState();

  // A data: or about: document has no base to resolve against, so a relative
  // href in one has nowhere to point. The page's own base is what this reads,
  // rather than its address, because a `<base href>` the document declared is
  // what its relative URLs mean -- and one answer to that question is what
  // stops a `<base>` from applying to the stylesheets and not to the images.
  load_.base = page_.BaseUrl();
  if (load_.base.has_value()) {
    StartSubresources();
  }
}

void Engine::AdvanceLoad() {
  if (!load_.active || !load_.document_arrived) {
    return;
  }
  if (load_.scripts_ran) {
    if (ProcessDynamicScripts()) {
      if (FollowScriptNavigation()) {
        return;
      }
    }
  }
  // Scripts run once every render-blocking resource has resolved, and before
  // the images are decoded. Stylesheets first so a script that asks about a
  // style sees the ones the document declared; images after, so a script that
  // sets a width has said so before an SVG is rasterized to it.
  if (load_.MayRunScripts()) {
    if (!script_prelude_.empty()) {
      // Before the page's own scripts, once. Hooks installed here see every
      // call the player makes; `-eval` after the load cannot.
      (void)page_.EvaluateScript(script_prelude_);
      script_prelude_.clear();
    }
    // **Before the document's own scripts, and this is not the same call as the one in
    // `ProcessDynamicFrames`.** An `<iframe>` with no `src` -- and one with `srcdoc` -- is handed
    // its document synchronously during the subresource pass, so by the time the parser's scripts
    // run it already exists and `iframe.contentWindow` has to answer with it. That is how most of
    // the suite builds a second realm: `<iframe></iframe>` followed by an inline script that reads
    // `contentWindow` on its first line. Without this the window is null there and the page fails
    // at the *first* thing it does.
    RunFrameScripts(page_, /*run_scripts=*/false);
    page_.RunScripts(NowMilliseconds());
    load_.scripts_ran = true;
    post_load_.document_interactive = true;
    // When the document's own scripts finished, which is what a page reads as
    // `domContentLoadedEventStart`. The counter for it is fired by the binding
    // layer that dispatches the event; this is only the number.
    load_.dom_content_loaded_ms = NowMilliseconds();
    // A script that submitted a form navigates now, which throws this load
    // away -- so nothing below may touch `load_`. reddit's front door is this
    // line: its interstitial fills in a form from `DOMContentLoaded` and
    // submits it, and the answer to that submission is the real page.
    if (FollowScriptNavigation()) {
      return;
    }
    ProcessDynamicScripts();
    if (FollowScriptNavigation()) {
      return;
    }
  }
  if (load_.MayPaint()) {
    Paint();
  } else if (load_.painted && !load_.image_bytes.empty()) {
    // Images that arrived after the first frame, now that they no longer hold
    // it back. Batched here rather than decoded one at a time as each
    // completion lands, and that is the point of doing it in `AdvanceLoad`: a
    // page's images tend to arrive together -- eleven of wikipedia's finish
    // inside 250 microseconds of each other -- and decoding each one at its own
    // completion would lay the document out eleven times to reach the same
    // frame.
    DecodePendingImages();
    load_.image_bytes.clear();
    page_.InvalidateLayout();
    LayoutAndPaint();
  }
  if (load_.IsFinished()) {
    if (load_.scripts_outstanding > 0 || load_.modules_outstanding > 0) {
      return;
    }
    ProcessDynamicScripts();
    if (FollowScriptNavigation()) {
      return;
    }
    if (load_.scripts_outstanding > 0 || load_.modules_outstanding > 0) {
      return;
    }
    // The `navigation` entry, before `load_` goes: it is the only thing that
    // knows when this navigation started, and a page observing `navigation` with
    // `buffered: true` reads it from a script that has not run yet.
    const std::int64_t finished_ms = NowMilliseconds();
    const auto since_start = [&](std::int64_t at) {
      return static_cast<double>(at - load_.started_ms);
    };
    page_.SetNavigationTiming(since_start(load_.dom_content_loaded_ms), since_start(finished_ms),
                              since_start(finished_ms));
    // Script fetches still in flight move to `post_load_.scripts` rather than being
    // dropped when `load_.resources` goes. A completion that arrives after this
    // point must still reach the slot it was started for.
    for (const auto& [id, resource] : load_.resources) {
      if (resource.kind == ResourceKind::Script) {
        post_load_.scripts[id] = resource.index;
      }
    }
    // Only now is the navigation over. It stayed alive past the first frame
    // for the `async` scripts the page said it would not wait for, which is
    // the difference between not blocking on one and dropping it.
    load_ = PendingLoad{};
    // And only now does `load` fire: it means the document *and its
    // subresources*, which is the difference between it and DOMContentLoaded.
    // The relayout happens only when something was listening, so a page with
    // no handler does not pay for having finished.
    if (page_.NotifyLoad()) {
      if (FollowScriptNavigation()) {
        return;
      }
      page_.InvalidateLayout();
      LayoutAndPaint();
    }
    // And the observers, because the `navigation` entry was produced *after* the
    // last paint of this load: it cannot exist before the load is over, and the
    // frame that would have delivered it has already happened. Without this a
    // page observing `navigation` hears nothing until something else causes a
    // frame -- which on a settled page is never, and is exactly the shape of a
    // `PerformanceObserver` that appears to work.
    if (page_.DeliverObservations(NowMilliseconds())) {
      if (FollowScriptNavigation()) {
        return;
      }
      page_.InvalidateLayout();
      LayoutAndPaint();
    }
  }
}

void Engine::Paint() {
  DecodePendingImages();
  // Cleared, because images no longer hold this frame back and more will arrive
  // after it: leaving them here would decode every already-decoded image again
  // on the next batch.
  load_.image_bytes.clear();
  load_.painted = true;
  // The title again. It was sent when the document committed, before its
  // scripts ran, and a page that changes its own title -- which is most of
  // them -- would otherwise keep whatever the markup said, or the URL.
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::Navigate(const std::string& url) {
  Navigate(url, {});
}

void Engine::Navigate(const std::string& url, const net::FetchOptions& options) {
  Navigate(url, options, nullptr);
}

void Engine::Navigate(const std::string& url, const net::FetchOptions& options,
                      const url::Url* referrer_document) {
  util::PerformanceTrace::Scope scope("engine::Navigate");
  AddPerformanceCounter(PerfCounterId::EngineNavigations);
  // The origin every other milestone is measured from. Here rather than at the
  // first request, because the time between deciding to navigate and asking for
  // a byte is time the user is waiting too.
  util::LoadTimeline::Begin(url);

  page_.SetScrollOffsetY(0.0f);

  // Everything the previous navigation had in flight goes now, connections and
  // all. That is what makes a response for a document that is gone
  // undeliverable rather than merely ignored -- ADR 0011 asked for it by
  // construction, and this is the construction.
  loader_.CancelAll();
  // And the page's sockets. ADR 0020 §5: a connection dies with the document that opened
  // it, and erasing the table *is* closing it -- the connection's destructor closes its
  // transport. One line rather than a shutdown sequence, because that is a property of
  // the type rather than a sequence someone has to remember.
  CloseAllSockets();
  // Outgoing document script next: CancelAll drops in-flight fetches, but timers
  // on the still-mounted page used to fire `generate_204` / consent beacons into
  // the new document's H2 session and kill the reload GET (TD-0048).
  page_.AbandonForNavigation();
  load_ = PendingLoad{};
  // The images the previous document was still fetching go with it, for the
  // reason `load_` does: a response for a page that is gone must be
  // undeliverable rather than merely ignored.
  post_load_.Clear();
  // And so do the requests its script made. A `fetch` belongs to the document
  // that asked for it: ADR 0020 §1 says a request that outlives its navigation
  // is what `AbortController` exists to prevent, and a navigation is the one
  // abort nobody has to ask for.
  script_fetches_.clear();
  // And the modules its graph was still fetching, for the same reason.
  module_fetches_.clear();
  font_fetches_.clear();
  worker_fetches_.clear();
  load_.active = true;
  load_.started_ms = NowMilliseconds();
  load_.started_wall_ms = NowWallMilliseconds();
  load_.url = url.empty() ? std::string("about:blank") : url;
  load_.bypass_cache = options.bypass_cache;

  if (url.empty() || url == "about:blank") {
    // The blank document is not fetched. It still goes through the same state
    // machine, so that "what happens after a document arrives" has one
    // implementation rather than two.
    load_.document_arrived = true;
    Loader::Result blank;
    blank.ok = true;
    blank.body = std::string(kBlankDocument);
    blank.final_url = load_.url;
    blank.status = 200;
    OnDocument(std::move(blank));
    AdvanceLoad();
    return;
  }

  endpoint_.Send(ipc::LoadProgressMessage{0.0f});
  load_.document = loader_.StartLoad(url, NowSeconds(), options, referrer_document);
  // One turn now, so that a data: URL or a cache hit does not wait for the
  // loop to come back round.
  Advance();
}

void Engine::NavigateFromCurrentDocument(const std::string& url,
                                         const net::FetchOptions& options) {
  const std::optional<url::Url> referrer = url::Url::Parse(page_.Url());
  Navigate(url, options, referrer.has_value() ? &*referrer : nullptr);
}

bool Engine::FollowScriptNavigation() {
  // A traversal first, because `history.back()` is the one a router calls and
  // both are taken at the same boundary for the same reason.
  if (FollowPendingTraversal()) {
    return true;
  }
  // A full `location.assign` tears the document down. A fragment-only change
  // does not — and a `requestSubmit` queued in the same turn (or left pending
  // while a hash write ran first) must still be taken (TD-0026).
  if (FollowPendingLocationNavigation()) {
    if (IsLoading()) {
      return true;
    }
  }
  const std::optional<FormSubmission> submission = page_.TakeScriptFormSubmission();
  if (submission.has_value()) {
    AddPerformanceCounter(PerfCounterId::EngineScriptNavigations);
    return Navigate(*submission);
  }
  // And the activation behaviour of an `element.click()` a script ran, which
  // is the same walk a real pointer release takes -- one algorithm, because
  // two of them is how a checkbox toggles under the mouse and not under
  // `click()`. See Page::ApplyScriptActivation.
  bool changed_document = false;
  std::optional<std::string> href;
  const std::optional<FormSubmission> activated =
      page_.ApplyScriptActivation(changed_document, href);
  if (activated.has_value()) {
    AddPerformanceCounter(PerfCounterId::EngineScriptNavigations);
    return Navigate(*activated);
  }
  if (href.has_value()) {
    const std::optional<std::string> resolved = ResolveLink(*href, page_.Url());
    if (resolved.has_value()) {
      AddPerformanceCounter(PerfCounterId::EngineScriptNavigations);
      // **`NavigateToFragment` first, exactly as the real click path does.**
      // Leaving it out is what hung the loop: `a.click()` on an `href="#link"`
      // went straight to a full navigation, which re-fetched the same
      // document, whose script clicked the anchor again. One anchor click and
      // the page reloaded forever. A same-document hash change is a scroll and
      // a `hashchange`, not a load, and both callers have to agree about that
      // for the same reason they share the activation walk.
      if (NavigateToFragment(*resolved)) {
        return true;
      }
      NavigateFromCurrentDocument(*resolved, {});
      return true;
    }
  }
  if (changed_document) {
    page_.InvalidateLayout();
    return true;
  }
  return false;
}

bool Engine::Navigate(const FormSubmission& submission) {
  AddPerformanceCounter(PerfCounterId::EngineFormSubmissions);
  // `form-action`. Here rather than where the submission is built, because a
  // click, the Enter key and a script all arrive at this one function -- and
  // three places checking a policy is two chances to have the wrong answer.
  if (!page_.Policy().AllowsUrl(csp::Directive::FormAction, submission.url)) {
    return false;
  }
  const std::optional<std::string> resolved = ResolveLink(submission.url, page_.Url());
  if (!resolved.has_value()) {
    return false;
  }
  util::LoadTimeline::MarkWith("navigation.form", *resolved);
  NavigateFromCurrentDocument(*resolved, FetchOptionsForSubmission(submission));
  return true;
}

void Engine::ShowError(std::string_view url, std::string_view message) {
  // A browser that shows nothing when a load fails is indistinguishable from a
  // browser that has hung.
  std::string html =
      "<!DOCTYPE html><html><head><title>Cannot load page</title>"
      "<style>body{margin:32px;font-family:sans-serif;color:#202028}"
      "h1{font-size:1.5em;color:#8b1a1a}code{font-family:monospace;color:#404050}</style>"
      "</head><body><h1>Cannot load this page</h1><p>";
  html += EscapeHtml(message);
  html += "</p><p><code>";
  html += EscapeHtml(url);
  html += "</code></p></body></html>";

  page_.Load(html, std::string(url));
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::SetViewport(const gfx::IntSize& size, float device_scale) {
  if (size == viewport_size_ && device_scale == device_scale_) {
    return;
  }
  viewport_size_ = size;
  device_scale_ = device_scale;
  // The page selects images against this, so it is told before anything is
  // laid out or fetched. The scale is guarded here rather than at every use:
  // it arrives over IPC from the UI process, and a zero would divide the
  // viewport into infinity.
  const float scale = device_scale_ > 0.0f ? device_scale_ : 1.0f;
  page_.SetViewport(css::MediaContext{static_cast<float>(size.width) / scale,
                                      static_cast<float>(size.height) / scale, scale});
  // A resize changes the containing block, so it relays out. This is the one
  // input that does.
  LayoutAndPaint();
  // After layout: iron-fit's `notifyResize` from a window `resize` listener
  // reads geometry, and answering before layout would refit against the old
  // containing block. Cheap when nobody listens (DispatchAtWindow checks).
  (void)page_.NotifyWindowResize();
}

const std::vector<std::string>& CspViolations(const Engine& engine) {
  return engine.page_.Policy().Violations();
}

void SettleForSnapshot(Engine& engine) {
  if (!engine.post_load_.document_interactive) {
    return;
  }
  // Only drop the box tree when dynamic scripts actually changed the document.
  // Unconditionally InvalidateLayout here doubled youtube `/results` memory past
  // bad_alloc on an already-painted tree (TD-0031).
  if (engine.ProcessDynamicScripts()) {
    engine.page_.InvalidateLayout();
  }
  engine.LayoutAndPaint();
}

}  // namespace microbrowser::engine

#include "engine/Engine.h"

#include "engine/Clock.h"

#include "engine/LinkResolution.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "csp/SubresourceIntegrity.h"
#include "gfx/DisplayListDiff.h"
#include "gfx/JpegDecoder.h"
#include "gfx/PngDecoder.h"
#include "gfx/SvgDecoder.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"

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
  // And its sockets, same lifetime and same reason.
  page_.SetSocketSource(this);
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
  if (!load_.active && late_images_.empty() && script_fetches_.empty() &&
      module_fetches_.empty() && font_fetches_.empty() && !page_.HasPendingModules()) {
    if (socket_activity) {
      // A message ran a handler, which may have changed the document. Paint, for the
      // reason a timer callback paints: the page a user is looking at is now stale.
      LayoutAndPaint();
    }
    return socket_activity;
  }
  std::vector<Loader::Completion> completions = loader_.TakeCompletions();
  bool moved = false;
  for (Loader::Completion& completion : completions) {
    if (late_images_.find(completion.id) != late_images_.end()) {
      moved = OnLateImage(std::move(completion)) || moved;
      continue;
    }
    if (font_fetches_.find(completion.id) != font_fetches_.end()) {
      if (OnFontFetch(std::move(completion))) {
        moved = true;
        LayoutAndPaint();
      }
      continue;
    }
    if (module_fetches_.find(completion.id) != module_fetches_.end()) {
      moved = OnModuleFetch(std::move(completion)) || moved;
      continue;
    }
    if (script_fetches_.find(completion.id) != script_fetches_.end()) {
      moved = OnScriptFetch(std::move(completion)) || moved;
      // A `then` handler can navigate, and a navigation from inside one leaves
      // every id in this batch belonging to a document that is gone.
      if (FollowScriptNavigation()) {
        return true;
      }
      continue;
    }
    if (!load_.active) {
      // A completion for a load that is gone. Only a late image outlives one.
      continue;
    }
    OnCompletion(std::move(completion));
    if (!load_.active) {
      // The document failed, or a navigation replaced this one from inside a
      // completion. Anything still in the batch belongs to a load that is gone.
      return true;
    }
  }
  moved = moved || !completions.empty();
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
  return (load_.active || !late_images_.empty() || !script_fetches_.empty() ||
          !module_fetches_.empty() || !font_fetches_.empty() || page_.HasPendingModules()) &&
         loader_.HasRunnableWork();
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
  if (!page_.RunDueWork(NowMilliseconds())) {
    return false;
  }
  if (FollowScriptNavigation()) {
    return true;
  }
  LayoutAndPaint();
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
      const bool is_async = page_.PendingScriptIsAsync(resource.index);
      --(is_async ? load_.async_scripts_outstanding : load_.scripts_outstanding);
      if (completion.result.ok &&
          !IntegrityHolds(page_.PendingScripts(), resource.index, completion.result.body)) {
        // Refused to execute. The slot stays empty and the scripts after it
        // still run, which is what a failed script load already did.
        AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
        break;
      }
      if (completion.result.ok) {
        page_.AddScript(resource.index, std::move(completion.result.body));
        AddPerformanceCounter(PerfCounterId::EngineScriptsLoaded);
        if (is_async && load_.scripts_ran && page_.RunReadyAsyncScripts()) {
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
  if (traversing_) {
    // A traversal's entry is already in the list, at its own index. Its URL is
    // rewritten rather than pushed, because a redirect can land somewhere else
    // and the entry has to say where the document actually is.
    traversing_ = false;
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
  // Scripts run once every render-blocking resource has resolved, and before
  // the images are decoded. Stylesheets first so a script that asks about a
  // style sees the ones the document declared; images after, so a script that
  // sets a width has said so before an SVG is rasterized to it.
  if (load_.MayRunScripts()) {
    page_.RunScripts(NowMilliseconds());
    load_.scripts_ran = true;
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
  }
  if (load_.MayPaint()) {
    Paint();
  }
  if (load_.IsFinished()) {
    // The `navigation` entry, before `load_` goes: it is the only thing that
    // knows when this navigation started, and a page observing `navigation` with
    // `buffered: true` reads it from a script that has not run yet.
    const std::int64_t finished_ms = NowMilliseconds();
    const auto since_start = [&](std::int64_t at) {
      return static_cast<double>(at - load_.started_ms);
    };
    page_.SetNavigationTiming(since_start(load_.dom_content_loaded_ms), since_start(finished_ms),
                              since_start(finished_ms));
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
  load_ = PendingLoad{};
  // The images the previous document was still fetching go with it, for the
  // reason `load_` does: a response for a page that is gone must be
  // undeliverable rather than merely ignored.
  late_images_.clear();
  // And so do the requests its script made. A `fetch` belongs to the document
  // that asked for it: ADR 0020 §1 says a request that outlives its navigation
  // is what `AbortController` exists to prevent, and a navigation is the one
  // abort nobody has to ask for.
  script_fetches_.clear();
  // And the modules its graph was still fetching, for the same reason.
  module_fetches_.clear();
  font_fetches_.clear();
  load_.active = true;
  load_.started_ms = NowMilliseconds();
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
  const std::optional<FormSubmission> submission = page_.TakeScriptFormSubmission();
  if (!submission.has_value()) {
    return false;
  }
  AddPerformanceCounter(PerfCounterId::EngineScriptNavigations);
  return Navigate(*submission);
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
}

int Engine::ScrollY() const {
  return static_cast<int>(page_.ScrollOffsetY());
}

int Engine::MaxScroll() const {
  return std::max(0, static_cast<int>(page_.ContentHeight()) - viewport_size_.height);
}

void Engine::LayoutAndPaint() {
  // The instant this frame is at, set before anything restyles. Every restyle in a turn -- a hover, a
  // script, a resize -- must agree about what time it is, or two halves of one transition end up at
  // two different progresses on the same frame.
  page_.SetAnimationTime(NowMilliseconds());
  if (viewport_size_.width > 0) {
    page_.Layout(static_cast<float>(viewport_size_.width) / device_scale_);
    page_.SetScrollOffsetY(static_cast<float>(std::clamp(ScrollY(), 0, MaxScroll())));
  }
  PaintAndSend();
}

void Engine::PaintAndSend() { PaintAndSend(gfx::IntPoint{}, nullptr); }

void Engine::PaintAndSend(gfx::IntPoint scroll_delta, const gfx::IntRect* only) {
  util::PerformanceTrace::Scope scope("engine::Paint");
  AddPerformanceCounter(PerfCounterId::EnginePaintsProduced);
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);

  const gfx::IntRect viewport{0, 0, viewport_size_.width, viewport_size_.height};
  if (viewport.IsEmpty()) {
    return;
  }

  // The frame's observation step, and the single place it happens: every path
  // that puts something on screen comes through here, so an observer cannot be
  // sampled twice for one frame or missed for another. ADR 0018 §5 -- the
  // sample is at the frame and never inside the scroll that caused it.
  //
  // A callback that ran may have moved the document, and a frame whose
  // geometry changed is no longer the previous frame shifted: the blit is off
  // the table and the damage goes back to being whatever the display-list diff
  // finds.
  if (page_.DeliverObservations(NowMilliseconds())) {
    scroll_delta = gfx::IntPoint{};
    only = nullptr;
  }
  // And the images that came within reach of the scrollport while all that was
  // happening. Here rather than beside the observers because it is not one: a
  // lazy image is a geometry test the browser performs, not a callback a page
  // registered, and it works on a page with no script at all.
  StartImageRequests();

  pending_.Clear();
  // The canvas behind the document, painted here rather than by the page: a
  // document shorter than the viewport still has a window under it, and the
  // page has no opinion about pixels it does not cover.
  pending_.FillRect(viewport, gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  page_.Paint(pending_);

  ipc::PaintFrameMessage frame;
  // A scroll knows its own damage and the diff cannot compute it: every command
  // in the list moved by the same amount, so the diff reports the whole viewport
  // and the browser repaints the window for a two-pixel wheel notch. This is
  // the one place the truth is cheaper to state than to derive. Likewise a box
  // that scrolled inside the page: what changed is its clip rectangle, and
  // nothing else.
  if (scroll_delta != gfx::IntPoint{}) {
    frame.damage = ScrollDamage(scroll_delta);
    frame.scroll_delta = scroll_delta;
  } else if (only != nullptr) {
    frame.damage.push_back(*only);
  } else {
    gfx::DirtyRegion damage;
    const bool bounded = gfx::ComputeDamage(display_list_, pending_, viewport, damage);
    if (bounded && damage.IsEmpty()) {
      // Nothing on screen would change. Sending the frame anyway would make the
      // UI upload a texture to draw the same picture, which is most of what a
      // browser wastes power on.
      AddPerformanceCounter(PerfCounterId::EnginePaintsSkipped);
      return;
    }
    // Empty damage means "the whole viewport", which is what the diff reports
    // when it cannot bound the change -- a clip moved, and every command after a
    // clip reads it as state.
    if (bounded) {
      frame.damage = damage.Rects();
    }
  }
  frame.display_list = pending_;
  display_list_ = pending_;
  endpoint_.Send(std::move(frame));
}

}  // namespace microbrowser::engine

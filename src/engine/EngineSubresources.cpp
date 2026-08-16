// How a subresource is fetched, and whether the bytes that came back are the
// ones the document said they would be.
//
// Its own translation unit for the reason EngineFetch.cpp is: Engine.cpp is at
// its module cap, and a file over its cap means a missing seam rather than a
// bigger file. The seam here is a real one -- everything in this file is one
// question asked twice, about a stylesheet and about a script, and the whole
// point of it being one function each is that "refuse to apply" and "refuse to
// execute" cannot come to mean two different things.
//
// ADR 0020 §4 is the argument. Subresource Integrity is the only mechanism in
// this browser that protects the user against the site's *own* CDN, which is a
// threat the site chose to defend against and that we would otherwise silently
// discard.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "csp/SubresourceIntegrity.h"
#include "engine/Clock.h"
#include "engine/Engine.h"
#include "engine/Frames.h"
#include "url/Origin.h"
#include "url/Url.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How many times one turn will re-collect the frame tree after dispatching `load`. See
// Engine::ProcessDynamicFrames -- a handler that navigates its own frame is a loop a page writes.
constexpr int kMaxFrameLoadPasses = 8;

// The interpreter a page's script runs in, built if it has none yet. Asked of a *parent* when a
// same-origin child needs a realm: a parent with no script of its own still has to supply the heap
// its child's objects live in, because a child allocating from an interpreter nobody else held
// would hand its parent pointers into a heap that dies with it.
//
// A free function over the page's public surface rather than a method on it, because everything it
// needs -- the document and the address it was loaded from -- is already public, and Page.h is at
// the module's line cap.
js::Interpreter* EnsureInterpreterOf(Page& page) {
  dom::Document* document = page.MutableDocument();
  if (document == nullptr) {
    return nullptr;  // a frame whose response has not arrived
  }
  return &page.ScriptHalf()->EnsureHostInterpreter(*document, page.Url(), NowMilliseconds());
}

Frame* FrameForPage(Page& root, Page* target) {
  for (Frame& frame : root.MutableFrames().MutableFrames()) {
    if (frame.page.get() == target) {
      return &frame;
    }
    if (frame.page != nullptr) {
      if (Frame* nested = FrameForPage(*frame.page, target)) {
        return nested;
      }
    }
  }
  return nullptr;
}

}  // namespace

std::optional<net::FetchOptions> Engine::OptionsForSubresource(
    const SubresourceRequest& request, const url::Url* document_base) const {
  net::FetchOptions options;
  // After the navigation finishes, `load_` is cleared — post-load scripts
  // (youtube's PLAYER_JS_URL / base.js on SPA watch) still need a document
  // URL for CORS and relative resolution. Same fallback as images.
  options.bypass_cache = load_.active && load_.bypass_cache;
  std::optional<url::Url> parsed;
  const url::Url* document = document_base;
  if (document == nullptr) {
    document = load_.active && load_.base.has_value() ? &*load_.base : nullptr;
  }
  if (document == nullptr) {
    parsed = page_.BaseUrl();
    if (!parsed.has_value()) {
      parsed = url::Url::Parse(page_.Url());
    }
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    document = &*parsed;
  }

  if (!request.cross_origin.has_value()) {
    const std::optional<url::Url> target = url::Url::Parse(request.url, *document);
    const bool cross_origin =
        target.has_value() &&
        !url::Origin::FromUrl(*target).IsSameOrigin(url::Origin::FromUrl(*document));
    if (cross_origin && csp::HasIntegrityMetadata(request.integrity)) {
      // ADR 0020 §4: `integrity` on a cross-origin resource requires
      // `crossorigin`. Refused rather than fetched and left unchecked, because
      // an integrity check over bytes the page may not read is an oracle for
      // them -- one guess per reload. Plex sets both, which is why it is the
      // site this rule was measured against.
      return std::nullopt;
    }
    return options;
  }
  // `crossorigin` present: this is a CORS request, and `use-credentials` is the
  // only value that sends any. `crossorigin=""` is `anonymous`, which is why the
  // attribute is an optional rather than a string.
  options.cors.mode = net::RequestMode::Cors;
  options.cors.credentials =
      util::EqualsAsciiCaseInsensitive(*request.cross_origin, "use-credentials")
          ? net::CredentialsMode::Include
          : net::CredentialsMode::Omit;
  options.cors.origin = url::Origin::FromUrl(*document);
  return options;
}

// Whether the bytes are the ones the document said they would be.
//
// True when the element named no usable hash, which is what an absent
// `integrity` means and also what an unreadable one means: an attribute this
// browser cannot parse must not be stricter than no attribute at all, or a typo
// takes a site offline.
bool Engine::IntegrityHolds(const std::vector<SubresourceRequest>& requests, std::size_t index,
                            std::string_view body) {
  if (index >= requests.size() || requests[index].integrity.empty()) {
    return true;
  }
  AddPerformanceCounter(PerfCounterId::SriChecks);
  switch (csp::CheckIntegrity(requests[index].integrity, body)) {
    case csp::IntegrityResult::Match:
      return true;
    case csp::IntegrityResult::NoMetadata:
      AddPerformanceCounter(PerfCounterId::SriUnparseable);
      return true;
    case csp::IntegrityResult::Mismatch:
      break;
  }
  AddPerformanceCounter(PerfCounterId::SriMismatches);
  return false;
}

void Engine::StartSubresources() {
  const url::Url& document = *load_.base;

  // All at once. The order they are *started* in is document order and stays
  // deterministic; the order they arrive in is the network's business and this
  // engine must not have an opinion about it, which is what the slot-filling
  // below is for.
  const std::vector<SubresourceRequest>& sheets = page_.PendingStyleSheets();
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    const std::optional<net::FetchOptions> options = OptionsForSubresource(sheets[i]);
    if (!options.has_value()) {
      continue;
    }
    const Loader::RequestId id = loader_.StartSubresource(
        sheets[i].url, document, privacy::ResourceType::Stylesheet, NowSeconds(), *options);
    load_.resources[id] = PendingResource{ResourceKind::StyleSheet, i, {}};
    ++load_.sheets_outstanding;
  }

  const std::vector<SubresourceRequest>& scripts = page_.ScriptHalf()->PendingUrls();
  for (std::size_t i = 0; i < scripts.size(); ++i) {
    const std::optional<net::FetchOptions> options = OptionsForSubresource(scripts[i]);
    if (!options.has_value()) {
      continue;
    }
    const Loader::RequestId id = loader_.StartSubresource(
        scripts[i].url, document, privacy::ResourceType::Script, NowSeconds(), *options);
    load_.resources[id] = PendingResource{ResourceKind::Script, i, {}};
    ++(page_.ScriptHalf()->IsAsync(i) ? load_.async_scripts_outstanding
                                     : load_.scripts_outstanding);
  }

  StartImageRequests();
  StartFontRequests();
  StartFrameRequests();
  StartWorkerScriptRequests();

  page_.ScriptHalf()->MarkScriptsRequested();
  load_.total_resources = load_.resources.size();
}

void Engine::StartFrameRequests() {
  // ADR 0027 §1. A frame is fetched like any other subresource -- through the privacy verdict and
  // the request queue, bounded per partition key -- and the key is ADR 0005's, whose *top-level
  // site is the embedder's*. That is what makes a third-party frame unable to see the cookies its
  // own site set elsewhere, and it costs nothing here because `StartSubresource` already keys by
  // the page's partition. ADR 0027 §4 says plainly that this will look like a bug and is not.
  std::optional<url::Url> parsed;
  const url::Url* document = load_.active && load_.base.has_value() ? &*load_.base : nullptr;
  if (document == nullptr) {
    parsed = page_.BaseUrl();
    if (!parsed.has_value()) {
      parsed = url::Url::Parse(page_.Url());
    }
    if (!parsed.has_value()) {
      return;
    }
    document = &*parsed;
  }
  FrameTree& tree = page_.MutableFrames();
  std::vector<Frame>& frames = tree.MutableFrames();
  for (std::size_t index = 0; index < frames.size(); ++index) {
    Frame& frame = frames[index];
    if (frame.loaded || frame.requested || frame.element == nullptr) {
      continue;
    }
    // **`srcdoc` beats `src`**, which is what HTML says and what makes it worth handling here
    // rather than at a URL: the markup is already in hand, so there is no request, and the child
    // inherits the embedder's origin exactly as `about:blank` does. It is also how most of WPT
    // builds a second document -- an `<iframe srcdoc>` needs no server round trip and no separate
    // file, so a suite that avoided it would be avoiding the tests rather than the feature.
    if (const std::string* srcdoc = frame.element->GetAttribute("srcdoc"); srcdoc != nullptr) {
      tree.MarkRequested(index);
      // The *embedder's* URL, because that is what a `srcdoc` document's relative URLs resolve
      // against and what its origin is. Not `about:srcdoc`: this browser has no scheme for it and
      // a URL that parses to nothing would take the base with it.
      frame.url = page_.Url();
      tree.SetDocument(index, *srcdoc, frame.url, csp::PolicyList{}, "text/html", true);
      continue;
    }
    const std::string* src = frame.element->GetAttribute("src");
    if (src == nullptr || src->empty()) {
      // No `src` is `about:blank`, and **an `about:blank` frame inherits its embedder's origin** --
      // the one place a document's origin is not derived from its URL. ADR 0027's consequences list
      // names it as the source of a recurring vulnerability class, so it is decided here once
      // rather than derived at each caller: same-origin by construction, and no request.
      tree.MarkRequested(index);
      frame.url = "about:blank";
      tree.SetDocument(index, std::string_view(), frame.url, csp::PolicyList{}, std::string_view(),
                       true);
      continue;
    }
    // `frame-src` decides before the request rather than after the response, which is where every
    // other CSP enforcement point in this engine sits (ADR 0020 §3). A refused frame is an empty
    // frame rather than an error page -- the same answer ADR 0027 §4 gives for a blocked one.
    if (!page_.Policy().AllowsUrl(csp::Directive::Frame, *src)) {
      tree.MarkRequested(index);
      frame.loaded = true;  // it will never arrive, and `load` must not wait for it
      continue;
    }
    SubresourceRequest request;
    request.url = *src;
    const std::optional<net::FetchOptions> options = OptionsForSubresource(request);
    if (!options.has_value()) {
      tree.MarkRequested(index);
      frame.loaded = true;
      continue;
    }
    const Loader::RequestId id = loader_.StartSubresource(
        *src, *document, privacy::ResourceType::Subdocument, NowSeconds(), *options);
    tree.MarkRequested(index);
    if (load_.active) {
      load_.resources[id] = PendingResource{ResourceKind::Frame, index, *src};
      ++load_.frames_outstanding;
    } else {
      // A frame a script created after the load was over. It goes on the post-load table for the
      // reason a late image and a `fetch` do: `load_` owns the navigation's requests and is torn
      // down with it, so an id filed there would be dropped when the answer came back -- which is
      // every `document.body.appendChild(iframe)` a page performs, since a page that appends one
      // during its own load is the rare case rather than the common one.
      post_load_.frames[id] = index;
    }
  }
}

bool Engine::SubmitIntoNamedFrame(std::string_view name, const std::string& url) {
  if (name.empty() || name == "_self" || name == "_parent" || name == "_top" ||
      name == "_blank") {
    return false;
  }
  for (Frame& frame : page_.MutableFrames().MutableFrames()) {
    if (frame.element == nullptr) {
      continue;
    }
    const std::string* frame_name = frame.element->GetAttribute("name");
    const std::string* frame_id = frame.element->GetAttribute("id");
    if ((frame_name == nullptr || *frame_name != name) &&
        (frame_id == nullptr || *frame_id != name)) {
      continue;
    }
    frame.element->SetAttribute("src", url);
    page_.CollectFrames();
    StartFrameRequests();
    return true;
  }
  return false;
}

bool Engine::OnFrameFetch(Loader::Completion completion, const PendingResource& resource) {
  if (load_.frames_outstanding > 0) {
    --load_.frames_outstanding;
  }
  FrameTree& tree = page_.MutableFrames();
  std::vector<Frame>& frames = tree.MutableFrames();
  if (resource.index >= frames.size()) {
    return false;
  }
  // **The origin check, and it happens here because this is the module that understands URLs.**
  // `src/bindings` may not see `src/url` and `engine::Page` does not compare origins either; what
  // `Page` is told is a decision rather than a pair of URLs. A cross-origin child gets its document
  // loaded and laid out and never attached to its element, so `iframe.contentDocument` has nothing
  // to return rather than something a future caller has to remember to guard. ADR 0027 §2.
  const std::string final_url =
      completion.result.final_url.empty() ? resource.src : completion.result.final_url;
  const std::optional<url::Url> child = url::Url::Parse(final_url);
  const bool same_origin = child.has_value() && page_.Policy().IsSameOrigin(*child);
  if (!completion.result.ok) {
    // **A frame that does not load still fires `load`**, and gets an empty document rather than
    // none. That is what every browser does -- a 404 in an `<iframe>` is a *rendered* error
    // document, not a failed subresource -- and the difference is not cosmetic: a page that waits
    // on `onload` before reading `contentDocument` hangs forever otherwise, which is a hang a
    // server the page does not control gets to cause.
    tree.SetDocument(resource.index, std::string_view(), final_url, csp::PolicyList{}, "text/html",
                     same_origin);
    StartPendingScriptRequests();
    AddPerformanceCounter(PerfCounterId::EngineFramesFailed);
    return false;
  }
  tree.SetDocument(resource.index, completion.result.body, final_url, csp::PolicyList{},
                   completion.result.content_type, same_origin);
  StartPendingScriptRequests();
  return true;
}

void Engine::StartPendingScriptRequests() {
  std::optional<url::Url> parsed;
  const url::Url* document = load_.active && load_.base.has_value() ? &*load_.base : nullptr;
  if (document == nullptr) {
    parsed = page_.BaseUrl();
    if (!parsed.has_value()) {
      parsed = url::Url::Parse(page_.Url());
    }
  }
  if (document == nullptr && parsed.has_value()) {
    document = &*parsed;
  }
  if (document != nullptr) {
    const std::size_t first_index = page_.ScriptHalf()->PendingUrls().size();
    std::vector<SubresourceRequest> pending = page_.ScriptHalf()->TakeUnrequestedScripts();
    if (!pending.empty()) {
      const std::size_t base_index = first_index - pending.size();
      for (std::size_t i = 0; i < pending.size(); ++i) {
        const std::size_t index = base_index + i;
        const std::optional<net::FetchOptions> options = OptionsForSubresource(pending[i]);
        if (!options.has_value()) {
          // Refused before the wire (integrity without crossorigin, or no document
          // URL). YouTube's player loader waits on `load` only — a silent skip
          // leaves At7 hanging forever (TD-0024). Fire `error` so the page can
          // fail closed rather than wait.
          page_.ScriptHalf()->NotifyFetchFailed(index);
          AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
          continue;
        }
        const Loader::RequestId id = loader_.StartSubresource(
            pending[i].url, *document, privacy::ResourceType::Script, NowSeconds(), *options);
        if (load_.active) {
          load_.resources[id] = PendingResource{ResourceKind::Script, index, {}};
          ++load_.total_resources;
          ++(page_.ScriptHalf()->IsAsync(index) ? load_.async_scripts_outstanding
                                               : load_.scripts_outstanding);
        } else {
          post_load_.scripts[id] = index;
        }
      }
    }
  }

  // A child's `<script src>` is collected when its document is parsed and was never requested:
  // StartPendingScriptRequests only looked at the embedder, so Range-test-iframe.html's
  // common.js never left the slot. Relative URLs resolve against the *child*.
  const auto start_child = [&](auto&& self, Page& parent) -> void {
    for (Frame& frame : parent.MutableFrames().MutableFrames()) {
      if (frame.page == nullptr || !frame.loaded) {
        continue;
      }
      std::optional<url::Url> child_parsed = frame.page->BaseUrl();
      if (!child_parsed.has_value()) {
        child_parsed = url::Url::Parse(frame.page->Url());
      }
      if (child_parsed.has_value()) {
        const std::size_t first_index = frame.page->ScriptHalf()->PendingUrls().size();
        std::vector<SubresourceRequest> pending =
            frame.page->ScriptHalf()->TakeUnrequestedScripts();
        if (!pending.empty()) {
          const std::size_t base_index = first_index - pending.size();
          for (std::size_t i = 0; i < pending.size(); ++i) {
            const std::size_t index = base_index + i;
            const std::optional<net::FetchOptions> options =
                OptionsForSubresource(pending[i], &*child_parsed);
            if (!options.has_value()) {
              frame.page->ScriptHalf()->NotifyFetchFailed(index);
              AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
              continue;
            }
            const Loader::RequestId id = loader_.StartSubresource(
                pending[i].url, *child_parsed, privacy::ResourceType::Script, NowSeconds(),
                *options);
            post_load_.frame_scripts[id] = {frame.page.get(), index};
            ++frame.scripts_outstanding;
          }
        }
      }
      self(self, *frame.page);
    }
  };
  start_child(start_child, page_);
}

bool Engine::OnLateScript(Loader::Completion completion) {
  const auto found = post_load_.scripts.find(completion.id);
  if (found == post_load_.scripts.end()) {
    return false;
  }
  const std::size_t index = found->second;
  post_load_.scripts.erase(found);
  if (!completion.result.ok ||
      !IntegrityHolds(page_.ScriptHalf()->PendingUrls(), index, completion.result.body)) {
    AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
    page_.ScriptHalf()->NotifyFetchFailed(index);
    return true;
  }
  page_.AddScript(index, std::move(completion.result.body));
  AddPerformanceCounter(PerfCounterId::EngineScriptsLoaded);
  if (ProcessDynamicScripts()) {
    return true;
  }
  page_.InvalidateLayout();
  LayoutAndPaint();
  return true;
}

bool Engine::OnLateFrame(Loader::Completion& completion) {
  const auto found = post_load_.frames.find(completion.id);
  if (found == post_load_.frames.end()) {
    return false;
  }
  // The `src` is left empty rather than stored twice: `OnFrameFetch` reads it only as a fallback
  // for a response that carried no final URL, and a post-load frame always carries one.
  const PendingResource resource{ResourceKind::Frame, found->second, std::string()};
  post_load_.frames.erase(found);
  (void)OnFrameFetch(std::move(completion), resource);
  // The child arrived and its `load` is owed, but the event is *not* dispatched here:
  // `ProcessDynamicFrames` does that, once, after the whole completion batch. A handler runs
  // script, and running script from inside the drain would re-enter it.
  return true;
}

bool Engine::DrainCompletionBatch(bool& moved) {
  std::vector<Loader::Completion> completions = loader_.TakeCompletions();
  bool font_changed = false;
  for (Loader::Completion& completion : completions) {
    if (post_load_.images.find(completion.id) != post_load_.images.end()) {
      moved = OnLateImage(std::move(completion)) || moved;
      continue;
    }
    if (post_load_.scripts.find(completion.id) != post_load_.scripts.end()) {
      if (OnLateScript(std::move(completion))) {
        moved = true;
        if (FollowScriptNavigation()) {
          return true;
        }
      }
      continue;
    }
    if (const auto found = post_load_.frame_scripts.find(completion.id);
        found != post_load_.frame_scripts.end()) {
      Page* child = found->second.first;
      const std::size_t index = found->second.second;
      post_load_.frame_scripts.erase(found);
      Frame* frame = child != nullptr ? FrameForPage(page_, child) : nullptr;
      if (frame != nullptr && frame->scripts_outstanding > 0) {
        --frame->scripts_outstanding;
      }
      if (child != nullptr && frame != nullptr) {
        if (!completion.result.ok ||
            !IntegrityHolds(child->ScriptHalf()->PendingUrls(), index, completion.result.body)) {
          child->ScriptHalf()->NotifyFetchFailed(index);
          AddPerformanceCounter(PerfCounterId::EngineScriptsFailed);
        } else {
          child->AddScript(index, std::move(completion.result.body));
          AddPerformanceCounter(PerfCounterId::EngineScriptsLoaded);
        }
      }
      moved = true;
      continue;
    }
    if (OnLateFrame(completion)) {
      moved = true;
      continue;
    }
    // All three worker tables behind one question -- a worker script that starts a thread, an
    // `importScripts` that unblocks one, and a worker's own `fetch` -- because `OnWorkerFetch` is
    // the router for them and asking here as well would be the same decision written twice.
    if (worker_fetches_.count(completion.id) + worker_import_fetches_.count(completion.id) +
            worker_script_fetches_.count(completion.id) >
        0) {
      moved = OnWorkerFetch(std::move(completion)) || moved;
      continue;
    }
    if (font_fetches_.find(completion.id) != font_fetches_.end()) {
      if (OnFontFetch(std::move(completion))) {
        moved = true;
        font_changed = true;
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
  if (font_changed) {
    LayoutAndPaint();
  }
  // Child documents that arrived in this batch are owed a `load`. After the completions rather
  // than inside the loop, so a page whose four frames answer together sees four events from one
  // turn instead of re-entering the drain four times.
  bool frame_navigated = false;
  if (SettleFrameLoads(frame_navigated)) {
    if (frame_navigated) {
      return true;
    }
    moved = true;
  }
  moved = moved || !completions.empty();
  return false;
}

bool Engine::SettleFrameLoads(bool& navigated) {
  navigated = false;
  if (!ProcessDynamicFrames()) {
    return false;
  }
  // A `load` handler is a script turn like any other: it can navigate, and a navigation from
  // inside one leaves every id in the caller's batch belonging to a document that is gone. Which
  // is why this reports it rather than carrying on to lay out a page that no longer exists.
  if (FollowScriptNavigation()) {
    navigated = true;
    return true;
  }
  page_.InvalidateLayout();
  LayoutAndPaint();
  return true;
}

bool Engine::ProcessDynamicFrames() {
  // **Nothing before the document's own scripts have run.** An `<iframe srcdoc>` is handed its
  // document synchronously in the subresource pass, which is over before the parser's scripts are;
  // dispatching its `load` from the completion drain that follows would fire the event before the
  // line that assigns the handler had executed. The same guard `ProcessDynamicScripts` uses, and
  // for the same reason: this is the point after which the page can be said to exist.
  if (!load_.scripts_ran && !post_load_.document_interactive) {
    return false;
  }
  bool dispatched = false;
  // **A loop, because a `load` handler is where the next frame comes from.** The pattern the whole
  // suite is written in is `a.onload = () => { b.src = ... }`, so collecting once and dispatching
  // once leaves the second navigation sitting in the document with nothing to notice it -- and
  // nothing else will, since an idle page with no outstanding request never reaches this function
  // again. Each turn of this loop either starts a request or has nothing left to do.
  //
  // Bounded, and the bound is not a formality: `f.onload = () => { f.src = f.src === a ? b : a }`
  // is a page ping-ponging one frame between two URLs, and every iteration would be a request to
  // whoever they point at. Past the bound the work is left for the next turn of the event loop,
  // which is where a runaway page belongs -- it keeps the browser responsive and it keeps the
  // requests paced by the loop rather than by the page.
  for (int pass = 0; pass < kMaxFrameLoadPasses; ++pass) {
    // A script that appended an `<iframe>` created a browsing context the same way the parser does,
    // and until this existed it created a box with nothing in it. The walk is gated inside
    // `Page::CollectFrames`, so a page that touched neither its tree nor any `src` pays a handful
    // of integer comparisons per turn -- which is the whole reason this can be called from a path
    // that runs on every turn at all (see TD-0021 for the shape it would otherwise be).
    page_.CollectFrames();
    StartFrameRequests();
    StartPendingScriptRequests();
    // **Before the `load` events below, and that ordering is the specification's.** A child's own
    // scripts run as its document is processed; the `load` its element fires in the embedder is
    // what says they are finished. A page that reads `frame.contentWindow.result` from `onload` --
    // which is most of how the suite gets an answer out of a frame -- sees nothing at all if these
    // two are the other way round.
    RunFrameScripts(page_, /*run_scripts=*/true);
    // And the `load` events owed to frames whose documents have arrived. Dispatched here rather
    // than where the document was set, because this is a *task* boundary: `iframe.onload = f` runs
    // after `appendChild` in the source, and a synchronous dispatch inside the collection pass
    // would fire before the assignment. Every `assert_unreached` in the suite's iframe tests is
    // that ordering.
    if (!page_.DispatchPendingFrameLoads()) {
      break;
    }
    dispatched = true;
  }
  return dispatched;
}

bool Engine::RunFrameDueWork(Page& parent, std::int64_t now_ms) {
  bool ran = false;
  for (Frame& frame : parent.MutableFrames().MutableFrames()) {
    if (frame.page == nullptr || !frame.loaded) {
      continue;
    }
    // A child's timers and animation frames are its own, and nothing else was turning them. A
    // `setTimeout` in a frame that never fires is the same class of bug as a script that never
    // runs, one turn of the loop later.
    ran = frame.page->RunDueWork(now_ms) != Page::DueWorkKind::None || ran;
    bool child_changed = false;
    std::optional<std::string> child_href;
    (void)frame.page->ApplyScriptActivation(child_changed, child_href);
    ran = RunFrameDueWork(*frame.page, now_ms) || ran;
  }
  return ran;
}

void Engine::SettleFrameContexts() {
  page_.CollectFrames();
  StartFrameRequests();
  // `run_scripts=false` is the whole point: a context and a window, and nothing that runs. A
  // child's own scripts are a task boundary away, and so is the `load` its element owes.
  RunFrameScripts(page_, /*run_scripts=*/false);
}

bool Engine::RunFrameScripts(Page& parent, bool run_scripts, js::Object* top_window) {
  bool ran = false;
  bool tree_changed = false;
  for (Frame& frame : parent.MutableFrames().MutableFrames()) {
    if (frame.page == nullptr || !frame.loaded) {
      continue;
    }
    if (!frame.scripting_attached) {
      frame.scripting_attached = true;  // once, whatever the answer below is; see Frames.h
      if (frame.same_origin) {
        // **A realm of the embedder's interpreter, and only for a same-origin child.** They hand
        // each other live objects -- `parent.document.body` from the frame is the embedder's node,
        // not a copy -- so they have to share a heap, and an object from one heap reached from
        // another is a use-after-free waiting for the first collection (ADR 0042 §3). A
        // cross-origin child falls through to the branch below and builds its own interpreter,
        // which *is* the isolation rather than a check on top of it.
        js::Interpreter* host = EnsureInterpreterOf(parent);
        if (host == nullptr) {
          continue;
        }
        const std::optional<js::RealmId> realm = host->CreateRealm();
        if (!realm.has_value()) {
          // Past `kMaxRealms`. The frame simply does not run script -- there is nothing to tear
          // down and nothing to report, because the count is page-controlled and a page that made
          // sixty-four scripted frames is outside everything in ADR 0007.
          continue;
        }
        frame.page->ScriptHalf()->AttachToRealm(*host, *realm);
      }
    }
    // **The relations before the script, and both before `RunScripts`.** A child's first line can
    // read `parent` and `top`, so the answer has to be on its global before its interpreter has
    // evaluated anything -- which is why `AttachScriptRealm` is not enough on its own and why
    // `EnsureScriptInterpreter` on the child is called here rather than left to `RunScripts`.
    js::Object* child_window = nullptr;
    if (frame.same_origin) {
      if (EnsureInterpreterOf(*frame.page) != nullptr) {
        child_window = frame.page->ScriptHalf()->Global();
      }
    }
    if (child_window != nullptr) {
      js::Object* parent_window = parent.ScriptHalf()->Global();
      // `top` for a child of the top-level document is its parent, which is why the caller passes
      // null and this passes its own window down: the root of the tree is the first window in it,
      // not the nearest one.
      frame.page->ScriptHalf()->FrameWindows().SetEmbedder(
          parent_window, top_window != nullptr ? top_window : parent_window);
      parent.ScriptHalf()->FrameWindows().SetNested(frame.element, child_window);
      tree_changed = true;
    }
    if (tree_changed) {
      // Published before this child runs, so `window.length` is already right for it -- and
      // republished after every child for the same reason, since a later frame is a later index
      // and a script already running would otherwise see the list grow underneath it.
      parent.ScriptHalf()->PublishFrameWindows();
      frame.page->ScriptHalf()->PublishFrameWindows();
    }
    if (!run_scripts) {
      continue;  // the window exists; its document's scripts run at the point below
    }
    if (frame.scripts_outstanding > 0 ||
        frame.page->ScriptHalf()->HasOutstandingScriptFetches()) {
      // The document is here; its `<script src>` is not. Running now would skip the helper
      // file and execute the inline script after it, which is how setupRangeTests stayed
      // undefined while the iframe's load had already fired.
      continue;
    }
    frame.page->RunScripts(NowMilliseconds());
    // **What a child's script asked the browser to do, carried out.** A page that only *ran* a
    // child's script and never drained its queues is worse than one that ran none: the child's
    // `element.click()` records an activation nobody performs, so a promise waiting on the
    // resulting event never settles and the whole test times out rather than failing. Found that
    // way, in `fetch/security/dangling-markup/`.
    //
    // The submission it can produce is deliberately **dropped**: submitting a form inside a frame
    // navigates that frame, and a child navigation the engine drives is not built yet (it is the
    // next thing this file owes). What the drain does deliver is the `submit` event itself, which
    // is what a page listens for -- so the answer is wrong rather than absent, and a wrong answer
    // is visible.
    bool child_changed = false;
    std::optional<std::string> child_href;
    (void)frame.page->ApplyScriptActivation(child_changed, child_href);
    ran = true;
    // A frame inside a frame, and it recurses rather than iterating a flat list because the realm
    // a grandchild borrows is *its* parent's -- which for a same-origin chain is the same
    // interpreter all the way up, and for a chain with a cross-origin link in it is not. `top`
    // travels down unchanged, because it is the root of the whole tree rather than of this level.
    ran = RunFrameScripts(*frame.page, run_scripts,
                          top_window != nullptr ? top_window
                                                : parent.ScriptHalf()->Global()) ||
          ran;
  }
  return ran;
}

bool Engine::ProcessDynamicScripts() {
  if (!load_.scripts_ran && !post_load_.document_interactive) {
    return false;
  }
  bool changed = ProcessDynamicFrames();
  while (page_.CollectInsertedScripts()) {
    changed = true;
    StartPendingScriptRequests();
    DrainReadyLoaderCompletions();
  }
  if (AdvanceModules()) {
    changed = true;
    if (FollowScriptNavigation()) {
      return true;
    }
  }
  if (load_.modules_outstanding > 0 || load_.scripts_outstanding > 0) {
    if (changed) {
      page_.InvalidateLayout();
      LayoutAndPaint();
    }
    return changed;
  }
  if (page_.ScriptHalf()->RunPendingScripts()) {
    if (FollowScriptNavigation()) {
      return true;
    }
    // Scripts ran but the recursive pass may find nothing left to do and return
    // false without painting -- which left reddit's feed hoisted in the DOM while
    // the last IPC frame was still chrome-only (Gate B / TD-0016).
    (void)ProcessDynamicScripts();
    page_.InvalidateLayout();
    LayoutAndPaint();
    return true;
  }
  if (changed) {
    page_.InvalidateLayout();
    LayoutAndPaint();
  }
  return changed;
}

void Engine::DrainReadyLoaderCompletions() {
  std::vector<Loader::Completion> completions = loader_.TakeCompletions();
  for (Loader::Completion& completion : completions) {
    if (!load_.active) {
      return;
    }
    OnCompletion(std::move(completion));
    if (!load_.active) {
      return;
    }
  }
}

void Engine::StartFontRequests() {
  if (!page_.BaseUrl().has_value()) {
    return;
  }
  for (const Page::PendingFontFace& face : page_.TakeUnrequestedFontFaces()) {
    const Loader::RequestId id = loader_.StartSubresource(
        face.url, *page_.BaseUrl(), privacy::ResourceType::Font, NowSeconds(), {});
    font_fetches_[id] = face;
  }
}

void Engine::StartWorkerScriptRequests() {
  if (!page_.BaseUrl().has_value()) {
    return;
  }
  for (const Page::PendingWorkerScript& pending : page_.TakeUnrequestedWorkerScripts()) {
    const Loader::RequestId id = loader_.StartSubresource(
        pending.url, *page_.BaseUrl(), privacy::ResourceType::Script, NowSeconds(), {});
    if (id == 0) {
      // Refused before it went out -- the privacy layer, or a URL the loader would not take. The page
      // hears about it as an `error` event, which is the same thing a 404 produces: a page does not need
      // to know *why* its worker script did not arrive, only that it did not.
      page_.FailWorkerLoad(pending.worker_id, "the worker script could not be requested");
      continue;
    }
    worker_fetches_[id] = pending.worker_id;
  }
}

// Called from `Advance` as well as from `RunDueWork`, and the difference is not tidiness: both loops
// that drive this engine are shaped `if (Advance() || HasRunnableWork()) continue;` with `RunDueWork`
// only on the else. A worker blocked in `importScripts` makes `HasRunnableWork` true, so the request
// that would unblock it was never issued -- the loop span at full speed until its deadline while the
// worker slept, and every one of the suite's 1,763 worker tests reported nothing.
void Engine::StartWorkerImportRequests() {
  for (const Page::PendingWorkerImport& pending : page_.TakeWorkerImportRequests()) {
    // Resolved against the **worker's own script URL**, which is what the specification says
    // `importScripts` uses as its base -- not the document's. A worker at `/a/b/w.js` importing
    // `helper.js` means `/a/helper.js`, and a document-relative resolution would silently fetch the
    // wrong file whenever the two directories differ.
    const std::optional<url::Url> base = url::Url::Parse(pending.base_url);
    if (!base.has_value()) {
      page_.CompleteWorkerImport(pending.worker_id, false, std::string());
      continue;
    }
    const Loader::RequestId id = loader_.StartSubresource(
        pending.specifier, *base, privacy::ResourceType::Script, NowSeconds(), {});
    if (id == 0) {
      // Refused before it went out -- the privacy layer, or a URL the loader would not take. The
      // worker's `importScripts` throws a `NetworkError`, which is the specification's answer for a
      // script that could not be fetched and is indistinguishable from a 404 by design.
      page_.CompleteWorkerImport(pending.worker_id, false, std::string());
      continue;
    }
    worker_import_fetches_[id] = pending.worker_id;
  }
}

// One entry point for both, because the router upstream asks one question -- "is this completion a
// worker's?" -- and the two answers are opposite sides of the same feature: a worker script *starts* a
// thread and an `importScripts` *unblocks* one.
bool Engine::OnWorkerFetch(Loader::Completion completion) {
  if (worker_fetches_.count(completion.id) != 0) {
    return OnWorkerScriptFetch(std::move(completion));
  }
  if (worker_script_fetches_.count(completion.id) != 0) {
    return OnWorkerScriptRequestFetch(std::move(completion));
  }
  const auto found = worker_import_fetches_.find(completion.id);
  if (found == worker_import_fetches_.end()) {
    return false;
  }
  const std::uint64_t worker_id = found->second;
  worker_import_fetches_.erase(found);
  // **A worker thread is blocked on this call**, so every path out of here must answer. An empty body
  // is an answer: a zero-length script is a legal one, and refusing it would hang the worker on a file
  // that simply had nothing in it.
  //
  // The *status* is checked and not only `ok`, and it is the difference between a diagnosis and a
  // riddle: `ok` is "bytes arrived", and a 404's bytes are an error page. Running one as script
  // reports `SyntaxError: expected ';'` from inside `importScripts`, which names neither the file nor
  // the fact that it was never there. The specification's answer for a non-2xx is a `NetworkError`.
  const bool ok = completion.result.ok && completion.result.status >= 200 &&
                  completion.result.status < 300;
  page_.CompleteWorkerImport(worker_id, ok, std::move(completion.result.body));
  return true;
}

bool Engine::OnWorkerScriptFetch(Loader::Completion completion) {
  const auto found = worker_fetches_.find(completion.id);
  if (found == worker_fetches_.end()) {
    return false;
  }
  const std::uint64_t worker_id = found->second;
  worker_fetches_.erase(found);
  if (!completion.result.ok || completion.result.body.empty()) {
    page_.FailWorkerLoad(worker_id, "the worker script failed to load");
    return true;
  }
  // The thread starts here, and anything the page queued while the fetch was in flight is drained on the
  // worker's first loop iteration -- which is why the inbox exists from `Reserve` rather than from here.
  page_.ProvideWorkerScript(worker_id, std::move(completion.result.body));
  return true;
}

bool Engine::OnFontFetch(Loader::Completion completion) {
  const auto found = font_fetches_.find(completion.id);
  if (found == font_fetches_.end()) {
    return false;
  }
  const Page::PendingFontFace face = found->second;
  font_fetches_.erase(found);
  if (!completion.result.ok || completion.result.body.empty()) {
    AddPerformanceCounter(PerfCounterId::GfxWebFontsRefused);
    return false;
  }
  std::vector<std::byte> bytes;
  bytes.reserve(completion.result.body.size());
  for (const char byte : completion.result.body) {
    bytes.push_back(static_cast<std::byte>(byte));
  }
  // True only when the provider took them. A refused face is not an error: the
  // page renders in the next family of its stack, which is what a stack is for.
  return page_.AddWebFont(face, std::move(bytes));
}

}  // namespace microbrowser::engine

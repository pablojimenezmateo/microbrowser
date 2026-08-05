// Session history, where the documents are.
//
// Its own translation unit for the reason EngineFetch.cpp is: Engine.cpp is at
// its module cap, and the seam is real -- everything in this file is one
// question, *is this traversal a load or a paint*, and the answer is a
// comparison of document ids rather than of URLs. `pushState('/a')` twice on one
// document and two loads of `/a` look identical as URLs and are nothing alike.
//
// ADR 0026 §1-3. Three rules here are the whole of it:
//
//   * **The origin check is this file's, not the binding layer's.** `src/bindings`
//     may not see `url`, so `PushHistoryState` takes the URL as the page wrote it
//     and answers with a decision. One origin comparison, in the module that owns
//     URLs, and the binding turns a refusal into a `SecurityError`.
//   * **A traversal a script asked for is recorded, not performed.** It can
//     replace the document, and doing that with the interpreter on the stack is
//     the use-after-free ADR 0026 §3 is written to prevent -- the same reason a
//     form submission from script waits for the turn boundary.
//   * **`popstate` never fires on the initial load.** Only this side knows
//     whether a document is new, which is why the rule lives here and not in the
//     binding that fires the event.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "engine/Engine.h"
#include "js/StructuredClone.h"
#include "url/Origin.h"
#include "url/Url.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Whether two URLs differ only in their fragment. What makes a navigation a
// `hashchange` rather than a load, and the one case that has always been able to
// move the URL without fetching anything.
bool DiffersOnlyInFragment(const url::Url& a, const url::Url& b) {
  return a.Serialize(/*exclude_fragment=*/true) == b.Serialize(/*exclude_fragment=*/true) &&
         (a.HasFragment() != b.HasFragment() || a.Fragment() != b.Fragment());
}

}  // namespace

std::size_t Engine::HistoryLength() const { return history_.Length(); }

const js::SerializedValue& Engine::HistoryState() const {
  static const js::SerializedValue kNone;
  const HistoryEntry* current = history_.Current();
  return current == nullptr ? kNone : current->state;
}

std::uint64_t Engine::HistoryStateGeneration() const { return history_.StateGeneration(); }

bindings::HistorySource::UrlOutcome Engine::PushHistoryState(const js::SerializedValue& state,
                                                             std::string_view url, bool replace) {
  using Outcome = bindings::HistorySource::UrlOutcome;
  std::string target = page_.Url();
  const std::string previous = target;
  if (!url.empty()) {
    const std::optional<url::Url>& base = page_.BaseUrl();
    if (!base.has_value()) {
      // A `data:` or `about:` document. There is nothing for a relative URL to
      // resolve against, and nothing for `'self'` to name either.
      return Outcome::NotSameOrigin;
    }
    const std::optional<url::Url> resolved = url::Url::Parse(url, *base);
    if (!resolved.has_value()) {
      return Outcome::Unparseable;
    }
    // **The load-bearing line.** Same *origin*, by url::Origin's own comparison:
    // not same-site, not same-host. A different port, a different scheme, a
    // `data:` URL and a `javascript:` URL are all refused by it, and those are
    // the shapes an address-bar spoof is built from. ADR 0026 §2.
    if (!page_.Policy().IsSameOrigin(*resolved)) {
      return Outcome::NotSameOrigin;
    }
    target = resolved->Serialize();
  }

  if (replace) {
    history_.ReplaceState(target, state);
  } else {
    // The scroll offset belongs to the entry being left, not to the new one: a
    // page that pushes an entry and then scrolls has to come back to where it
    // was, which is the whole reason the offset is on the entry.
    history_.SetCurrentScroll(page_.ScrollOffsetY());
    history_.PushState(target, state);
  }
  if (target != previous) {
    page_.UpdateUrl(target);
    // The chrome shows the URL of the document that is displayed -- which after
    // a `pushState` is this URL, and the browser process decides what the bar
    // shows rather than the renderer. ADR 0026 §2.
    endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
    LayoutAndPaint();
  }
  SendHistoryState();
  return Outcome::Ok;
}

void Engine::RequestHistoryTraversal(int delta) {
  // Recorded, never performed. See the note at the top of this file.
  pending_traversal_ += delta;
}

void Engine::SendHistoryState() {
  endpoint_.Send(ipc::HistoryStateMessage{history_.CanGoBack(), history_.CanGoForward()});
}

bool Engine::FollowPendingTraversal() {
  const int delta = pending_traversal_;
  pending_traversal_ = 0;
  return delta != 0 && Traverse(delta);
}

bool Engine::Traverse(int delta) {
  const HistoryEntry* current = history_.Current();
  if (current == nullptr) {
    return false;
  }
  // Where the document being left is, so that coming back lands where it was.
  history_.SetCurrentScroll(page_.ScrollOffsetY());
  const std::uint64_t leaving = current->document;
  const std::string old_url = current->url;

  const HistoryEntry* entry = history_.Go(delta);
  if (entry == nullptr) {
    // Off the end of the list. Nothing moves, which is what `history.go(-100)`
    // does in every browser -- and deliberately not a clamp, because a page that
    // asked to go somewhere that does not exist meant somewhere specific.
    return false;
  }
  AddPerformanceCounter(PerfCounterId::HistoryTraversals);

  if (entry->document != leaving) {
    // A different document: a real navigation. The entry keeps its place in the
    // list, so the load must not push a new one -- which is what
    // `traversing_` says.
    const std::string url = entry->url;
    traversing_ = true;
    Navigate(url);
    SendHistoryState();
    return true;
  }

  // Same document. A paint and two events, and no network at all -- which is the
  // entire reason `document` is on the entry.
  AddPerformanceCounter(PerfCounterId::HistorySameDocumentTraversals);
  const std::string new_url = entry->url;
  const float scroll_y = entry->scroll_y;
  if (new_url != old_url) {
    page_.UpdateUrl(new_url);
    endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  }
  page_.SetScrollOffsetY(scroll_y);
  SendHistoryState();

  // `popstate` first and `hashchange` second, which is the specification's order
  // and the one a router depends on: a handler that re-renders from
  // `event.state` has to run before anything keyed on the fragment.
  page_.NotifyPopState();
  const std::optional<url::Url> before = url::Url::Parse(old_url);
  const std::optional<url::Url> after = url::Url::Parse(new_url);
  if (before.has_value() && after.has_value() && DiffersOnlyInFragment(*before, *after)) {
    page_.NotifyHashChange(old_url, new_url);
  }
  page_.InvalidateLayout();
  LayoutAndPaint();
  return true;
}

bool Engine::NavigateToFragment(const std::string& url) {
  const std::optional<url::Url> current = url::Url::Parse(page_.Url());
  const std::optional<url::Url> target = url::Url::Parse(url);
  if (!current.has_value() || !target.has_value() || !DiffersOnlyInFragment(*current, *target)) {
    return false;
  }
  // A same-document navigation: a new history entry on this document, the
  // fragment applied, and `hashchange`. No request, which is what every
  // in-page anchor on every documentation site depends on.
  history_.SetCurrentScroll(page_.ScrollOffsetY());
  const std::string old_url = page_.Url();
  history_.PushState(target->Serialize(), js::SerializedValue{});
  page_.UpdateUrl(target->Serialize());
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  SendHistoryState();
  page_.NotifyHashChange(old_url, page_.Url());
  page_.InvalidateLayout();
  LayoutAndPaint();
  return true;
}

}  // namespace microbrowser::engine

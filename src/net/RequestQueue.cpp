#include "net/RequestQueue.h"

#include <algorithm>
#include <utility>

#include "util/LoadTimeline.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

RequestQueue::RequestQueue(const privacy::PrivacyPolicy& policy, TransportFactory& transport,
                           CookieJar& cookies, HttpCache& cache)
    : policy_(policy), pool_(transport), cookies_(cookies), cache_(cache) {}

void RequestQueue::SetTransport(TransportFactory& transport) {
  if (!active_.empty() || !queued_.empty()) {
    return;
  }
  pool_.SetFactory(transport);
}

std::size_t RequestQueue::ActiveInPartition(std::string_view partition) const {
  return static_cast<std::size_t>(std::count_if(
      active_.begin(), active_.end(),
      [partition](const Active& active) { return active.partition == partition; }));
}

RequestQueue::Id RequestQueue::Start(privacy::Verdict verdict, const FetchOptions& options,
                                     std::int64_t now) {
  const Id id = next_id_++;
  Enqueue(id, Pending{std::move(verdict), options, now});
  return id;
}

void RequestQueue::Enqueue(Id id, Pending pending) {
  // Dropped here, once, rather than at the point they would have been written.
  // `Fetch` drops them too and that is the unbypassable copy -- but the CORS
  // decision below reads this header list, and a page that could put `Origin`
  // in it would be able to force a preflight for a header that then never
  // appears on the wire. The set a preflight asks about has to be the set the
  // request actually sends.
  DropHeadersOwnedByFetch(pending.options.headers);
  Queued queued;
  queued.id = id;
  queued.partition = pending.verdict.Partition().Serialize();
  queued.pending = std::move(pending);
  queued_.push_back(std::move(queued));
}

bool RequestQueue::NeedsPreflightExchange(Pending& pending) const {
  CorsParams& cors = pending.options.cors;
  // Already paid for. This is the request coming back through the front door
  // after its own preflight was allowed, and without this line it would ask
  // again -- forever, one `OPTIONS` per turn, for any server that granted
  // permission without an `Access-Control-Max-Age` to remember it by.
  if (cors.preflighted) {
    return false;
  }
  if (cors.mode != RequestMode::Cors || IsSameOrigin(cors.origin, pending.verdict.FinalUrl())) {
    return false;
  }
  if (!NeedsPreflight(pending.options.method, pending.options.headers)) {
    return false;
  }
  if (preflights_.Allows(pending.verdict.Partition().Serialize(), cors,
                         pending.verdict.FinalUrl(), pending.options.method,
                         pending.options.headers, pending.now)) {
    // Permission already in hand. Marked preflighted anyway, because what that
    // flag governs is whether a redirect may be followed -- and a grant from
    // the cache names the same one URL the `OPTIONS` did.
    cors.preflighted = true;
    AddPerformanceCounter(PerfCounterId::NetCorsPreflightsCached);
    return false;
  }
  return true;
}

RequestQueue::Pending RequestQueue::PreflightFor(const Pending& real) {
  Pending pre;
  pre.verdict = real.verdict;
  pre.now = real.now;
  pre.options.method = "OPTIONS";
  // A preflight that followed a redirect would be asking one server for
  // permission and giving the answer to another.
  pre.options.max_redirects = 0;
  pre.options.cors = real.options.cors;
  // Never. The whole question a preflight asks is whether the real request is
  // allowed to carry credentials, and asking it with credentials attached would
  // answer it in advance.
  pre.options.cors.credentials = CredentialsMode::Omit;
  pre.options.cors.preflight_method = real.options.method;
  pre.options.cors.preflight_headers = PreflightRequestHeaderList(real.options.headers);
  return pre;
}

bool RequestQueue::OnPreflightComplete(Id id, Pending deferred, const FetchResult& result) {
  // A failure here is reported as the preflight's, not the request's. The two
  // are indistinguishable to script -- which is the point of CORS -- but they
  // are entirely distinguishable to whoever is reading the console, and "the
  // OPTIONS was refused" is the one sentence that explains a request that never
  // appeared on the server it was aimed at.
  const auto refuse = [this, id](std::string reason) {
    FetchResult failed;
    failed.error = std::move(reason);
    completions_.push_back(Completion{id, std::move(failed)});
  };
  if (!result.ok) {
    refuse("CORS preflight failed: " + result.error);
    return false;
  }
  PreflightGrant grant;
  const CorsResult decision =
      CheckPreflight(deferred.options.cors, deferred.options.method, deferred.options.headers,
                     result.final_url, result.response, grant);
  if (!decision.allowed) {
    refuse(decision.error);
    return false;
  }
  preflights_.Store(deferred.verdict.Partition().Serialize(), deferred.options.cors,
                    deferred.verdict.FinalUrl(), grant, deferred.now);
  deferred.options.cors.preflighted = true;
  Enqueue(id, std::move(deferred));
  return true;
}

bool RequestQueue::PromoteQueued(std::int64_t now_ms) {
  bool started = false;
  for (std::size_t i = 0; i < queued_.size();) {
    if (ActiveInPartition(queued_[i].partition) >= kMaxRequestsPerPartition) {
      ++i;
      continue;
    }
    Queued queued = std::move(queued_[i]);
    queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(i));

    Active active;
    active.id = queued.id;
    active.partition = std::move(queued.partition);
    active.last_progress_ms = now_ms;

    Pending pending = std::move(queued.pending);
    if (NeedsPreflightExchange(pending)) {
      Pending preflight = PreflightFor(pending);
      active.deferred = std::make_unique<Pending>(std::move(pending));
      pending = std::move(preflight);
      AddPerformanceCounter(PerfCounterId::NetCorsPreflights);
    }
    // Stamped before the fetch rather than after, so the timeline's gap column
    // measures the request and not the bookkeeping around it. The URL is taken
    // from the verdict, which is the last thing that saw it before the wire.
    if (util::LoadTimeline::Enabled()) {
      active.url = pending.verdict.FinalUrl().Serialize();
      util::LoadTimeline::MarkWith("request.start", active.url);
    }
    active.request = Fetch(std::move(pending.verdict), policy_, pool_, cookies_, cache_,
                           pending.options, pending.now);
    active_.push_back(std::move(active));
    AddPerformanceCounter(PerfCounterId::NetRequestsStarted);
    started = true;
  }
  if (!queued_.empty()) {
    // Held back by the per-partition bound. Worth a counter rather than a log
    // line: a page whose load is one long queue is a measurement, not an event.
    AddPerformanceCounter(PerfCounterId::NetRequestsDeferred);
  }
  return started;
}

void RequestQueue::Advance(std::int64_t now_ms) {
  // Before anything is promoted, so a request that starts on this turn cannot
  // be handed a connection that has just run out of time. This is also the only
  // place idle connections are ever closed, and it is why `Advance` is called
  // even when nothing is loading: a pooled socket is one the user did not ask
  // to keep open, and nothing else would ever come back for it.
  pool_.CloseExpired(now_ms);

  // Promotion and advancing alternate rather than running once each: a request
  // served from the cache completes the moment it starts, freeing the slot the
  // next one was waiting for. Running one pass would make a fully cached page
  // take one turn of the loop per six resources.
  bool changed = true;
  while (changed) {
    changed = PromoteQueued(now_ms);

    for (std::size_t i = 0; i < active_.size();) {
      Active& active = active_[i];
      if (active.request->Advance(now_ms)) {
        active.last_progress_ms = now_ms;
      } else if (!active.request->IsComplete() &&
                 now_ms - active.last_progress_ms >= kRequestStallTimeoutMs) {
        // Nothing below this line ever blocks, so nothing below this line can
        // time out. This is the only deadline a request has.
        active.request->Fail("the server stopped responding");
        AddPerformanceCounter(PerfCounterId::NetRequestTimeouts);
      }
      if (!active.request->IsComplete()) {
        ++i;
        continue;
      }
      FetchResult result = active.request->TakeResult();
      const Id id = active.id;
      if (util::LoadTimeline::Enabled()) {
        // With the outcome, not just the moment. A request that *failed* fast
        // and one that succeeded fast are the same row otherwise, and a page
        // that renders four images out of nineteen looks from the timeline
        // exactly like a page that wanted four.
        std::string outcome = active.url + " ";
        if (!result.ok) {
          outcome += "FAILED " + result.error;
        } else {
          outcome += std::to_string(result.response.status);
          if (result.from_cache) {
            outcome += " cached";
          }
        }
        util::LoadTimeline::MarkWith("request.done", outcome);
      }
      std::unique_ptr<Pending> deferred = std::move(active.deferred);
      active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
      if (deferred != nullptr) {
        // A preflight finished. Its response is consumed here and never
        // becomes a completion: what the caller asked for has not happened yet.
        OnPreflightComplete(id, std::move(*deferred), result);
      } else {
        completions_.push_back(Completion{id, std::move(result)});
      }
      changed = true;
    }
  }
}

std::vector<RequestQueue::Completion> RequestQueue::TakeCompletions() {
  return std::exchange(completions_, {});
}

void RequestQueue::AppendDescriptors(util::WaitDescriptorList& out) const {
  const std::size_t ours = out.size();
  for (const Active& active : active_) {
    const std::optional<util::WaitDescriptor> interest = active.request->Interest();
    if (!interest.has_value() || !interest->IsValid()) {
      continue;
    }
    // Deduplicated, and HTTP/2 is why: several requests share one session and
    // therefore one descriptor, so a page fetching nineteen images over one
    // connection would otherwise hand the wait nineteen copies of the same
    // socket. Linear over a list bounded by the per-partition concurrency
    // limit, which is smaller than any container that would beat it.
    bool already = false;
    for (std::size_t i = ours; i < out.size(); ++i) {
      if (out[i].descriptor == interest->descriptor) {
        // Merged rather than skipped: two requests on one socket can want
        // different things, and a wait that dropped the second want would
        // sleep through the event it was for.
        out[i].readable = out[i].readable || interest->readable;
        out[i].writable = out[i].writable || interest->writable;
        already = true;
        break;
      }
    }
    if (!already) {
      out.push_back(*interest);
    }
  }
}

bool RequestQueue::HasRunnableWork() const {
  if (!completions_.empty()) {
    return true;
  }
  for (const Active& active : active_) {
    // Not "has no descriptor": a transport with nothing to wait on still
    // answers Blocked or Ready, and only it knows which. Asking the descriptor
    // instead would make a canned transport that is deliberately holding a
    // response look like one that is ready to hand it over.
    if (!active.request->IsBlocked()) {
      return true;
    }
    // A request parked waiting to learn whether its origin speaks HTTP/2 has no
    // socket of its own; it is woken by the socket of whoever is connecting. If
    // nobody is connecting any more -- that request was cancelled, or failed --
    // then this one is runnable and there is nothing else in the loop that
    // would ever say so. Without this it would sit until the stall deadline.
    if (active.request->IsAwaitingProtocol() && pool_.PendingConnects() == 0) {
      return true;
    }
  }
  // A queued request with every slot in its partition busy is not runnable: it
  // moves when one of the active ones finishes, and those are already in the
  // wait.
  return !queued_.empty() && active_.empty();
}

std::optional<std::uint32_t> RequestQueue::NextDeadlineMs(std::int64_t now_ms) const {
  // The idle pool's timeout is a deadline like any other, and it is the one
  // that exists when nothing is loading. A browser holding no connections
  // schedules nothing, which is the zero-idle-CPU invariant; one holding
  // connections schedules exactly one wakeup, after which it holds none.
  std::optional<std::uint32_t> soonest = pool_.NextDeadlineMs(now_ms);
  for (const Active& active : active_) {
    const std::int64_t due = active.last_progress_ms + kRequestStallTimeoutMs;
    const std::int64_t remaining = std::max<std::int64_t>(0, due - now_ms);
    const auto capped = static_cast<std::uint32_t>(
        std::min<std::int64_t>(remaining, static_cast<std::int64_t>(UINT32_MAX)));
    soonest = soonest.has_value() ? std::min(*soonest, capped) : capped;
  }
  return soonest;
}

bool RequestQueue::Cancel(Id id) {
  // Every place a request can be, in the order it passes through them. All
  // three are cleared rather than the first match returned: an id is unique, so
  // finding it twice would be a bug, and looking everywhere is what makes an
  // abort that races a completion mean "no completion" rather than "maybe one".
  bool found = false;
  for (std::size_t i = 0; i < active_.size();) {
    if (active_[i].id != id) {
      ++i;
      continue;
    }
    // Dropping the FetchRequest closes its connection in the destructor, which
    // is what makes an abort stop the transfer rather than stop reporting it.
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
    found = true;
  }
  const auto by_id = [id](const auto& entry) { return entry.id == id; };
  const auto queued_end = std::remove_if(queued_.begin(), queued_.end(), by_id);
  found = found || queued_end != queued_.end();
  queued_.erase(queued_end, queued_.end());
  const auto done_end = std::remove_if(completions_.begin(), completions_.end(), by_id);
  found = found || done_end != completions_.end();
  completions_.erase(done_end, completions_.end());
  return found;
}

void RequestQueue::CancelAll() {
  // Dropping the FetchRequest closes its stream in the destructor, so a
  // response in flight for a document that is gone has nowhere to be delivered.
  // That is what "dropped by construction" means here.
  active_.clear();
  queued_.clear();
  completions_.clear();
  // The preflight grants go with them. They are keyed by partition already, so
  // keeping them would not leak across sites -- but a permission a page's
  // server granted is state that page caused, and ADR 0011's rule is that a
  // navigation leaves nothing of the last document behind.
  preflights_.Clear();
  // H2 sessions and idle HTTP/1.1 sockets too. CancelAll used to leave pooled
  // sessions alive after mass RST; youtube consent's `location.reload()` then
  // reused a half-dead socket and the document load failed with "the connection
  // failed" (TD-0044). Engine::Navigate's comment already promised "connections
  // and all" — this is that half.
  pool_.Clear();
}

}  // namespace microbrowser::net

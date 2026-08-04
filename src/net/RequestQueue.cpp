#include "net/RequestQueue.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

RequestQueue::RequestQueue(const privacy::PrivacyPolicy& policy, TransportFactory& transport,
                           CookieJar& cookies, HttpCache& cache)
    : policy_(policy), transport_(&transport), cookies_(cookies), cache_(cache) {}

void RequestQueue::SetTransport(TransportFactory& transport) {
  if (!active_.empty() || !queued_.empty()) {
    return;
  }
  transport_ = &transport;
}

std::size_t RequestQueue::ActiveInPartition(std::string_view partition) const {
  return static_cast<std::size_t>(std::count_if(
      active_.begin(), active_.end(),
      [partition](const Active& active) { return active.partition == partition; }));
}

RequestQueue::Id RequestQueue::Start(privacy::Verdict verdict, const FetchOptions& options,
                                     std::int64_t now) {
  const Id id = next_id_++;
  Queued queued;
  queued.id = id;
  queued.partition = verdict.Partition().Serialize();
  queued.verdict = std::move(verdict);
  queued.options = options;
  queued.now = now;
  queued_.push_back(std::move(queued));
  return id;
}

bool RequestQueue::PromoteQueued(std::int64_t now_ms) {
  bool started = false;
  for (std::size_t i = 0; i < queued_.size();) {
    if (ActiveInPartition(queued_[i].partition) >= kMaxConnectionsPerPartition) {
      ++i;
      continue;
    }
    Queued queued = std::move(queued_[i]);
    queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(i));

    Active active;
    active.id = queued.id;
    active.partition = std::move(queued.partition);
    active.last_progress_ms = now_ms;
    active.request = Fetch(std::move(queued.verdict), policy_, *transport_, cookies_, cache_,
                           queued.options, queued.now);
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
  // Promotion and advancing alternate rather than running once each: a request
  // served from the cache completes the moment it starts, freeing the slot the
  // next one was waiting for. Running one pass would make a fully cached page
  // take one turn of the loop per six resources.
  bool changed = true;
  while (changed) {
    changed = PromoteQueued(now_ms);

    for (std::size_t i = 0; i < active_.size();) {
      Active& active = active_[i];
      if (active.request->Advance()) {
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
      completions_.push_back(Completion{active.id, active.request->TakeResult()});
      active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
      changed = true;
    }
  }
}

std::vector<RequestQueue::Completion> RequestQueue::TakeCompletions() {
  return std::exchange(completions_, {});
}

void RequestQueue::AppendDescriptors(util::WaitDescriptorList& out) const {
  for (const Active& active : active_) {
    if (const std::optional<util::WaitDescriptor> interest = active.request->Interest()) {
      if (interest->IsValid()) {
        out.push_back(*interest);
      }
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
  }
  // A queued request with every slot in its partition busy is not runnable: it
  // moves when one of the active ones finishes, and those are already in the
  // wait.
  return !queued_.empty() && active_.empty();
}

std::optional<std::uint32_t> RequestQueue::NextDeadlineMs(std::int64_t now_ms) const {
  std::optional<std::uint32_t> soonest;
  for (const Active& active : active_) {
    const std::int64_t due = active.last_progress_ms + kRequestStallTimeoutMs;
    const std::int64_t remaining = std::max<std::int64_t>(0, due - now_ms);
    const auto capped = static_cast<std::uint32_t>(
        std::min<std::int64_t>(remaining, static_cast<std::int64_t>(UINT32_MAX)));
    soonest = soonest.has_value() ? std::min(*soonest, capped) : capped;
  }
  return soonest;
}

void RequestQueue::CancelAll() {
  // Dropping the FetchRequest closes its connection in the destructor, so a
  // response in flight for a document that is gone has nowhere to be delivered.
  // That is what "dropped by construction" means here.
  active_.clear();
  queued_.clear();
  completions_.clear();
}

}  // namespace microbrowser::net

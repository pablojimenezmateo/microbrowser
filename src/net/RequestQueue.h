#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "net/ConnectionPool.h"
#include "net/CookieJar.h"
#include "net/Cors.h"
#include "net/Fetch.h"
#include "net/HttpCache.h"
#include "net/Transport.h"
#include "privacy/PrivacyPolicy.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::net {

// How many *sockets* one partition may open at once.
//
// **Per key rather than global, and that is the privacy content of this file.**
// A global limit would let one site's requests starve another's, which is a
// cross-site interaction of exactly the kind the ADR 0005 partition key exists
// to prevent — and an observable one, because the starved site can time it.
// Six is what the rest of the web assumed for a decade of HTTP/1.1.
inline constexpr std::size_t kMaxConnectionsPerPartition = 6;

// How many *requests* one partition may have on the wire at once.
//
// Until HTTP/2 this was the same number as `kMaxConnectionsPerPartition`,
// because one request owned one socket. A multiplexed session can carry many
// streams, and keeping the bound at six meant youtube.com deferred hundreds of
// subresources behind a connection that would have taken them all (TD-0010).
// Sixty-four is below a typical SETTINGS_MAX_CONCURRENT_STREAMS (100) and well
// inside what `HttpLimits::max_body` can afford in aggregate for the pages this
// browser loads; raising it further wants a measured per-queue byte budget.
inline constexpr std::size_t kMaxRequestsPerPartition = 64;

// How long a request may make no progress at all before it is given up on.
//
// Measured from the last byte that moved rather than from the start, so a large
// download is not killed for being large and a server that accepts a connection
// and then says nothing is. With a non-blocking transport there is no socket
// timeout left to inherit — nothing below this line ever waits — so if this
// deadline were missing, a silent server would hold a descriptor in the loop's
// wait forever and the page would hang with no way out.
inline constexpr std::int64_t kRequestStallTimeoutMs = 30000;

// Every request in flight, and the bound on how many there may be.
//
// This is the piece ADR 0011 adds between the loader and the wire. It exists
// for three reasons that all have to be true at once: requests must be able to
// run concurrently, the loop must be able to sleep on them, and a navigation
// must be able to throw them all away — because a response arriving for a
// document that is gone has to be dropped by construction rather than by a
// check somebody remembers to write.
class RequestQueue {
 public:
  using Id = std::uint64_t;

  struct Completion {
    Id id = 0;
    FetchResult result;
  };

  RequestQueue(const privacy::PrivacyPolicy& policy, TransportFactory& transport,
               CookieJar& cookies, HttpCache& cache);

  RequestQueue(const RequestQueue&) = delete;
  RequestQueue& operator=(const RequestQueue&) = delete;

  // Starts a request, or holds it until this partition has a free slot. The id
  // is handed back immediately either way: the caller records what it asked for
  // now and finds out what happened later, which is the whole shape of the
  // change.
  //
  // Takes a Verdict by value for the reason `Fetch` does — there is no way to
  // reach the wire without one, and this is not a second entry point but a
  // scheduler in front of the only one.
  Id Start(privacy::Verdict verdict, const FetchOptions& options, std::int64_t now);

  // An id that will never be handed to a request. A caller that answers
  // something itself -- a data: URL, a URL that does not parse -- still needs
  // an id to tag the answer with, and one id space is what keeps its callers
  // from having to know which kind of answer they are holding.
  Id ReserveId() { return next_id_++; }

  // Carries everything that can move forward. `now_ms` is *steady* time and the
  // seconds passed to `Start` are wall time: cache and cookie expiry follow the
  // wall clock and a stall deadline must not, or a clock correction fails a
  // request that was fine.
  void Advance(std::int64_t now_ms);

  // Everything that finished since the last call, and nothing twice.
  std::vector<Completion> TakeCompletions();

  // Appends what the loop must wait on. Appends rather than assigns so one
  // buffer can be reused across every source the loop waits on.
  void AppendDescriptors(util::WaitDescriptorList& out) const;

  // True when something outstanding can make progress with no wait at all.
  // A scripted transport in a test is always in this state; a socket almost
  // never is. The loop needs the answer because a request with nothing to wait
  // on and nothing to show would otherwise leave it blocking on input while a
  // load stands still.
  bool HasRunnableWork() const;

  // Milliseconds until the soonest request gives up, or nothing when none is
  // outstanding.
  std::optional<std::uint32_t> NextDeadlineMs(std::int64_t now_ms) const;

  // Drops everything, started and queued, without producing completions. This
  // is what makes "a request belongs to a page and dies with it" a property of
  // the code rather than a rule.
  void CancelAll();

  // Drops one request, wherever it is: waiting for a slot, on the wire, or
  // finished but not yet collected. No completion is produced, which is what
  // makes an aborted `fetch` unable to run its own `then` afterwards.
  //
  // `AbortController` is why this exists (ADR 0020 §1) and it is not a
  // convenience: a request that cannot be cancelled is one that outlives the
  // thing that made it, and this browser's answer to that has so far been
  // `CancelAll` at a navigation -- which is the right hammer for a page going
  // away and no help at all to a page cancelling its own search-as-you-type.
  bool Cancel(Id id);

  std::size_t InFlight() const { return active_.size() + queued_.size(); }
  bool IsIdle() const { return active_.empty() && queued_.empty() && completions_.empty(); }

  // Swaps in a different socket layer. Tests use it to serve canned bytes.
  // Refused while anything is outstanding: changing the factory under a live
  // connection would leave a descriptor nobody owns. Idle pooled connections
  // are dropped with it, for the same reason.
  void SetTransport(TransportFactory& transport);

  // The connections this queue keeps between requests. Exposed so a test can
  // ask how many are idle and so the loop can be told when the soonest of them
  // expires; not for handing one out.
  const ConnectionPool& Connections() const { return pool_; }

  // What a preflight has bought and has not yet been spent. Exposed so a test
  // can assert an `OPTIONS` happened once rather than twice; not for handing a
  // grant out.
  const PreflightCache& Preflights() const { return preflights_; }

 private:
  // Everything needed to start one request, before it has one. A request that
  // needs a CORS preflight is two exchanges under one id, so this exists as a
  // value that can be *held* while the first one runs -- which the three
  // parallel fields it replaces could not be.
  struct Pending {
    privacy::Verdict verdict;
    FetchOptions options;
    // Wall-clock seconds, which is what cookie expiry, cache expiry and
    // `Access-Control-Max-Age` are all measured in.
    std::int64_t now = 0;
  };

  struct Active {
    Id id = 0;
    std::unique_ptr<FetchRequest> request;
    // Serialized rather than the key itself, because the bound is a count of
    // equal keys and a string compare is the cheapest way to say that without
    // teaching PartitionKey to hash.
    std::string partition;
    std::int64_t last_progress_ms = 0;
    // Only ever filled in when the load timeline is on, and empty otherwise: a
    // per-request string allocated on every load to serve a diagnostic nobody
    // asked for is exactly the cost instrumentation is supposed to avoid.
    std::string url;
    // Set only when `request` is the `OPTIONS` of a CORS preflight: the real
    // request, waiting for permission, which starts under the same id once the
    // preflight is allowed. One id space for both is what keeps a caller from
    // having to know that its request cost two round trips.
    std::unique_ptr<Pending> deferred;
  };

  struct Queued {
    Id id = 0;
    Pending pending;
    std::string partition;
  };

  // Moves queued requests into open slots. Returns true when it started any,
  // which is what tells `Advance` to go round again — a request served from
  // cache completes the instant it starts and frees its slot at once.
  bool PromoteQueued(std::int64_t now_ms);
  std::size_t ActiveInPartition(std::string_view partition) const;
  void Enqueue(Id id, Pending pending);
  // Whether this request has to ask permission first, and marks it preflighted
  // when the cache already holds that permission. Both answers in one function
  // because they are one decision made against one cache lookup.
  bool NeedsPreflightExchange(Pending& pending) const;
  // The `OPTIONS` that asks. Never carries credentials, never follows a
  // redirect, and never reaches the caller: its response is consumed here.
  static Pending PreflightFor(const Pending& real);
  // Acts on a finished preflight: stores what it granted and re-queues the real
  // request, or turns it into a failed completion. True when the real request
  // was queued, which is the caller's signal that this id is not finished.
  bool OnPreflightComplete(Id id, Pending deferred, const FetchResult& result);

  const privacy::PrivacyPolicy& policy_;
  ConnectionPool pool_;
  CookieJar& cookies_;
  HttpCache& cache_;
  std::vector<Active> active_;
  std::vector<Queued> queued_;
  std::vector<Completion> completions_;
  // Keyed by the ADR 0005 partition key, for the reason the connection pool is:
  // "did this preflight already happen" is a question the next site must not be
  // able to ask about this one. See Cors.h.
  PreflightCache preflights_;
  Id next_id_ = 1;
};

}  // namespace microbrowser::net

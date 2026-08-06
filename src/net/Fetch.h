#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/ConnectionPool.h"
#include "net/CookieJar.h"
#include "net/Cors.h"
#include "net/HttpCache.h"
#include "net/Http2Session.h"
#include "net/HttpMessage.h"
#include "net/Transport.h"
#include "privacy/PrivacyPolicy.h"
#include "privacy/Verdict.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::net {

struct FetchOptions {
  std::string method = "GET";
  HttpHeaders headers;
  std::vector<std::byte> body;
  // Redirect chains are bounded because a server can make them infinite.
  int max_redirects = 10;
  // False for a subresource. Decides which cookies travel — Lax cookies go on
  // a top-level navigation and nowhere else.
  bool is_top_level_navigation = false;
  // True for an explicit reload that should validate by going to the network
  // rather than serving an existing fresh entry. A successful response may
  // still replace the cache entry.
  bool bypass_cache = false;
  // Who is asking and what they may read back. Defaults to `Browser`, which is
  // no CORS check at all -- every request this browser makes for itself is one
  // of those, and a document load is not a `no-cors` fetch that happens to be
  // readable. `fetch` fills this in; see Cors.h.
  //
  // On the options rather than beside them because a redirect rewrites the
  // options and has to rewrite this with them: a hop to a third party is how a
  // same-origin request becomes a cross-origin one, and the origin is tainted
  // at the hop.
  CorsParams cors;
};

struct FetchResult {
  bool ok = false;
  // Owned rather than a `const char*`. Most reasons are literals, but the one
  // that matters most -- what the privacy layer said when it refused -- is a
  // string the verdict owns, and the request that owns the verdict is gone by
  // the time anybody renders the result.
  std::string error;
  HttpResponse response;
  url::Url final_url;
  int redirects = 0;
  bool from_cache = false;
  // A `no-cors` response to a cross-origin request. The status is 0, the
  // headers are gone and the body is empty -- not hidden behind this flag but
  // *discarded*, inside `net`, before this result existed. The flag says why
  // the response is empty; there is nothing behind it to read through, which is
  // what ADR 0020 §2 means by opaque being a real thing rather than a marking.
  bool opaque = false;
};

// One request, in flight.
//
// ADR 0011 turned fetching from a call that returns a response into a request
// that is *started* and whose completion arrives on a later turn of the loop.
// This is that object. `Advance()` carries it as far as it can go without
// blocking and returns; when it can go no further it says so through
// `Interest()`, which is the descriptor the loop's single blocking wait must
// watch. Nothing here spins and nothing here sleeps.
//
// Neither copyable nor movable: it owns a live connection, and a request whose
// address changed under an outstanding descriptor is the kind of bug that only
// appears under load.
class FetchRequest {
 public:
  // Takes the pool rather than a factory, and for the reason it takes a
  // Verdict: with the pool in the way there is no path from a request to a
  // socket that does not go past the ADR 0005 partition key.
  FetchRequest(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy, ConnectionPool& pool,
               CookieJar& cookies, HttpCache& cache, FetchOptions options, std::int64_t now);
  ~FetchRequest();

  FetchRequest(const FetchRequest&) = delete;
  FetchRequest& operator=(const FetchRequest&) = delete;

  // Carries the request forward as far as it can without blocking. True when it
  // made progress — connected, sent bytes, received bytes, or finished. That is
  // what an inactivity deadline is measured against, which is why it is the
  // return value rather than "am I done".
  //
  // `now_ms` is steady time, and it is here rather than in the constructor
  // because a request that completes hands its connection back to the pool and
  // the pool times that connection out from the moment it arrived, not from the
  // moment the request started.
  bool Advance(std::int64_t now_ms);

  bool IsComplete() const { return complete_; }
  // True when the last `Advance()` stopped because the transport had nothing
  // for it. The queue asks so it can tell the loop whether anything is
  // runnable right now, which is a different question from "is there a
  // descriptor" -- a transport with no descriptor at all still answers Blocked
  // or Ready, and only it knows which.
  bool IsBlocked() const { return blocked_; }
  // Only meaningful once complete.
  const FetchResult& Result() const { return result_; }
  FetchResult TakeResult() { return std::move(result_); }

  // What the loop must wait on for this request to make progress. Absent when
  // it is complete, or when its transport cannot block.
  std::optional<util::WaitDescriptor> Interest() const;

  // True while this request is parked because another connect to the same
  // origin has not yet said which protocol it speaks (see
  // `ConnectionPool::Acquire`). It has no socket of its own, so it has nothing
  // for the loop to wait on -- the queue asks, because a parked request whose
  // connector has gone away is runnable and nothing else would ever say so.
  bool IsAwaitingProtocol() const { return stage_ == Stage::AwaitingProtocol; }

  // The partition this request is being made in. The concurrency bound is per
  // key rather than global — see RequestQueue.
  const url::PartitionKey& Partition() const { return verdict_.Partition(); }

  // Gives up, with a reason the caller can render. Used by the queue when a
  // request stops making progress, and by a navigation that abandons one.
  void Fail(std::string_view reason);

 private:
  enum class Stage : std::uint8_t {
    Begin,       // nothing opened yet, or a redirect just re-entered the policy
    // Parked: somebody else is opening the connection whose ALPN will say
    // whether this origin speaks HTTP/2, and opening a second one would be the
    // burst that protocol exists to remove.
    AwaitingProtocol,
    Connecting,  // the TCP connect, and the TLS handshake behind it
    Sending,     // the request line, the headers, then the body -- HTTP/1.1
    Receiving,   // HTTP/1.1
    Streaming,   // one stream on a shared HTTP/2 session
    Done,
  };

  // Opens the connection for the current URL, or serves it from cache. False
  // when the request finished here rather than moving on.
  bool BeginExchange();
  // The handshake is over and ALPN has answered. Either the socket becomes a
  // session in the pool and this request becomes a stream on it, or the origin
  // is recorded as HTTP/1.1 and the request is serialized. **This is the one
  // place the two protocols diverge**, and everything before and after it --
  // the verdict, the cookies, the cache, CORS, redirects, the retry rule -- is
  // shared by construction rather than by discipline.
  void ChooseProtocol();
  // Puts this request on the session as a stream. False when the session could
  // not take it, which is a fresh connection's job rather than a failure.
  bool StartStream();
  // Cookies, cache, and the redirect decision. Sets the next stage. Takes the
  // response rather than reading it off the parser, because on HTTP/2 there is
  // no parser -- the session assembled it. Steady time is not a parameter: what
  // this does is measured in the wall clock a cookie and a cache entry expire
  // against, and the connection was given back before it was called.
  void DeliverResponse(HttpResponse response);
  void Complete(HttpResponse response, const url::Url& url);
  // Hands the connection back to the pool, or closes it. Everything that
  // decides between those two is in one place on purpose: a connection kept
  // when it should not have been is the next request reading a body as a
  // status line.
  void ReleaseConnection(const HttpResponse& response, std::int64_t now_ms);
  // Gives back whatever this request is holding: the HTTP/1.1 socket, its
  // HTTP/2 stream, and the claim on "somebody is connecting to this origin".
  // One function because forgetting the third parks every other request for the
  // same origin until the stall deadline, and there are five paths out of here.
  void ReleaseEverything();
  // True when a request that has produced nothing may be sent again on a fresh
  // connection. Worth exactly one retry and no more.
  //
  // `nothing_was_processed` is the protocol-specific half: on HTTP/1.1 it is a
  // pooled connection that failed before saying anything -- the race every pool
  // has -- and on HTTP/2 it is REFUSED_STREAM or a GOAWAY that never promised
  // to handle this stream. Both mean the server did not act on the request, and
  // that is the only condition under which resending it is not a second side
  // effect.
  bool MayRetry(bool nothing_was_processed) const { return !retried_ && nothing_was_processed; }
  // The HTTP/1.1 half of that, spelled once: a connection that came out of the
  // pool and then failed before the server said anything is the race every pool
  // has, and it is the only HTTP/1.1 failure a resend is safe after.
  bool PooledConnectionSaidNothing() const { return reused_ && parser_.NothingReceived(); }

  privacy::Verdict verdict_;
  const privacy::PrivacyPolicy& policy_;
  ConnectionPool& pool_;
  CookieJar& cookies_;
  HttpCache& cache_;
  FetchOptions remaining_;
  std::int64_t now_ = 0;

  std::unique_ptr<Transport> connection_;
  // Set instead of `connection_` once ALPN said `h2`: the connection is the
  // pool's and is shared with every other request to this origin.
  std::shared_ptr<Http2Session> session_;
  Http2Session::StreamId stream_ = 0;
  // The pool key this request's connection was taken under, and whether this
  // request is the one holding that origin's connect claim.
  std::string connect_key_;
  bool owns_connect_ = false;
  // Built once, before the protocol is known, because both protocols send the
  // same fields and only the framing differs. Keeping the *header list* rather
  // than the serialized request is what makes that true.
  HttpHeaders request_headers_;
  ResponseParser parser_;
  // The request line, headers and body as one buffer, and how much of it has
  // gone out. A partial write is normal on a non-blocking socket, and treating
  // it as an error is the classic way an async rewrite corrupts large POSTs.
  std::string outgoing_;
  std::size_t sent_ = 0;
  int redirects_ = 0;
  bool may_use_cache_ = false;
  bool complete_ = false;
  bool blocked_ = false;
  // Whether the connection in hand came out of the pool, and whether the one
  // retry that fact buys has been spent.
  bool reused_ = false;
  bool retried_ = false;
  Stage stage_ = Stage::Begin;
  FetchResult result_;
};

// The only way to make a network request.
//
// **It takes a `privacy::Verdict` by value and there is no overload without
// one.** `guidelines/privacy.md` states the rule; this signature is what makes
// it unbypassable, and `ArchitectureInvariants` fails the build if a second
// entry point appears. A Verdict can only come from `privacy::PrivacyPolicy`,
// so "every request passes privacy first" is a property of the type system
// rather than of anyone's diligence.
//
// The policy is passed as well, and not as a redundancy: **a redirect is a new
// request to a URL the page did not choose**, and it has to be put through the
// policy again. A fetch that followed redirects without re-deciding would let
// any server reach a blocked host, downgrade to http, or re-add the tracking
// parameters that were just stripped, by answering with a 302.
//
// It *starts* the request and hands it back. There is no overload that returns
// a response, and that absence is the decision rather than an oversight: a
// codebase with both shapes grows calls that block inside a completion, which
// is the one thing an event loop cannot survive. See ADR 0011.
std::unique_ptr<FetchRequest> Fetch(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
                                    ConnectionPool& pool, CookieJar& cookies, HttpCache& cache,
                                    const FetchOptions& options, std::int64_t now);

}  // namespace microbrowser::net

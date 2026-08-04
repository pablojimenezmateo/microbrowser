#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/CookieJar.h"
#include "net/HttpCache.h"
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
  FetchRequest(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
               TransportFactory& transport, CookieJar& cookies, HttpCache& cache,
               FetchOptions options, std::int64_t now);
  ~FetchRequest();

  FetchRequest(const FetchRequest&) = delete;
  FetchRequest& operator=(const FetchRequest&) = delete;

  // Carries the request forward as far as it can without blocking. True when it
  // made progress — connected, sent bytes, received bytes, or finished. That is
  // what an inactivity deadline is measured against, which is why it is the
  // return value rather than "am I done".
  bool Advance();

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

  // The partition this request is being made in. The concurrency bound is per
  // key rather than global — see RequestQueue.
  const url::PartitionKey& Partition() const { return verdict_.Partition(); }

  // Gives up, with a reason the caller can render. Used by the queue when a
  // request stops making progress, and by a navigation that abandons one.
  void Fail(std::string_view reason);

 private:
  enum class Stage : std::uint8_t {
    Begin,       // nothing opened yet, or a redirect just re-entered the policy
    Connecting,  // the TCP connect, and the TLS handshake behind it
    Sending,     // the request line, the headers, then the body
    Receiving,
    Done,
  };

  // Opens the connection for the current URL, or serves it from cache. False
  // when the request finished here rather than moving on.
  bool BeginExchange();
  // Cookies, cache, and the redirect decision. Sets the next stage.
  void FinishResponse();
  void Complete(HttpResponse response, const url::Url& url);

  privacy::Verdict verdict_;
  const privacy::PrivacyPolicy& policy_;
  TransportFactory& transport_;
  CookieJar& cookies_;
  HttpCache& cache_;
  FetchOptions remaining_;
  std::int64_t now_ = 0;

  std::unique_ptr<Transport> connection_;
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
                                    TransportFactory& transport, CookieJar& cookies,
                                    HttpCache& cache, const FetchOptions& options,
                                    std::int64_t now);

}  // namespace microbrowser::net

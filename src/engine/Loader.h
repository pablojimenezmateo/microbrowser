#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/CookieJar.h"
#include "net/Fetch.h"
#include "net/HttpCache.h"
#include "net/RequestQueue.h"
#include "net/SocketTransport.h"
#include "privacy/PrivacyPolicy.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::engine {

// Turns a URL into bytes, eventually.
//
// Everything network-shaped lives here rather than on Engine: the policy, the
// cookie jar, the cache, and the request queue are members that would otherwise
// be that many more reasons for Engine to become the browser.
//
// Every load goes through privacy::PrivacyPolicy first, because net::Fetch
// takes a Verdict and there is no overload without one -- see
// guidelines/privacy.md. This class cannot bypass that rule; it can only fail
// to be used.
//
// ADR 0011 changed the shape and not the rules. A load is *started* and hands
// back an id; the answer arrives from `TakeCompletions()` on some later turn of
// the loop, tagged with that id. There is no call here that returns a document,
// and that absence is the design: a codebase with both shapes grows a caller
// that blocks inside a completion.
class Loader {
 public:
  Loader();

  using RequestId = net::RequestQueue::Id;

  struct Result {
    bool ok = false;
    // Empty when `ok`. A short reason, suitable to render. Owned rather than a
    // `const char*` because the most useful one -- what the privacy layer said
    // when it refused -- belongs to a request that is gone by the time anyone
    // renders it.
    std::string error;
    std::string body;
    std::string content_type;
    // Where the bytes actually came from, after redirects. Not the requested
    // URL: a document's base URL is where it ended up.
    std::string final_url;
    int status = 0;
    // The reason phrase, and every header this request is allowed to read.
    //
    // Here for `fetch` and for nothing else so far: a document load wants one
    // header and reads it out of `content_type`, but a page's own request is
    // handed a `Response` whose `headers` it enumerates. The list is already
    // *filtered* -- a cross-origin CORS response arrives with only the
    // safelisted fields and whatever `Access-Control-Expose-Headers` named,
    // because `net` removed the rest before this struct existed. Copying them
    // here cannot widen that.
    std::string status_text;
    std::vector<std::pair<std::string, std::string>> headers;
    // A `no-cors` response to a cross-origin request: status 0, no headers, no
    // body. Not a curtain over readable bytes -- there are none. See
    // net::FetchResult::opaque.
    bool opaque = false;
    bool redirected = false;
  };

  struct Completion {
    RequestId id = 0;
    Result result;
  };

  // Starts a top-level navigation. `now` is wall-clock seconds, passed in
  // rather than read from the clock so that cache expiry and cookie expiry are
  // testable and so that two decisions inside one load cannot disagree about
  // what time it is.
  // `referrer_document` is null for a navigation the user typed and is the
  // current document for one a page caused. Not defaulted, because which of
  // those two a call is deciding is exactly what a reader needs to see.
  RequestId StartLoad(std::string_view url, std::int64_t now, const net::FetchOptions& options,
                      const url::Url* referrer_document);

  // Starts something the document asked for, rather than something the user
  // did. The distinction is not cosmetic: it decides which cookies travel,
  // whether HTTPS-only may show an interstitial or must simply refuse, and
  // what `$1p`/`$3p` filter rules mean -- so it is a parameter of the request
  // rather than a flag on the transport.
  //
  // `url` is resolved against `document`, which is also the initiator and the
  // top-level site: this browser has no frames, so the document that asked is
  // always the top-level one.
  RequestId StartSubresource(std::string_view url, const url::Url& document,
                             privacy::ResourceType type, std::int64_t now);
  RequestId StartSubresource(std::string_view url, const url::Url& document,
                             privacy::ResourceType type, std::int64_t now,
                             const net::FetchOptions& options);

  // Carries every started request as far as it can go without blocking.
  // `now_ms` is *steady* milliseconds and the `now` above is wall seconds: a
  // stall deadline must not follow a clock correction, and cache expiry must.
  void Advance(std::int64_t now_ms);

  // Everything that finished since the last call, and nothing twice.
  std::vector<Completion> TakeCompletions();

  // Appends what the loop's single blocking wait must watch.
  void AppendDescriptors(util::WaitDescriptorList& out) const;
  // True when something can make progress with no wait at all -- a data: URL
  // that has been decoded, a refusal, or a transport that does not block.
  bool HasRunnableWork() const;
  // Milliseconds until the soonest request gives up on a silent server.
  std::optional<std::uint32_t> NextDeadlineMs(std::int64_t now_ms) const;

  // Drops everything outstanding without producing completions. A navigation
  // calls it, and that is what makes "a response for a document that is gone is
  // dropped by construction" true rather than aspirational.
  void CancelAll();

  // Drops one. `AbortController` is why it exists: a page cancelling its own
  // search-as-you-type is not a navigation, and `CancelAll` is the wrong
  // hammer for it. No completion follows, so an aborted request cannot run its
  // own `then`.
  bool Cancel(RequestId id);

  bool IsIdle() const { return queue_.IsIdle() && ready_.empty(); }

  privacy::PrivacyPolicy& Policy() { return policy_; }
  net::CookieJar& Cookies() { return cookies_; }

  // Swaps in a different socket layer. Tests use it to serve canned bytes;
  // there is no other way to exercise a fetch without a network.
  void SetTransport(net::TransportFactory& transport) {
    queue_.SetTransport(transport);
    factory_ = &transport;
  }

  // A transport for something that is *not* a request: a WebSocket, which lives outside
  // the queue because it has no response and no completion. The factory rather than the
  // queue, so that a test's scripted transport serves both -- and so that there is
  // exactly one place in the engine that decides what a socket is made of.
  std::unique_ptr<net::Transport> NewTransport() {
    return factory_ != nullptr ? factory_->Create() : sockets_.Create();
  }

 private:
  // The one place a request is actually started. Both entry points funnel
  // through it so that "every request passed the policy" is true by
  // construction rather than by two functions remembering to do the same thing.
  RequestId Start(const privacy::Request& request, const net::FetchOptions& options,
                  bool top_level, std::int64_t now, const url::Url* referrer_document);
  // Records an answer that needed no network at all: a data: URL, a URL that
  // does not parse, a refusal. It still arrives through `TakeCompletions()`,
  // because a caller that had to handle two delivery shapes would grow a branch
  // that only the second one exercises.
  RequestId Deliver(Result result);

  privacy::PrivacyPolicy policy_;
  net::SocketTransportFactory sockets_;
  net::CookieJar cookies_;
  net::HttpCache cache_;
  net::RequestQueue queue_;
  // The factory a caller handed over, or null for the real one. Held because a
  // long-lived connection is made outside the queue; see NewTransport.
  net::TransportFactory* factory_ = nullptr;
  // Answers produced without a request. Kept apart from the queue's own
  // completions so that `CancelAll` clears both, and neither can outlive the
  // document that asked.
  std::vector<Completion> ready_;
};

// Decodes a `data:` URL. Empty and `ok == false` for anything malformed.
//
// Separate from Loader because it touches no network state and is the thing a
// test reaches for when it wants a document without a server.
struct DataUrl {
  bool ok = false;
  std::string content_type;
  std::string body;
};
DataUrl DecodeDataUrl(std::string_view url);

}  // namespace microbrowser::engine

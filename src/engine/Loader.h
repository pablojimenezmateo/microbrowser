#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "net/CookieJar.h"
#include "net/Fetch.h"
#include "net/HttpCache.h"
#include "net/SocketTransport.h"
#include "privacy/PrivacyPolicy.h"

namespace microbrowser::engine {

// Turns a URL into bytes.
//
// Everything network-shaped lives here rather than on Engine: the policy, the
// cookie jar, the cache, and the socket factory are four members that would
// otherwise be four more reasons for Engine to become the browser.
//
// Every load goes through privacy::PrivacyPolicy first, because net::Fetch
// takes a Verdict and there is no overload without one -- see
// guidelines/privacy.md. This class cannot bypass that rule; it can only fail
// to be used.
class Loader {
 public:
  Loader();

  struct Result {
    bool ok = false;
    // Non-null when `ok` is false. A short reason, suitable to render.
    const char* error = nullptr;
    std::string body;
    std::string content_type;
    // Where the bytes actually came from, after redirects. Not the requested
    // URL: a document's base URL is where it ended up.
    std::string final_url;
    int status = 0;
  };

  // Loads a top-level navigation. `now` is passed in rather than read from the
  // clock so that cache expiry and cookie expiry are testable and so that two
  // decisions inside one load cannot disagree about what time it is.
  Result Load(std::string_view url, std::int64_t now);

  // Loads something the document asked for, rather than something the user
  // did. The distinction is not cosmetic: it decides which cookies travel,
  // whether HTTPS-only may show an interstitial or must simply refuse, and
  // what `$1p`/`$3p` filter rules mean -- so it is a parameter of the request
  // rather than a flag on the transport.
  //
  // `url` is resolved against `document`, which is also the initiator and the
  // top-level site: this browser has no frames, so the document that asked is
  // always the top-level one.
  Result LoadSubresource(std::string_view url, const url::Url& document,
                         privacy::ResourceType type, std::int64_t now);

  privacy::PrivacyPolicy& Policy() { return policy_; }
  net::CookieJar& Cookies() { return cookies_; }

  // Swaps in a different socket layer. Tests use it to serve canned bytes;
  // there is no other way to exercise a fetch without a network.
  void SetTransport(net::TransportFactory& transport) { transport_ = &transport; }

 private:
  // The one place a request is actually made. Both entry points funnel through
  // it so that "every request passed the policy" is true by construction
  // rather than by two functions remembering to do the same thing.
  Result Fetch(const url::Url& target, const privacy::Request& request, bool top_level,
               std::int64_t now);

  privacy::PrivacyPolicy policy_;
  net::SocketTransportFactory sockets_;
  net::TransportFactory* transport_ = nullptr;
  net::CookieJar cookies_;
  net::HttpCache cache_;
  // Owns the string a blocked Result::error points at. The error is a `const
  // char*` because most of them are literals; the one that is not needs
  // somewhere to live that outlives the call.
  std::string blocked_reason_;
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

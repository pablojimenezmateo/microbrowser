#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "net/HttpMessage.h"
#include "url/Origin.h"
#include "url/Url.h"

namespace microbrowser::net {

// Cross-origin resource sharing, decided here and enforced in `Fetch`.
//
// **This file is the network process.** ADR 0020 §2 is explicit about why that
// matters: an implementation that fetches, then checks, then throws in the
// binding has already put a cross-origin response in the address space of the
// process that runs attacker-supplied script, and every side channel out of
// that process is then a cross-origin read. The process split has not landed
// (ADR 0004 is M7), so the check is written where the network process will be
// and the response is *discarded* rather than marked -- `FetchRequest::Complete`
// replaces it with a failure or with an empty opaque one before the result
// exists at all. When the split lands this code moves without changing.
//
// Nothing here is stateful except the preflight cache, and nothing here reads a
// clock: every decision is a pure function of the request parameters and the
// response headers, which is what makes it testable without a socket.

// What a request is allowed to reach, and what it may read back.
//
// `Browser` is every request this browser made for itself -- a navigation, an
// image, a stylesheet -- and it means no CORS check at all. Naming it rather
// than defaulting to one of the fetch modes is deliberate: a document load is
// not a `no-cors` fetch that happens to be readable, and a browser whose
// subresource loads went through the script path would be one mistake away
// from making every one of them opaque.
enum class RequestMode : std::uint8_t {
  Browser,
  // `mode: "same-origin"`. A cross-origin URL is a network error before a
  // socket is opened.
  SameOrigin,
  // `mode: "no-cors"`. Cross-origin is allowed and the answer is opaque:
  // status 0, no headers, no body. That has to be a property of what the
  // caller receives rather than a flag it is trusted to check.
  NoCors,
  // `mode: "cors"`, the default for `fetch`. The response is delivered only
  // when the server named this origin, and only the safelisted headers plus
  // whatever `Access-Control-Expose-Headers` names are visible.
  Cors,
};

// Whether the user's cookies travel, and whether the server has to say it is
// happy about that.
enum class CredentialsMode : std::uint8_t {
  Omit,
  // The default: credentials on a same-origin request, nothing cross-origin.
  SameOrigin,
  Include,
};

// The CORS half of a request, carried on `FetchOptions` so that a redirect --
// which is a new request to a URL the page did not choose -- re-decides with it.
struct CorsParams {
  RequestMode mode = RequestMode::Browser;
  CredentialsMode credentials = CredentialsMode::SameOrigin;
  // The origin that asked. Opaque means "null", which is both what a `data:`
  // document's origin serializes to and what a request whose origin was tainted
  // by a cross-origin redirect must send. One representation for both, because
  // a second one is how a tainted request ends up sending its real origin.
  url::Origin origin;
  // Set by the queue when a preflight was performed or found in the cache. A
  // preflighted request that is then redirected is a network error: the
  // permission the server gave was for one URL, and following the redirect
  // would spend it on another.
  bool preflighted = false;
  // Set only on the `OPTIONS` a preflight sends: the method and the folded
  // header names the real request will use. `Fetch` turns these into
  // `Access-Control-Request-Method` and `Access-Control-Request-Headers`.
  //
  // Here rather than in the preflight's header list because a caller cannot set
  // those two fields at all -- they are in `IsFetchOwnedHeader`, so that a page
  // cannot claim a preflight it never made. Something has to be able to write
  // them, and it is this, which only the queue fills in.
  std::string preflight_method;
  std::string preflight_headers;
};

// Same-origin in the CORS sense: scheme, host and port, with no site-level
// relaxation. `url::Origin::IsSameOrigin` is the comparison; this exists so no
// caller is tempted to compare serializations, which makes two opaque origins
// equal and is the classic way a sandboxed document becomes same-origin with
// its parent.
bool IsSameOrigin(const url::Origin& origin, const url::Url& url);

// The methods a cross-origin request may use without asking first.
bool IsCorsSafelistedMethod(std::string_view method);

// The request headers a page may set without asking first. The `content-type`
// entry is the one with teeth: `application/json` is *not* safelisted, which is
// why nearly every JSON API costs a preflight.
bool IsCorsSafelistedRequestHeader(std::string_view name, std::string_view value);

// The response headers a cross-origin `cors` response exposes without being
// told to. Everything else -- `set-cookie` above all -- is removed before the
// response leaves `net`.
bool IsCorsSafelistedResponseHeader(std::string_view name);

// Whether an `OPTIONS` has to go first. `author_headers` is exactly what the
// caller set: `Fetch` builds the rest itself and drops any of its own the
// caller tried to send, so this cannot be fooled by a forged `Host`.
bool NeedsPreflight(std::string_view method, const HttpHeaders& author_headers);

// The value of `Access-Control-Request-Headers`: the author header names,
// lowercased and sorted, which is the form servers match against.
std::string PreflightRequestHeaderList(const HttpHeaders& author_headers);

struct CorsResult {
  bool allowed = false;
  // Empty when allowed. A short reason for the console; never handed to script,
  // which learns only that the fetch failed -- the whole point of CORS is that
  // the failure carries no information about the response.
  std::string error;
};

// The check on the response to the actual request. Same-origin requests and
// `Browser` mode are allowed without looking at a header.
CorsResult CheckResponse(const CorsParams& params, const url::Url& url,
                         const HttpResponse& response);

// What a preflight response permits, and for how long.
struct PreflightGrant {
  bool allow_all_methods = false;
  bool allow_all_headers = false;
  bool credentials = false;
  std::vector<std::string> methods;
  // Lowercased.
  std::vector<std::string> headers;
  std::int64_t max_age_seconds = 0;
};

// The check on a preflight response. Fills `grant` only when it passes.
CorsResult CheckPreflight(const CorsParams& params, std::string_view method,
                          const HttpHeaders& author_headers, const url::Url& url,
                          const HttpResponse& response, PreflightGrant& grant);

// Removes every response header a cross-origin `cors` response is not allowed
// to expose. Applied inside `net`, on the way out, so that "the renderer never
// saw `Set-Cookie`" is true of the bytes rather than of a getter.
void FilterExposedHeaders(HttpResponse& response);

// What a preflight bought, remembered so the next request does not buy it
// again.
//
// **Keyed by the ADR 0005 partition key before anything else.** A preflight
// cache is a cross-site linkage like any other cache: without the key, one
// site's preflight of `api.example.com` would be measurable by the next site to
// request it, as the absence of an `OPTIONS` on the wire.
//
// Bounded, because every key in it comes from a page: entries beyond the cap
// evict the oldest, and an expired one is dropped when it is looked at.
class PreflightCache {
 public:
  // Whether this exact request is already permitted. `now_seconds` is wall time
  // for the reason cache expiry is: `Access-Control-Max-Age` is a duration the
  // server chose, not a deadline this process measures.
  bool Allows(std::string_view partition, const CorsParams& params, const url::Url& url,
              std::string_view method, const HttpHeaders& author_headers,
              std::int64_t now_seconds) const;

  void Store(std::string_view partition, const CorsParams& params, const url::Url& url,
             const PreflightGrant& grant, std::int64_t now_seconds);

  // Dropped on navigation, with everything else a page owned.
  void Clear() { entries_.clear(); }
  std::size_t Size() const { return entries_.size(); }

 private:
  struct Entry {
    std::string key;
    PreflightGrant grant;
    std::int64_t expires = 0;
  };

  // One string rather than four fields, for the reason RequestQueue serializes
  // a partition key to count connections: the lookup is an equality test and a
  // string compare is the cheapest way to say that.
  static std::string KeyFor(std::string_view partition, const CorsParams& params,
                            const url::Url& url);

  std::vector<Entry> entries_;
};

// How many preflight grants are remembered at once. Small on purpose: a page
// that talks to more than this many distinct API origins pays an `OPTIONS`
// again rather than being allowed to grow a table in the browser.
inline constexpr std::size_t kMaxPreflightEntries = 64;

}  // namespace microbrowser::net

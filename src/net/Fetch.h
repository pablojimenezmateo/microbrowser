#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/CookieJar.h"
#include "net/HttpCache.h"
#include "net/HttpMessage.h"
#include "net/Transport.h"
#include "privacy/PrivacyPolicy.h"
#include "privacy/Verdict.h"

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
  const char* error = nullptr;
  HttpResponse response;
  url::Url final_url;
  int redirects = 0;
  bool from_cache = false;
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
FetchResult Fetch(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
                  TransportFactory& transport, CookieJar& cookies, HttpCache& cache,
                  const FetchOptions& options, std::int64_t now);

}  // namespace microbrowser::net

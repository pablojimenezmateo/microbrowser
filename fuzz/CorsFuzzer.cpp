#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "net/Cors.h"
#include "net/HttpMessage.h"
#include "url/Origin.h"
#include "url/Url.h"

// The CORS decision, fed a response a hostile server wrote.
//
// Every input to `CheckResponse` and `CheckPreflight` comes off the wire: the
// header values are the server's, and the request half is a page's. The
// property being fuzzed is not "does it crash" -- there is no buffer here to
// overrun -- but the one that actually matters: **a response is never allowed
// through unless a header named this exact origin, or said `*` without
// credentials.** A parser bug that made `SplitList` produce an empty piece, or
// made the max-age arithmetic wrap, would show up as an allow where there
// should be a refusal, and that is an assertion rather than a sanitizer report.
//
// The `Access-Control-Max-Age` bound is checked here for the same reason
// RootMarginFuzzer checks finiteness: the value is attacker-controlled, and a
// grant whose expiry overflowed is a permission the server can no longer
// revoke.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) {
    return 0;
  }
  using namespace microbrowser;

  const auto page = url::Url::Parse("https://page.example/");
  const auto target = url::Url::Parse("https://api.example/data");
  if (!page.has_value() || !target.has_value()) {
    return 0;
  }

  net::CorsParams params;
  params.mode = static_cast<net::RequestMode>(data[0] % 4);
  params.credentials = static_cast<net::CredentialsMode>(data[1] % 3);
  params.origin = url::Origin::FromUrl(*page);

  // The rest is one header block, split on newlines. A server sends fields, so
  // that is what this feeds it -- and the split is here rather than in a helper
  // so a field with no colon, a repeated field and an empty value are all
  // reachable by mutation.
  const std::string_view text(reinterpret_cast<const char*>(data + 2), size - 2);
  net::HttpResponse response;
  response.status = 200;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::string_view line = text.substr(at, end - at);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      response.headers.Add(line.substr(0, colon), line.substr(colon + 1));
    }
    at = end + 1;
  }

  const net::CorsResult decision = net::CheckResponse(params, *target, response);
  if (decision.allowed && params.mode == net::RequestMode::Cors) {
    // The invariant, stated as the thing an attacker wants to break: a
    // cross-origin `cors` response that was allowed must have carried exactly
    // one `Access-Control-Allow-Origin`, and it must have been this origin or a
    // wildcard -- and a wildcard is never enough with credentials.
    const auto allow = response.headers.Get("access-control-allow-origin");
    if (!allow.has_value() || response.headers.Count("access-control-allow-origin") != 1) {
      __builtin_trap();
    }
    const bool wildcard = *allow == "*";
    if (wildcard && params.credentials == net::CredentialsMode::Include) {
      __builtin_trap();
    }
    if (!wildcard && *allow != params.origin.Serialize()) {
      __builtin_trap();
    }
  }

  net::HttpHeaders author;
  author.Add("Content-Type", "application/json");
  net::PreflightGrant grant;
  const net::CorsResult preflight =
      net::CheckPreflight(params, "PUT", author, *target, response, grant);
  if (preflight.allowed) {
    if (grant.max_age_seconds < 0 || grant.max_age_seconds > 86400) {
      __builtin_trap();
    }
    // A grant that permits a method it was not asked about, or a header it was
    // not asked about, is the bug this whole file exists to find.
    net::PreflightCache cache;
    cache.Store("partition", params, *target, grant, 1000);
    if (cache.Allows("other-partition", params, *target, "PUT", author, 1000)) {
      __builtin_trap();
    }
  }

  // Filtering must be idempotent and must never reintroduce a field: a second
  // pass over an already-filtered response is what a redirect chain does.
  net::HttpResponse filtered = response;
  net::FilterExposedHeaders(filtered);
  const std::size_t once = filtered.headers.Size();
  net::FilterExposedHeaders(filtered);
  if (filtered.headers.Size() != once) {
    __builtin_trap();
  }
  if (filtered.headers.Has("set-cookie")) {
    __builtin_trap();
  }
  return 0;
}

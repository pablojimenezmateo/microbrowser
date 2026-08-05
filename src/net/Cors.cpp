#include "net/Cors.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::EqualsAsciiCaseInsensitive;
using util::PerfCounterId;

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// The media type of a `Content-Type`, without its parameters, folded. The
// safelist is about the type alone: `text/plain; charset=utf-8` is safelisted
// and `application/json` is not, whatever follows either.
std::string MediaTypeOf(std::string_view value) {
  const std::size_t semicolon = value.find(';');
  return LowerAscii(util::TrimAscii(value.substr(0, semicolon)));
}

// A comma-separated header value, split and trimmed. `Access-Control-Allow-*`
// are all of this shape, and a server may also send the header more than once.
std::vector<std::string> SplitList(std::string_view value) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= value.size()) {
    const std::size_t comma = value.find(',', at);
    const std::string_view piece =
        value.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
    const std::string_view trimmed = util::TrimAscii(piece);
    if (!trimmed.empty()) {
      out.emplace_back(trimmed);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    at = comma + 1;
  }
  return out;
}

std::vector<std::string> ListHeader(const HttpResponse& response, std::string_view name) {
  std::vector<std::string> out;
  for (const std::string_view field : response.headers.GetAll(name)) {
    for (std::string& piece : SplitList(field)) {
      out.push_back(std::move(piece));
    }
  }
  return out;
}

bool Contains(const std::vector<std::string>& list, std::string_view wanted) {
  return std::any_of(list.begin(), list.end(),
                     [wanted](const std::string& entry) { return entry == wanted; });
}

bool ContainsFolded(const std::vector<std::string>& list, std::string_view wanted) {
  return std::any_of(list.begin(), list.end(), [wanted](const std::string& entry) {
    return EqualsAsciiCaseInsensitive(entry, wanted);
  });
}

CorsResult Refused(std::string reason) {
  AddPerformanceCounter(PerfCounterId::NetCorsBlocked);
  return CorsResult{false, std::move(reason)};
}

// The `Access-Control-Allow-Origin` check, shared by the actual response and by
// the preflight -- which is the point of it being one function. A preflight
// that was checked more loosely than the request it authorises is a preflight
// that grants nothing.
CorsResult CheckAllowOrigin(const CorsParams& params, const HttpResponse& response) {
  // Exactly one, always. Two `Access-Control-Allow-Origin` headers is not a
  // list a browser picks from: it is either a misconfigured server or a
  // response-splitting attempt, and the specification makes both a failure.
  if (response.headers.Count("access-control-allow-origin") != 1) {
    return Refused("no single Access-Control-Allow-Origin on a cross-origin response");
  }
  const std::string allow(util::TrimAscii(*response.headers.Get("access-control-allow-origin")));
  const bool wildcard = allow == "*";
  const bool credentialled = params.credentials == CredentialsMode::Include;

  if (credentialled) {
    // A wildcard is refused with credentials, and that refusal is the whole
    // reason the wildcard is safe to send at all: a server that says `*` has
    // said "anyone may read this", which is only true while the response does
    // not depend on who asked.
    if (wildcard) {
      return Refused("Access-Control-Allow-Origin: * with credentials");
    }
    const auto allow_credentials = response.headers.Get("access-control-allow-credentials");
    if (!allow_credentials.has_value() ||
        !EqualsAsciiCaseInsensitive(util::TrimAscii(*allow_credentials), "true")) {
      return Refused("credentialled request without Access-Control-Allow-Credentials");
    }
  }
  if (wildcard) {
    return CorsResult{true, {}};
  }
  // The comparison is against the serialization the `Origin` header carried,
  // which for a tainted or opaque origin is "null" -- and a server that echoes
  // "null" back has said yes to every opaque origin there is, including a
  // sandboxed frame's. That is the server's decision to make; ours is only to
  // compare the two strings we actually sent and received.
  if (allow != params.origin.Serialize()) {
    return Refused("Access-Control-Allow-Origin does not name this origin");
  }
  return CorsResult{true, {}};
}

}  // namespace

bool IsSameOrigin(const url::Origin& origin, const url::Url& url) {
  return origin.IsSameOrigin(url::Origin::FromUrl(url));
}

bool IsCorsSafelistedMethod(std::string_view method) {
  return method == "GET" || method == "HEAD" || method == "POST";
}

bool IsCorsSafelistedRequestHeader(std::string_view name, std::string_view value) {
  // The length bound is the specification's, and it is not decoration: a
  // safelisted header is one that a form could already have sent, and a
  // 128-kilobyte `Accept` is not.
  if (value.size() > 128) {
    return false;
  }
  if (EqualsAsciiCaseInsensitive(name, "accept") ||
      EqualsAsciiCaseInsensitive(name, "accept-language") ||
      EqualsAsciiCaseInsensitive(name, "content-language")) {
    return true;
  }
  if (EqualsAsciiCaseInsensitive(name, "content-type")) {
    const std::string media = MediaTypeOf(value);
    return media == "application/x-www-form-urlencoded" || media == "multipart/form-data" ||
           media == "text/plain";
  }
  return false;
}

bool IsCorsSafelistedResponseHeader(std::string_view name) {
  return EqualsAsciiCaseInsensitive(name, "cache-control") ||
         EqualsAsciiCaseInsensitive(name, "content-language") ||
         EqualsAsciiCaseInsensitive(name, "content-length") ||
         EqualsAsciiCaseInsensitive(name, "content-type") ||
         EqualsAsciiCaseInsensitive(name, "expires") ||
         EqualsAsciiCaseInsensitive(name, "last-modified") ||
         EqualsAsciiCaseInsensitive(name, "pragma");
}

bool NeedsPreflight(std::string_view method, const HttpHeaders& author_headers) {
  if (!IsCorsSafelistedMethod(method)) {
    return true;
  }
  for (const HttpHeaders::Field& field : author_headers.Fields()) {
    if (!IsCorsSafelistedRequestHeader(field.name, field.value)) {
      return true;
    }
  }
  return false;
}

std::string PreflightRequestHeaderList(const HttpHeaders& author_headers) {
  std::vector<std::string> names;
  for (const HttpHeaders::Field& field : author_headers.Fields()) {
    std::string folded = LowerAscii(field.name);
    if (!Contains(names, folded)) {
      names.push_back(std::move(folded));
    }
  }
  // Sorted, because the specification says sorted and because a server that
  // matches the string rather than the set is common enough that the order
  // being ours rather than the page's decides whether the request works.
  std::sort(names.begin(), names.end());
  std::string out;
  for (const std::string& name : names) {
    if (!out.empty()) {
      out.push_back(',');
    }
    out += name;
  }
  return out;
}

CorsResult CheckResponse(const CorsParams& params, const url::Url& url,
                         const HttpResponse& response) {
  if (params.mode == RequestMode::Browser) {
    return CorsResult{true, {}};
  }
  if (IsSameOrigin(params.origin, url)) {
    return CorsResult{true, {}};
  }
  switch (params.mode) {
    case RequestMode::Browser:
    case RequestMode::NoCors:
      // `no-cors` never fails on a header. It succeeds and answers with
      // nothing, which the caller turns into an opaque response.
      return CorsResult{true, {}};
    case RequestMode::SameOrigin:
      return Refused("same-origin request to a different origin");
    case RequestMode::Cors:
      break;
  }
  return CheckAllowOrigin(params, response);
}

CorsResult CheckPreflight(const CorsParams& params, std::string_view method,
                          const HttpHeaders& author_headers, const url::Url& url,
                          const HttpResponse& response, PreflightGrant& grant) {
  (void)url;
  // A preflight's own status has to be a success. A 404 with the right headers
  // is a server saying the endpoint does not exist, and treating it as
  // permission is how a browser turns a typo into a cross-origin write.
  if (response.status < 200 || response.status > 299) {
    return Refused("preflight did not succeed");
  }
  const CorsResult origin_ok = CheckAllowOrigin(params, response);
  if (!origin_ok.allowed) {
    return origin_ok;
  }

  PreflightGrant made;
  made.methods = ListHeader(response, "access-control-allow-methods");
  made.headers = ListHeader(response, "access-control-allow-headers");
  for (std::string& name : made.headers) {
    name = LowerAscii(name);
  }
  made.allow_all_methods = Contains(made.methods, "*");
  made.allow_all_headers = Contains(made.headers, "*");
  const auto allow_credentials = response.headers.Get("access-control-allow-credentials");
  made.credentials = allow_credentials.has_value() &&
                     EqualsAsciiCaseInsensitive(util::TrimAscii(*allow_credentials), "true");
  // A wildcard means nothing on a credentialled request, exactly as it means
  // nothing in `Access-Control-Allow-Origin` there.
  if (params.credentials == CredentialsMode::Include) {
    made.allow_all_methods = false;
    made.allow_all_headers = false;
  }

  // The method has to be named, unless it is one a form could have sent
  // anyway -- which is the specification's carve-out and the reason a
  // preflighted POST does not need `Access-Control-Allow-Methods: POST`.
  if (!made.allow_all_methods && !Contains(made.methods, std::string(method)) &&
      !IsCorsSafelistedMethod(method)) {
    return Refused("preflight did not allow the method");
  }
  if (!made.allow_all_headers) {
    for (const HttpHeaders::Field& field : author_headers.Fields()) {
      if (IsCorsSafelistedRequestHeader(field.name, field.value)) {
        continue;
      }
      if (!ContainsFolded(made.headers, field.name)) {
        return Refused("preflight did not allow a request header");
      }
    }
  }

  if (const auto max_age = response.headers.Get("access-control-max-age")) {
    std::int64_t seconds = 0;
    bool digits = false;
    for (const char c : util::TrimAscii(*max_age)) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      digits = true;
      // Saturated rather than wrapped. The value is attacker-controlled and a
      // cache entry that never expires because its expiry overflowed is a
      // permission the server can no longer revoke.
      seconds = std::min<std::int64_t>(seconds * 10 + (c - '0'), 86400);
    }
    made.max_age_seconds = digits ? seconds : 0;
  }

  grant = std::move(made);
  return CorsResult{true, {}};
}

void FilterExposedHeaders(HttpResponse& response) {
  const std::vector<std::string> exposed = ListHeader(response, "access-control-expose-headers");
  const bool expose_all = Contains(exposed, "*");
  HttpHeaders kept;
  for (const HttpHeaders::Field& field : response.headers.Fields()) {
    if (IsCorsSafelistedResponseHeader(field.name) || ContainsFolded(exposed, field.name) ||
        // A wildcard exposes everything *except* the headers that carry
        // credentials, which is why `set-cookie` is named here rather than
        // being covered by the safelist above.
        (expose_all && !EqualsAsciiCaseInsensitive(field.name, "set-cookie") &&
         !EqualsAsciiCaseInsensitive(field.name, "set-cookie2"))) {
      kept.Add(field.name, field.value);
    }
  }
  response.headers = std::move(kept);
}

std::string PreflightCache::KeyFor(std::string_view partition, const CorsParams& params,
                                   const url::Url& url) {
  std::string key(partition);
  key.push_back('\n');
  key += params.origin.Serialize();
  key.push_back('\n');
  key += url::Origin::FromUrl(url).Serialize();
  key.push_back('\n');
  // The credential mode is part of the key because the grant is: a preflight
  // answered without `Access-Control-Allow-Credentials` must not authorise a
  // request that carries cookies.
  key.push_back(params.credentials == CredentialsMode::Include ? 'c' : 'u');
  return key;
}

bool PreflightCache::Allows(std::string_view partition, const CorsParams& params,
                            const url::Url& url, std::string_view method,
                            const HttpHeaders& author_headers, std::int64_t now_seconds) const {
  const std::string key = KeyFor(partition, params, url);
  for (const Entry& entry : entries_) {
    if (entry.key != key || entry.expires <= now_seconds) {
      continue;
    }
    if (params.credentials == CredentialsMode::Include && !entry.grant.credentials) {
      return false;
    }
    if (!entry.grant.allow_all_methods && !Contains(entry.grant.methods, std::string(method)) &&
        !IsCorsSafelistedMethod(method)) {
      return false;
    }
    if (entry.grant.allow_all_headers) {
      return true;
    }
    for (const HttpHeaders::Field& field : author_headers.Fields()) {
      if (IsCorsSafelistedRequestHeader(field.name, field.value)) {
        continue;
      }
      if (!ContainsFolded(entry.grant.headers, field.name)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

void PreflightCache::Store(std::string_view partition, const CorsParams& params,
                           const url::Url& url, const PreflightGrant& grant,
                           std::int64_t now_seconds) {
  if (grant.max_age_seconds <= 0) {
    // A grant with no `Access-Control-Max-Age` is not cached at all. Five
    // seconds is what some browsers use as a default; storing nothing is the
    // honest version, and it costs one `OPTIONS` per request to a server that
    // did not ask to be remembered.
    return;
  }
  const std::string key = KeyFor(partition, params, url);
  const std::int64_t expires = now_seconds + grant.max_age_seconds;
  for (Entry& entry : entries_) {
    if (entry.key == key) {
      entry.grant = grant;
      entry.expires = expires;
      return;
    }
  }
  if (entries_.size() >= kMaxPreflightEntries) {
    entries_.erase(entries_.begin());
  }
  entries_.push_back(Entry{key, grant, expires});
}

}  // namespace microbrowser::net

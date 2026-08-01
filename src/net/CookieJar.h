#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::net {

enum class SameSite : std::uint8_t {
  // The default when a cookie says nothing. Lax rather than None, because a
  // cookie that is sent on every cross-site request is a CSRF token for
  // whoever wants one, and defaulting to the permissive answer means every
  // site that never thought about it gets the permissive answer.
  Lax,
  Strict,
  None,
};

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;  // without a leading dot; empty means host-only
  std::string path = "/";
  // Seconds since the epoch. Nullopt is a session cookie, which is not the
  // same as an expired one.
  std::optional<std::int64_t> expires;
  bool secure = false;
  bool http_only = false;
  bool host_only = true;
  SameSite same_site = SameSite::Lax;

  bool IsExpiredAt(std::int64_t now) const { return expires.has_value() && *expires <= now; }
};

// Parses one `Set-Cookie` field value. Nullopt when the cookie is malformed or
// is one this jar refuses to store.
//
// `request_url` is needed because a cookie's legality depends on where it came
// from: a page may not set a cookie for a domain it does not belong to, and
// "does not belong to" is decided against the Public Suffix List — otherwise
// any site could set a cookie for `.com` and read it everywhere.
std::optional<Cookie> ParseSetCookie(std::string_view field, const url::Url& request_url,
                                     std::int64_t now);

// Cookie storage, partitioned.
//
// **Every operation takes a `url::PartitionKey`, and there is no overload
// without one.** That is the same technique `net::Fetch` uses for
// `privacy::Verdict`: Total Cookie Protection stops being a policy that could
// be switched off and becomes a signature that cannot be called incorrectly.
// A third party embedded on two sites gets two jars, because it gets two keys.
class CookieJar {
 public:
  // Returns false when the cookie was refused. Refusal is routine — a page
  // trying to set a cookie for a domain it does not own is a normal thing for a
  // page to try.
  bool Store(const url::PartitionKey& key, const url::Url& request_url, const Cookie& cookie,
             std::int64_t now);

  // Convenience for a whole `Set-Cookie` header value.
  bool StoreFromHeader(const url::PartitionKey& key, const url::Url& request_url,
                       std::string_view field, std::int64_t now);

  // Cookies to send, already filtered by domain, path, secure, and same-site,
  // in the order RFC 6265 requires: longest path first, then oldest first.
  std::vector<Cookie> CookiesFor(const url::PartitionKey& key, const url::Url& request_url,
                                 bool same_site_context, bool is_top_level_navigation,
                                 std::int64_t now) const;

  // The `Cookie:` header value, or empty when there is nothing to send.
  std::string HeaderFor(const url::PartitionKey& key, const url::Url& request_url,
                        bool same_site_context, bool is_top_level_navigation,
                        std::int64_t now) const;

  void RemoveExpired(std::int64_t now);
  void Clear() { entries_.clear(); }
  std::size_t Size() const { return entries_.size(); }

  // Everything an ephemeral container stored, dropped. A private window closing
  // is this call and nothing else.
  void ClearContainer(url::ContainerId container);

 private:
  struct Entry {
    url::PartitionKey key;
    Cookie cookie;
    std::int64_t created = 0;
  };

  // A flat vector rather than a map. The jar is small, the match path needs the
  // whole partition key *and* a domain-suffix walk, and a map keyed on part of
  // that would invite a lookup that used part of the key — which is the exact
  // failure the key type exists to prevent.
  std::vector<Entry> entries_;
  std::int64_t sequence_ = 0;
};

// Whether `host` is covered by a cookie's `Domain`. Exposed because it is the
// rule most often written wrongly: `notevil.com` must not match `evil.com`, and
// a naive suffix comparison says it does.
bool CookieDomainMatches(std::string_view host, std::string_view domain);

// RFC 6265 path matching, which is not a prefix comparison: `/foo` matches
// `/foo` and `/foo/bar` but not `/foobar`.
bool CookiePathMatches(std::string_view request_path, std::string_view cookie_path);

}  // namespace microbrowser::net

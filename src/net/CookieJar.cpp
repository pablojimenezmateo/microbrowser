#include "net/CookieJar.h"

#include <algorithm>

#include "url/PublicSuffixList.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) { return ToLower(x) == ToLower(y); });
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// The default path: everything up to and including the last slash, with a
// trailing-slash-only path becoming "/". Straight from RFC 6265 section 5.1.4.
std::string DefaultPath(const url::Url& url) {
  const std::string path = url.PathString();
  if (path.empty() || path.front() != '/') {
    return "/";
  }
  const std::size_t last = path.rfind('/');
  if (last == 0) {
    return "/";
  }
  return path.substr(0, last);
}

}  // namespace

bool CookieDomainMatches(std::string_view host, std::string_view domain) {
  if (domain.empty()) {
    return false;
  }
  if (host == domain) {
    return true;
  }
  // The suffix must fall on a label boundary. Without that check `notevil.com`
  // matches `evil.com`, which is the whole attack.
  if (host.size() <= domain.size()) {
    return false;
  }
  if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) {
    return false;
  }
  return host[host.size() - domain.size() - 1] == '.';
}

bool CookiePathMatches(std::string_view request_path, std::string_view cookie_path) {
  if (cookie_path.empty()) {
    return false;
  }
  if (request_path == cookie_path) {
    return true;
  }
  if (request_path.size() <= cookie_path.size() ||
      request_path.compare(0, cookie_path.size(), cookie_path) != 0) {
    return false;
  }
  // `/foo` covers `/foo/bar` but not `/foobar`.
  return cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
}

std::optional<Cookie> ParseSetCookie(std::string_view field, const url::Url& request_url,
                                     std::int64_t now) {
  const std::size_t semicolon = field.find(';');
  const std::string_view pair = Trim(semicolon == std::string_view::npos ? field
                                                                        : field.substr(0, semicolon));
  const std::size_t equals = pair.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    // A cookie with no name is not storable. Some servers send one; treating it
    // as a nameless cookie means two of them collide in ways nothing defines.
    return std::nullopt;
  }

  Cookie cookie;
  cookie.name = std::string(Trim(pair.substr(0, equals)));
  cookie.value = std::string(Trim(pair.substr(equals + 1)));
  if (cookie.name.empty()) {
    return std::nullopt;
  }
  // A control character in a cookie is header injection aimed at whatever
  // reads the jar back out.
  const auto has_control = [](std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
      return static_cast<unsigned char>(c) < 0x20 || c == ';' || static_cast<unsigned char>(c) == 0x7F;
    });
  };
  if (has_control(cookie.name) || has_control(cookie.value)) {
    return std::nullopt;
  }

  cookie.path = DefaultPath(request_url);
  cookie.domain = request_url.HostSerialized();
  cookie.host_only = true;

  std::optional<std::int64_t> max_age;
  std::optional<std::int64_t> expires;

  std::size_t position = semicolon == std::string_view::npos ? field.size() : semicolon + 1;
  while (position < field.size()) {
    const std::size_t next = field.find(';', position);
    const std::string_view attribute =
        Trim(next == std::string_view::npos ? field.substr(position) : field.substr(position, next - position));
    position = next == std::string_view::npos ? field.size() : next + 1;
    if (attribute.empty()) {
      continue;
    }

    const std::size_t attribute_equals = attribute.find('=');
    const std::string_view name = Trim(attribute.substr(0, attribute_equals));
    const std::string_view value =
        attribute_equals == std::string_view::npos ? std::string_view()
                                                   : Trim(attribute.substr(attribute_equals + 1));

    if (EqualsIgnoringCase(name, "secure")) {
      cookie.secure = true;
    } else if (EqualsIgnoringCase(name, "httponly")) {
      cookie.http_only = true;
    } else if (EqualsIgnoringCase(name, "path")) {
      if (!value.empty() && value.front() == '/') {
        cookie.path = std::string(value);
      }
    } else if (EqualsIgnoringCase(name, "domain")) {
      std::string_view domain = value;
      if (!domain.empty() && domain.front() == '.') {
        domain.remove_prefix(1);
      }
      if (!domain.empty()) {
        cookie.domain = Lowered(domain);
        cookie.host_only = false;
      }
    } else if (EqualsIgnoringCase(name, "max-age")) {
      const auto seconds = util::ParseInt64(value);
      if (seconds.has_value()) {
        // Max-Age wins over Expires when both appear, which is what the RFC
        // says and what every implementation does.
        max_age = *seconds <= 0 ? std::int64_t{0} : util::SaturatingSignedAdd(now, *seconds);
      }
    } else if (EqualsIgnoringCase(name, "expires")) {
      // Only the epoch-seconds form is parsed. HTTP-date parsing needs a real
      // date parser with three legacy formats, and guessing at a date means
      // guessing at an expiry — a cookie that outlives its instructions is a
      // privacy bug. An unparsed Expires makes the cookie a session cookie,
      // which is the conservative answer.
      const auto seconds = util::ParseInt64(value);
      if (seconds.has_value()) {
        expires = *seconds;
      }
    } else if (EqualsIgnoringCase(name, "samesite")) {
      if (EqualsIgnoringCase(value, "strict")) {
        cookie.same_site = SameSite::Strict;
      } else if (EqualsIgnoringCase(value, "none")) {
        cookie.same_site = SameSite::None;
      } else {
        cookie.same_site = SameSite::Lax;
      }
    }
  }

  cookie.expires = max_age.has_value() ? max_age : expires;

  // `SameSite=None` without `Secure` is refused. It is the combination that
  // asks to be sent on every cross-site request over plain HTTP, which is a
  // tracking cookie with the safety catch removed.
  if (cookie.same_site == SameSite::None && !cookie.secure) {
    return std::nullopt;
  }
  if (StartsWith(cookie.name, "__Secure-") && !cookie.secure) {
    return std::nullopt;
  }
  if (StartsWith(cookie.name, "__Host-") &&
      (!cookie.secure || !cookie.host_only || cookie.path != "/")) {
    return std::nullopt;
  }
  return cookie;
}

bool CookieJar::Store(const url::PartitionKey& key, const url::Url& request_url,
                      const Cookie& cookie, std::int64_t now) {
  const std::string host = request_url.HostSerialized();

  if (!cookie.host_only) {
    // A page may only widen a cookie to a domain it belongs to, and never to a
    // public suffix. Without the second check any site could set a cookie for
    // `.com` and read it on every other.
    if (!CookieDomainMatches(host, cookie.domain)) {
      AddPerformanceCounter(PerfCounterId::NetCookiesRejected);
      return false;
    }
    if (url::IsPublicSuffix(cookie.domain)) {
      AddPerformanceCounter(PerfCounterId::NetCookiesRejected);
      return false;
    }
  } else if (cookie.domain != host) {
    AddPerformanceCounter(PerfCounterId::NetCookiesRejected);
    return false;
  }

  if (cookie.secure && !url::Origin::FromUrl(request_url).IsPotentiallyTrustworthy()) {
    // A Secure cookie set over plain HTTP would let a network attacker plant
    // one that the https site then trusts.
    AddPerformanceCounter(PerfCounterId::NetCookiesRejected);
    return false;
  }

  // Replacing rather than appending: a cookie is identified by name, domain and
  // path *within its partition*, and a jar that appended would grow forever and
  // send both.
  const auto same = [&](const Entry& entry) {
    return entry.key == key && entry.cookie.name == cookie.name &&
           entry.cookie.domain == cookie.domain && entry.cookie.path == cookie.path;
  };
  const auto found = std::find_if(entries_.begin(), entries_.end(), same);
  const std::int64_t created = found != entries_.end() ? found->created : ++sequence_;
  if (found != entries_.end()) {
    entries_.erase(found);
  }

  if (cookie.IsExpiredAt(now)) {
    // Setting an expired cookie is how a server deletes one. The removal above
    // is the whole operation.
    return true;
  }

  entries_.push_back(Entry{key, cookie, created});
  AddPerformanceCounter(PerfCounterId::NetCookiesStored);
  return true;
}

bool CookieJar::StoreFromHeader(const url::PartitionKey& key, const url::Url& request_url,
                                std::string_view field, std::int64_t now) {
  const auto cookie = ParseSetCookie(field, request_url, now);
  if (!cookie.has_value()) {
    AddPerformanceCounter(PerfCounterId::NetCookiesRejected);
    return false;
  }
  return Store(key, request_url, *cookie, now);
}

std::vector<Cookie> CookieJar::CookiesFor(const url::PartitionKey& key,
                                          const url::Url& request_url, bool same_site_context,
                                          bool is_top_level_navigation, std::int64_t now) const {
  const std::string host = request_url.HostSerialized();
  const std::string path = request_url.PathString().empty() ? "/" : request_url.PathString();
  const bool secure = url::Origin::FromUrl(request_url).IsPotentiallyTrustworthy();

  std::vector<const Entry*> matched;
  for (const Entry& entry : entries_) {
    if (!(entry.key == key)) {
      continue;  // a different partition is a different jar
    }
    const Cookie& cookie = entry.cookie;
    if (cookie.IsExpiredAt(now)) {
      continue;
    }
    if (cookie.host_only ? cookie.domain != host : !CookieDomainMatches(host, cookie.domain)) {
      continue;
    }
    if (!CookiePathMatches(path, cookie.path)) {
      continue;
    }
    if (cookie.secure && !secure) {
      continue;
    }
    if (!same_site_context) {
      if (cookie.same_site == SameSite::Strict) {
        continue;
      }
      // Lax travels on a top-level navigation and on nothing else, which is
      // what makes it a CSRF defence rather than a label.
      if (cookie.same_site == SameSite::Lax && !is_top_level_navigation) {
        continue;
      }
    }
    matched.push_back(&entry);
  }

  // RFC 6265 order: longer paths first, then by creation time. Servers do
  // depend on it, and a stable order is also one fewer thing to fingerprint.
  std::sort(matched.begin(), matched.end(), [](const Entry* a, const Entry* b) {
    if (a->cookie.path.size() != b->cookie.path.size()) {
      return a->cookie.path.size() > b->cookie.path.size();
    }
    return a->created < b->created;
  });

  std::vector<Cookie> result;
  result.reserve(matched.size());
  for (const Entry* entry : matched) {
    result.push_back(entry->cookie);
  }
  return result;
}

std::string CookieJar::HeaderFor(const url::PartitionKey& key, const url::Url& request_url,
                                 bool same_site_context, bool is_top_level_navigation,
                                 std::int64_t now) const {
  const std::vector<Cookie> cookies =
      CookiesFor(key, request_url, same_site_context, is_top_level_navigation, now);
  std::string out;
  for (const Cookie& cookie : cookies) {
    if (!out.empty()) {
      out += "; ";
    }
    out += cookie.name;
    out.push_back('=');
    out += cookie.value;
  }
  return out;
}

void CookieJar::RemoveExpired(std::int64_t now) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [now](const Entry& entry) { return entry.cookie.IsExpiredAt(now); }),
                 entries_.end());
}

void CookieJar::ClearContainer(url::ContainerId container) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [container](const Entry& entry) {
                                  return entry.key.Container() == container;
                                }),
                 entries_.end());
}

}  // namespace microbrowser::net

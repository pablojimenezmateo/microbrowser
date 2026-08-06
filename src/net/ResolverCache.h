#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

namespace microbrowser::net {

// How long a resolved name is kept.
//
// Short on purpose. The whole value of this cache is *within one page load*,
// where a page with subresources on six hosts resolves each of them once per
// connection it opens -- measured at four to six times per host on
// old.reddit.com and on youtube.com, each one a blocking call that stops the
// loop. A minute covers a load and the click that follows it, and covers
// nothing after that. The cost of being too short is one `getaddrinfo`; the
// cost of being too long is serving an address a server has moved off, which is
// a correctness bug wearing a performance fix's clothes.
inline constexpr std::int64_t kResolvedNameTtlMs = 60000;

// How many names are kept across every key at once. A page controls how many
// hosts it names, so this is bounded by the browser rather than by the page.
inline constexpr std::size_t kMaxResolvedNames = 64;

// Resolved addresses for one name.
//
// A copy of what `getaddrinfo` produced rather than its `addrinfo` list, which
// is owned by the resolver and freed with it. Each entry is everything
// `socket()` and `connect()` need and nothing else.
struct ResolvedAddress {
  int family = 0;
  int socket_type = 0;
  int protocol = 0;
  sockaddr_storage address{};
  socklen_t address_length = 0;
};

// Names already resolved, so the same host is not looked up once per connection.
//
// **Keyed by the ADR 0005 partition key, not by host, and that is the whole
// privacy content of this file.** A shared resolver cache is directly
// observable from script: a name another site has already resolved answers in
// microseconds and a cold one takes tens of milliseconds, so a cache keyed by
// host alone lets any page ask "has this browser been to that one?" -- the same
// cross-site inference the connection pool is partitioned to prevent and the
// same reason TLS session tickets are off (ADR 0005). Two sites that name the
// same CDN host resolve it twice here, and that is the feature.
//
// The key is a *parameter of the lookup* rather than something this class
// derives, and `SocketTransport::StartConnect` takes a partition for the same
// reason `Fetch` takes a `privacy::Verdict`: there is then no path from a
// request to a name lookup that does not go past the key, which is what makes
// the rule structural instead of remembered.
class ResolverCache {
 public:
  // The addresses for this name, or nothing when it is unknown or expired.
  const std::vector<ResolvedAddress>* Lookup(std::string_view partition, std::string_view host,
                                             std::uint16_t port, std::int64_t now_ms);

  // Records a successful resolution. A *failed* one is deliberately not stored:
  // a name that did not resolve is usually a name that was not reachable yet,
  // and caching that turns a transient outage into a page that stays broken
  // until a timeout nobody can see expires.
  void Store(std::string_view partition, std::string_view host, std::uint16_t port,
             std::vector<ResolvedAddress> addresses, std::int64_t now_ms);

  // Drops everything. The factory swap needs it, and so does any test that
  // wants a cold cache without waiting a minute for one.
  void Clear();

  std::size_t Size() const { return entries_.size(); }

 private:
  struct Entry {
    std::string key;
    std::vector<ResolvedAddress> addresses;
    std::int64_t stored_ms = 0;
  };

  std::vector<Entry> entries_;
};

// The cache key, serialized. Exposed for the same reason `ConnectionKey` is: a
// test that asserts two lookups do *not* share an entry has to be able to say
// which two, and writing the concatenation twice is how the partition would
// eventually fall out of one of them.
std::string ResolverKey(std::string_view partition, std::string_view host, std::uint16_t port);

}  // namespace microbrowser::net

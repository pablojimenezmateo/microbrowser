#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/Transport.h"

namespace microbrowser::net {

// How long an idle connection is kept before it is closed.
//
// Reuse is worth having across a page load and across the click that follows
// it, and it is worth nothing after that: a user who has moved on is a user
// whose browser is holding a socket open to a server for no reason. Thirty
// seconds covers both of those and neither of the ones after. The cost of
// being too short is one handshake; the cost of being too long is a connection
// the user did not ask to keep, which ADR 0010 calls part of the privacy
// surface rather than a resource decision.
inline constexpr std::int64_t kIdleConnectionTimeoutMs = 30000;

// How many idle connections may be held across every key at once.
//
// The per-partition concurrency bound already caps how many a single partition
// can have released to it; this is the bound across *keys*, of which there is
// no natural limit — a page with subresources on thirty hosts would otherwise
// leave thirty sockets behind. Oldest goes first when it binds.
inline constexpr std::size_t kMaxIdleConnections = 24;

// Connections, kept and handed back out.
//
// **The pool is keyed by the ADR 0005 partition key, not by host, and that is
// the whole privacy content of this file.** A reused connection is a linkage
// between two requests that the server can observe directly: same socket, same
// TLS session, therefore same client. Pooling by host would create exactly the
// cross-site correlation the partition key exists to prevent, and it would
// create it in a data structure rather than in a policy flag — which is the
// failure mode ADR 0005 was written to make impossible. Two sites that happen
// to share a CDN host get two connections here, and that is the feature.
//
// It also owns the factory, and that is not incidental either: with the pool in
// the way there is no path from a request to a fresh socket that does not go
// past the key. `Fetch` takes a pool for the same reason it takes a Verdict.
class ConnectionPool {
 public:
  // One connection, and whether it had already been through a handshake. The
  // caller drives it identically either way — an open transport answers `Ready`
  // to `Advance()` — so `reused` exists for the counters and for deciding
  // whether a failure with nothing received deserves one retry on a fresh
  // socket.
  struct Lease {
    std::unique_ptr<Transport> connection;
    bool reused = false;
  };

  explicit ConnectionPool(TransportFactory& factory) : factory_(&factory) {}

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  // An idle connection for this key, or a new one. `allow_reuse` is false on
  // the retry that follows a pooled connection turning out to be dead: taking
  // another one from the same key would be the same bet twice.
  //
  // A connection out on lease is not in the pool. Nothing is shared and nothing
  // is handed out twice.
  Lease Acquire(std::string_view partition, std::string_view host, std::uint16_t port, bool secure,
                bool allow_reuse);

  // Takes a connection back. Only ever called for one whose response was
  // completely read and self-delimited — a stream whose end nobody is sure of
  // is a stream whose next response starts at an unknown byte, which is request
  // smuggling with the browser playing both parsers.
  void Release(std::string_view partition, std::string_view host, std::uint16_t port, bool secure,
               std::unique_ptr<Transport> connection, std::int64_t now_ms);

  // Closes everything past its idle timeout.
  void CloseExpired(std::int64_t now_ms);

  // Milliseconds until the soonest idle connection expires, or nothing when the
  // pool is empty. This is what keeps the idle timer inside the zero-idle-CPU
  // invariant: a browser holding no connections schedules no wakeup, and one
  // holding connections schedules exactly one, after which it holds none.
  std::optional<std::uint32_t> NextDeadlineMs(std::int64_t now_ms) const;

  // Drops every idle connection. The factory swap needs it — a connection made
  // by a factory that is gone is a descriptor nobody owns.
  void Clear();
  void SetFactory(TransportFactory& factory);

  std::size_t IdleCount() const { return idle_.size(); }

 private:
  struct Idle {
    std::string key;
    std::unique_ptr<Transport> connection;
    std::int64_t since_ms = 0;
  };

  TransportFactory* factory_;
  std::vector<Idle> idle_;
};

// The pool key, serialized. Exposed because a test that asserts two requests
// share a connection has to be able to say which two, and because writing the
// concatenation twice is how the partition would eventually fall out of it.
std::string ConnectionKey(std::string_view partition, std::string_view host, std::uint16_t port,
                          bool secure);

}  // namespace microbrowser::net

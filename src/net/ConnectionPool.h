#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/Http2Session.h"
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

// How many origins the pool remembers the protocol of.
//
// Small on purpose. It is an optimisation -- it saves the *second* page load to
// a site one serialized connect -- and every entry is a fact about where the
// user has been. Bounded and partition-keyed, it is a cache; unbounded, it
// would be a history.
inline constexpr std::size_t kMaxRememberedProtocols = 32;

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
//
// **HTTP/2 changes what this class is.** Under HTTP/1.1 a connection is out on
// loan for the length of one request and comes back afterwards; an HTTP/2
// session is *shared* while it is being used, by as many requests as the server
// will take. So the pool now hands out two different things, and one of them is
// a `shared_ptr`.
//
// The hard part is timing, and it is worth stating before the code. Which
// protocol an origin speaks is decided by ALPN, during the TLS handshake —
// which is to say *after* a socket has been opened. Six concurrent images on
// one host would therefore open six sockets, discover six times that the server
// speaks HTTP/2, and end up with six sessions carrying one stream each. That is
// the HTTP/1.1 burst wearing a new protocol, and on `upload.wikimedia.org` it
// is what answers 429 (TD-0008).
//
// So the pool serialises the *first* connect to an origin whose protocol it
// does not know: one request connects, the rest are told to wait and ask again.
// The moment the handshake answers, either a session exists for them to join or
// the origin is known to speak HTTP/1.1 and the bound comes off. Every browser
// does this and none of them can avoid it — the alternative is to guess.
class ConnectionPool {
 public:
  // What one request got. Exactly one of `connection` and `session` is set,
  // unless `wait_for_protocol` is true, in which case neither is: somebody else
  // is finding out what this origin speaks and the caller should come back.
  struct Lease {
    // An HTTP/1.1 connection, on loan. Also what a *new* connection comes back
    // as, before its handshake has said which protocol it is.
    std::unique_ptr<Transport> connection;
    // An HTTP/2 session, shared with every other request on the same origin in
    // the same partition. Alive as long as anybody holds it.
    std::shared_ptr<Http2Session> session;
    // Whether it had already been through a handshake. The caller drives it
    // identically either way — an open transport answers `Ready` to
    // `Advance()` — so this exists for the counters and for deciding whether a
    // failure with nothing received deserves one retry on a fresh socket.
    bool reused = false;
    // A connect to this origin is already under way and nobody yet knows which
    // protocol it will speak. Nothing was handed over; ask again on a later
    // turn. See `PendingConnects`, which is what stops that from being a hang.
    bool wait_for_protocol = false;
    // This lease is the one doing the connecting, and it owes the pool an
    // answer: `AdoptHttp2`, `FinishedHttp1` or `AbandonConnect`. Giving none of
    // them back would park every other request for this origin.
    bool owns_connect = false;
    // The serialized key this lease was taken under, so the caller can hand it
    // back without re-deriving it from a URL a redirect may have changed.
    std::string key;
  };

  explicit ConnectionPool(TransportFactory& factory) : factory_(&factory) {}
  ~ConnectionPool();

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  // A live session for this key, an idle connection, or a new one — or an
  // instruction to wait. `allow_reuse` is false on the retry that follows a
  // pooled connection turning out to be dead: taking another one from the same
  // key would be the same bet twice.
  //
  // A connection out on lease is not in the pool. Nothing is shared and nothing
  // is handed out twice — except a session, which is shared on purpose.
  Lease Acquire(std::string_view partition, std::string_view host, std::uint16_t port, bool secure,
                bool allow_reuse);

  // The handshake finished and ALPN said `h2`. The pool takes the socket, wraps
  // it in a session, files it under the key and hands it back. Every later
  // request for the same key joins this one rather than opening its own.
  std::shared_ptr<Http2Session> AdoptHttp2(const std::string& key,
                                           std::unique_ptr<Transport> connection);
  // The handshake finished and this origin speaks HTTP/1.1. Recorded, so the
  // next six requests to it do not queue behind each other — a bound that is
  // right while the protocol is unknown is wrong the moment it is known.
  void FinishedHttp1(const std::string& key);
  // The connect failed, or the request that was making it went away. Releases
  // the claim without deciding anything.
  void AbandonConnect(const std::string& key);

  // How many connects are in flight with their protocol still unknown. The
  // request queue asks, because a request parked on `wait_for_protocol` is
  // woken by the socket of whoever is connecting — and if that request has been
  // cancelled, this is what says the parked one is runnable again rather than
  // leaving it to the stall deadline.
  std::size_t PendingConnects() const { return claims_.size(); }

  // Takes a connection back. Only ever called for one whose response was
  // completely read and self-delimited — a stream whose end nobody is sure of
  // is a stream whose next response starts at an unknown byte, which is request
  // smuggling with the browser playing both parsers.
  void Release(std::string_view partition, std::string_view host, std::uint16_t port, bool secure,
               std::unique_ptr<Transport> connection, std::int64_t now_ms);

  // Closes everything past its idle timeout, and advances every idle session.
  //
  // The advance is not housekeeping. An HTTP/2 session with no requests on it
  // is still a live connection the server may be saying GOAWAY on, and a
  // session that never reads is one that hands a dead socket to the next
  // request. Nothing here blocks: it is one non-blocking read per idle session,
  // and there are at most a handful.
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
  // Live HTTP/2 sessions. A test asserts that six concurrent requests to one
  // origin produced exactly one of these, which is the whole of TD-0008.
  std::size_t SessionCount() const { return sessions_.size(); }

 private:
  struct Idle {
    std::string key;
    std::unique_ptr<Transport> connection;
    std::int64_t since_ms = 0;
  };

  struct Session {
    std::string key;
    std::shared_ptr<Http2Session> session;
    // When it last had no streams on it. An idle session times out like an idle
    // connection does, and for the same reason: a socket the user did not ask
    // to keep open is part of the privacy surface, not a resource decision.
    std::int64_t idle_since_ms = 0;
  };

  // What an origin was last seen to speak. **Keyed by the ADR 0005 partition
  // key like everything else in this file**, and that is not decoration: "has
  // this browser learned that example.com speaks HTTP/2?" is a question about
  // where the user has been, and a memo keyed by host would answer it across
  // sites. Same argument as the resolver cache.
  enum class Protocol : std::uint8_t { Unknown, Http1, Http2 };

  struct Known {
    std::string key;
    Protocol protocol = Protocol::Unknown;
  };

  Protocol ProtocolFor(const std::string& key) const;
  void Remember(const std::string& key, Protocol protocol);

  TransportFactory* factory_;
  std::vector<Idle> idle_;
  std::vector<Session> sessions_;
  // The keys with a connect in flight whose protocol is not yet known. A string
  // per outstanding connect, and at most one per origin — that is the point.
  std::vector<std::string> claims_;
  std::vector<Known> protocols_;
};

// The pool key, serialized. Exposed because a test that asserts two requests
// share a connection has to be able to say which two, and because writing the
// concatenation twice is how the partition would eventually fall out of it.
std::string ConnectionKey(std::string_view partition, std::string_view host, std::uint16_t port,
                          bool secure);

}  // namespace microbrowser::net

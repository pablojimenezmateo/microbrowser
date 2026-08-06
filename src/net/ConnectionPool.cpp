#include "net/ConnectionPool.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

std::string ConnectionKey(std::string_view partition, std::string_view host, std::uint16_t port,
                          bool secure) {
  // The partition first, because it is the part that must never be dropped by
  // an edit to the rest of it. A key that read host-first would still work and
  // would read as though the host were the identity.
  std::string key(partition);
  key += secure ? "|https|" : "|http|";
  key += host;
  key.push_back(':');
  key += std::to_string(port);
  return key;
}

ConnectionPool::~ConnectionPool() = default;

ConnectionPool::Protocol ConnectionPool::ProtocolFor(const std::string& key) const {
  for (const Known& entry : protocols_) {
    if (entry.key == key) {
      return entry.protocol;
    }
  }
  return Protocol::Unknown;
}

void ConnectionPool::Remember(const std::string& key, Protocol protocol) {
  for (Known& entry : protocols_) {
    if (entry.key == key) {
      entry.protocol = protocol;
      return;
    }
  }
  if (protocols_.size() >= kMaxRememberedProtocols) {
    protocols_.erase(protocols_.begin());
  }
  protocols_.push_back(Known{key, protocol});
}

ConnectionPool::Lease ConnectionPool::Acquire(std::string_view partition, std::string_view host,
                                              std::uint16_t port, bool secure, bool allow_reuse) {
  const std::string key = ConnectionKey(partition, host, port, secure);

  // A live session first, and before the idle-connection list, because an
  // origin that speaks HTTP/2 has no HTTP/1.1 connections to find. `allow_reuse`
  // is false on the one retry a dead pooled connection buys, and a session is
  // pooled in exactly that sense: it may also have died while nobody looked.
  if (allow_reuse) {
    for (Session& entry : sessions_) {
      if (entry.key == key && entry.session->Capacity() > 0) {
        AddPerformanceCounter(PerfCounterId::NetConnectionsReused);
        Lease lease;
        lease.session = entry.session;
        lease.reused = true;
        lease.key = key;
        return lease;
      }
    }
    // Newest first: the connection least likely to have been closed by a peer
    // that got bored of it.
    for (auto entry = idle_.rbegin(); entry != idle_.rend(); ++entry) {
      if (entry->key != key) {
        continue;
      }
      Lease lease;
      lease.connection = std::move(entry->connection);
      lease.reused = true;
      lease.key = key;
      idle_.erase(std::next(entry).base());
      AddPerformanceCounter(PerfCounterId::NetConnectionsReused);
      return lease;
    }
  }

  // Nothing to join, so a socket has to be opened. The question is whether this
  // request may open one *now*.
  //
  // It may, without waiting, when the answer cannot be HTTP/2: a plaintext
  // origin never negotiates (there is no handshake to negotiate in, and h2c is
  // not offered), and an origin already known to speak HTTP/1.1 wants its six
  // parallel connections rather than one at a time. A bound that is right while
  // the protocol is unknown is wrong the moment it is known.
  const Protocol known = ProtocolFor(key);
  const bool protocol_is_in_question = secure && known == Protocol::Unknown;
  if (protocol_is_in_question) {
    const bool somebody_is_connecting =
        std::find(claims_.begin(), claims_.end(), key) != claims_.end();
    if (somebody_is_connecting) {
      // Parked. This is the whole of the coalescing, and it is worth being
      // clear that it costs latency on the *first* request to an origin and
      // buys a single connection for the next nineteen.
      AddPerformanceCounter(PerfCounterId::NetHttp2ConnectWaits);
      Lease lease;
      lease.wait_for_protocol = true;
      lease.key = key;
      return lease;
    }
    claims_.push_back(key);
  }

  Lease lease;
  lease.connection = factory_->Create();
  lease.owns_connect = protocol_is_in_question;
  lease.key = key;
  return lease;
}

std::shared_ptr<Http2Session> ConnectionPool::AdoptHttp2(const std::string& key,
                                                        std::unique_ptr<Transport> connection) {
  AbandonConnect(key);
  Remember(key, Protocol::Http2);
  if (connection == nullptr) {
    return nullptr;
  }
  auto session = std::make_shared<Http2Session>(std::move(connection));
  Session entry;
  entry.key = key;
  entry.session = session;
  entry.idle_since_ms = 0;
  sessions_.push_back(std::move(entry));
  return session;
}

void ConnectionPool::FinishedHttp1(const std::string& key) {
  AbandonConnect(key);
  Remember(key, Protocol::Http1);
}

void ConnectionPool::AbandonConnect(const std::string& key) {
  const auto end = std::remove(claims_.begin(), claims_.end(), key);
  claims_.erase(end, claims_.end());
}

void ConnectionPool::Release(std::string_view partition, std::string_view host, std::uint16_t port,
                             bool secure, std::unique_ptr<Transport> connection,
                             std::int64_t now_ms) {
  if (connection == nullptr) {
    return;
  }
  if (idle_.size() >= kMaxIdleConnections) {
    // The oldest goes, not the one arriving: the one arriving is the one a page
    // is most likely to want next.
    idle_.front().connection->Close();
    idle_.erase(idle_.begin());
    AddPerformanceCounter(PerfCounterId::NetConnectionsClosedIdle);
  }
  Idle entry;
  entry.key = ConnectionKey(partition, host, port, secure);
  entry.connection = std::move(connection);
  entry.since_ms = now_ms;
  idle_.push_back(std::move(entry));
  AddPerformanceCounter(PerfCounterId::NetConnectionsPooled);
}

void ConnectionPool::CloseExpired(std::int64_t now_ms) {
  const auto expired = [now_ms](const Idle& entry) {
    return now_ms - entry.since_ms >= kIdleConnectionTimeoutMs;
  };
  for (Idle& entry : idle_) {
    if (expired(entry)) {
      entry.connection->Close();
      AddPerformanceCounter(PerfCounterId::NetConnectionsClosedIdle);
    }
  }
  idle_.erase(std::remove_if(idle_.begin(), idle_.end(), expired), idle_.end());

  // Sessions. Two reasons one goes: it broke, or nobody has used it for long
  // enough. The advance is what finds the first — a session nobody reads is a
  // session that has not seen the GOAWAY it was sent, and handing that socket
  // to the next request costs a whole round trip to discover.
  for (Session& entry : sessions_) {
    if (entry.session->OpenStreams() == 0) {
      if (entry.idle_since_ms == 0) {
        entry.idle_since_ms = now_ms;
      }
      entry.session->Advance();
    } else {
      entry.idle_since_ms = 0;
    }
  }
  const auto session_expired = [now_ms](const Session& entry) {
    if (entry.session->Failed()) {
      return true;
    }
    // Held elsewhere, so somebody is still using it whatever its stream count
    // says at this instant.
    if (entry.session.use_count() > 1) {
      return false;
    }
    return entry.idle_since_ms != 0 && now_ms - entry.idle_since_ms >= kIdleConnectionTimeoutMs;
  };
  const auto end = std::remove_if(sessions_.begin(), sessions_.end(), session_expired);
  if (end != sessions_.end()) {
    AddPerformanceCounter(PerfCounterId::NetConnectionsClosedIdle);
  }
  sessions_.erase(end, sessions_.end());
}

std::optional<std::uint32_t> ConnectionPool::NextDeadlineMs(std::int64_t now_ms) const {
  std::optional<std::uint32_t> soonest;
  const auto consider = [&soonest, now_ms](std::int64_t since_ms) {
    const std::int64_t due = since_ms + kIdleConnectionTimeoutMs;
    const std::int64_t remaining = std::max<std::int64_t>(0, due - now_ms);
    const auto capped = static_cast<std::uint32_t>(
        std::min<std::int64_t>(remaining, static_cast<std::int64_t>(UINT32_MAX)));
    soonest = soonest.has_value() ? std::min(*soonest, capped) : capped;
  };
  for (const Idle& entry : idle_) {
    consider(entry.since_ms);
  }
  for (const Session& entry : sessions_) {
    // A session with requests on it is not idle and schedules nothing: the
    // requests themselves are already in the loop's wait.
    if (entry.idle_since_ms != 0 && entry.session->OpenStreams() == 0) {
      consider(entry.idle_since_ms);
    }
  }
  return soonest;
}

void ConnectionPool::Clear() {
  for (Idle& entry : idle_) {
    entry.connection->Close();
  }
  idle_.clear();
  // The sessions go with them, and so do the claims: a claim outstanding under
  // a factory that is gone would park every later request for that origin
  // against a connect that will never finish.
  sessions_.clear();
  claims_.clear();
  protocols_.clear();
}

void ConnectionPool::SetFactory(TransportFactory& factory) {
  Clear();
  factory_ = &factory;
}

}  // namespace microbrowser::net

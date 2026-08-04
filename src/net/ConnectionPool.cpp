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

ConnectionPool::Lease ConnectionPool::Acquire(std::string_view partition, std::string_view host,
                                              std::uint16_t port, bool secure, bool allow_reuse) {
  if (allow_reuse) {
    const std::string key = ConnectionKey(partition, host, port, secure);
    // Newest first: the connection least likely to have been closed by a peer
    // that got bored of it.
    for (auto entry = idle_.rbegin(); entry != idle_.rend(); ++entry) {
      if (entry->key != key) {
        continue;
      }
      Lease lease{std::move(entry->connection), true};
      idle_.erase(std::next(entry).base());
      AddPerformanceCounter(PerfCounterId::NetConnectionsReused);
      return lease;
    }
  }
  return Lease{factory_->Create(), false};
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
}

std::optional<std::uint32_t> ConnectionPool::NextDeadlineMs(std::int64_t now_ms) const {
  std::optional<std::uint32_t> soonest;
  for (const Idle& entry : idle_) {
    const std::int64_t due = entry.since_ms + kIdleConnectionTimeoutMs;
    const std::int64_t remaining = std::max<std::int64_t>(0, due - now_ms);
    const auto capped = static_cast<std::uint32_t>(
        std::min<std::int64_t>(remaining, static_cast<std::int64_t>(UINT32_MAX)));
    soonest = soonest.has_value() ? std::min(*soonest, capped) : capped;
  }
  return soonest;
}

void ConnectionPool::Clear() {
  for (Idle& entry : idle_) {
    entry.connection->Close();
  }
  idle_.clear();
}

void ConnectionPool::SetFactory(TransportFactory& factory) {
  Clear();
  factory_ = &factory;
}

}  // namespace microbrowser::net

#include "net/ResolverCache.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

std::string ResolverKey(std::string_view partition, std::string_view host, std::uint16_t port) {
  std::string key;
  key.reserve(partition.size() + host.size() + 8);
  key.append(partition);
  key.push_back('|');
  key.append(host);
  key.push_back(':');
  key.append(std::to_string(port));
  return key;
}

const std::vector<ResolvedAddress>* ResolverCache::Lookup(std::string_view partition,
                                                          std::string_view host,
                                                          std::uint16_t port,
                                                          std::int64_t now_ms) {
  const std::string key = ResolverKey(partition, host, port);
  // Expiry is checked on the way past rather than on a timer: a resolver cache
  // that armed a wakeup to forget something would be a wakeup on an idle
  // browser, and there is nothing that has to happen at the moment an entry
  // goes stale.
  const auto expired = [now_ms](const Entry& entry) {
    return now_ms - entry.stored_ms >= kResolvedNameTtlMs;
  };
  std::erase_if(entries_, expired);
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&key](const Entry& entry) { return entry.key == key; });
  if (found == entries_.end()) {
    AddPerformanceCounter(PerfCounterId::NetResolverCacheMisses);
    return nullptr;
  }
  AddPerformanceCounter(PerfCounterId::NetResolverCacheHits);
  return &found->addresses;
}

void ResolverCache::Store(std::string_view partition, std::string_view host, std::uint16_t port,
                          std::vector<ResolvedAddress> addresses, std::int64_t now_ms) {
  if (addresses.empty()) {
    return;
  }
  std::string key = ResolverKey(partition, host, port);
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&key](const Entry& entry) { return entry.key == key; });
  if (found != entries_.end()) {
    found->addresses = std::move(addresses);
    found->stored_ms = now_ms;
    return;
  }
  // Oldest goes first when the bound binds, which is the same rule the
  // connection pool uses for the same reason: the entry least likely to be
  // wanted again is the one that has been sitting longest.
  if (entries_.size() >= kMaxResolvedNames) {
    const auto oldest = std::min_element(
        entries_.begin(), entries_.end(),
        [](const Entry& a, const Entry& b) { return a.stored_ms < b.stored_ms; });
    entries_.erase(oldest);
  }
  entries_.push_back(Entry{std::move(key), std::move(addresses), now_ms});
}

void ResolverCache::Clear() { entries_.clear(); }

}  // namespace microbrowser::net

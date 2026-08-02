#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/HttpMessage.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::net {

// The HTTP cache. Memory-only, and partitioned.
//
// Memory-only because `guidelines/privacy.md` says nothing persists to disk
// unless the user opted in, and a cache is the easiest thing in a browser to
// leave on disk by accident.
//
// Partitioned because cache *timing* is a read oracle across partitions: a
// third party that can measure whether its own resource loaded quickly on one
// site learns whether the user visited another. That is why the key is a
// `url::PartitionKey` and not a URL, and why there is no overload that takes
// only a URL — a lookup that used part of the key would reintroduce exactly the
// oracle the partitioning removes.
class HttpCache {
 public:
  struct Entry {
    HttpResponse response;
    std::int64_t stored_at = 0;
    // Absolute expiry, derived from Cache-Control at store time. A response
    // with no freshness information is not cached at all rather than cached
    // with a guessed lifetime.
    std::int64_t expires_at = 0;
  };

  // Null when absent or stale. Staleness is checked here so no caller can
  // forget to.
  const Entry* Lookup(const url::PartitionKey& key, const url::Url& url, std::int64_t now) const;

  // Stores only what is safely cacheable. Returns false when the response said
  // not to be cached, which is the common answer.
  bool Store(const url::PartitionKey& key, const url::Url& url, const HttpResponse& response,
             std::int64_t now);

  void Clear() {
    entries_.clear();
    bytes_ = 0;
  }
  void ClearContainer(url::ContainerId container);
  void RemoveStale(std::int64_t now);
  std::size_t Size() const { return entries_.size(); }
  std::size_t Bytes() const { return bytes_; }

  void SetByteBudget(std::size_t bytes);

 private:
  struct Record {
    url::PartitionKey key;
    std::string url;
    Entry entry;
  };

  void EvictToBudget();

  std::vector<Record> entries_;
  std::size_t bytes_ = 0;
  std::size_t budget_ = 32u * 1024u * 1024u;
};

// Freshness lifetime in seconds from `Cache-Control`, or nullopt when the
// response must not be cached. Exposed for testing: this one function decides
// whether anything is stored at all.
std::optional<std::int64_t> FreshnessLifetime(const HttpResponse& response);

}  // namespace microbrowser::net

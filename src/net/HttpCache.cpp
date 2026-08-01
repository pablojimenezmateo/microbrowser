#include "net/HttpCache.h"

#include <algorithm>

#include "util/Parse.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Splits a comma-separated header value into lowercased directives.
std::vector<std::string> Directives(std::string_view value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    std::string_view piece =
        comma == std::string_view::npos ? value.substr(start) : value.substr(start, comma - start);
    while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
      piece.remove_prefix(1);
    }
    while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
      piece.remove_suffix(1);
    }
    if (!piece.empty()) {
      std::string lowered(piece);
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), ToLower);
      out.push_back(std::move(lowered));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

}  // namespace

std::optional<std::int64_t> FreshnessLifetime(const HttpResponse& response) {
  // Only these are cached. A method-and-status allowlist rather than a
  // blocklist: a status we have not thought about is one we do not know the
  // caching semantics of, and guessing produces a stale page rather than an
  // error somebody would notice.
  if (response.status != 200 && response.status != 203 && response.status != 300 &&
      response.status != 301 && response.status != 404 && response.status != 410) {
    return std::nullopt;
  }

  const auto control = response.headers.Get("cache-control");
  if (!control.has_value()) {
    // No freshness information. Heuristic caching from Last-Modified is what
    // the RFC permits and what produces the "why am I seeing an old page"
    // class of bug; not caching is the answer that is never wrong.
    return std::nullopt;
  }

  std::optional<std::int64_t> max_age;
  for (const std::string& directive : Directives(*control)) {
    if (directive == "no-store" || directive == "no-cache" || directive == "private") {
      // `private` is refused rather than honored-as-private: this cache is
      // already per-partition, and a shared-cache directive is not a statement
      // about a partitioned one.
      return std::nullopt;
    }
    if (directive.rfind("max-age=", 0) == 0) {
      const auto seconds = util::ParseInt64(std::string_view(directive).substr(8));
      if (seconds.has_value() && *seconds > 0) {
        max_age = *seconds;
      } else {
        return std::nullopt;
      }
    }
  }
  if (response.headers.Has("set-cookie")) {
    // A cached response carrying a Set-Cookie would replay somebody's cookie
    // into a later load.
    return std::nullopt;
  }
  return max_age;
}

const HttpCache::Entry* HttpCache::Lookup(const url::PartitionKey& key, const url::Url& url,
                                          std::int64_t now) const {
  const std::string target = url.Serialize(true);
  for (const Record& record : entries_) {
    if (record.key == key && record.url == target) {
      if (record.entry.expires_at <= now) {
        AddPerformanceCounter(PerfCounterId::NetCacheStale);
        return nullptr;
      }
      AddPerformanceCounter(PerfCounterId::NetCacheHits);
      return &record.entry;
    }
  }
  AddPerformanceCounter(PerfCounterId::NetCacheMisses);
  return nullptr;
}

bool HttpCache::Store(const url::PartitionKey& key, const url::Url& url,
                      const HttpResponse& response, std::int64_t now) {
  const auto lifetime = FreshnessLifetime(response);
  if (!lifetime.has_value()) {
    return false;
  }

  const std::string target = url.Serialize(true);
  const std::size_t size = response.body.size() + target.size() + 256;
  if (size > budget_) {
    return false;  // one response larger than the whole cache
  }

  const auto same = [&](const Record& record) {
    return record.key == key && record.url == target;
  };
  const auto found = std::find_if(entries_.begin(), entries_.end(), same);
  if (found != entries_.end()) {
    bytes_ -= std::min(bytes_, found->entry.response.body.size() + found->url.size() + 256);
    entries_.erase(found);
  }

  Record record;
  record.key = key;
  record.url = target;
  record.entry.response = response;
  record.entry.stored_at = now;
  record.entry.expires_at = now + *lifetime;
  entries_.push_back(std::move(record));
  bytes_ += size;
  EvictToBudget();
  AddPerformanceCounter(PerfCounterId::NetCacheStores);
  return true;
}

void HttpCache::EvictToBudget() {
  while (bytes_ > budget_ && !entries_.empty()) {
    const Record& oldest = entries_.front();
    bytes_ -= std::min(bytes_, oldest.entry.response.body.size() + oldest.url.size() + 256);
    entries_.erase(entries_.begin());
  }
}

void HttpCache::RemoveStale(std::int64_t now) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [now](const Record& record) {
                                  return record.entry.expires_at <= now;
                                }),
                 entries_.end());
  bytes_ = 0;
  for (const Record& record : entries_) {
    bytes_ += record.entry.response.body.size() + record.url.size() + 256;
  }
}

void HttpCache::ClearContainer(url::ContainerId container) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [container](const Record& record) {
                                  return record.key.Container() == container;
                                }),
                 entries_.end());
  bytes_ = 0;
  for (const Record& record : entries_) {
    bytes_ += record.entry.response.body.size() + record.url.size() + 256;
  }
}

}  // namespace microbrowser::net

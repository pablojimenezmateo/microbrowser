#pragma once

#include <cstddef>
#include <map>
#include <string>

#include "storage/StorageArea.h"
#include "url/PartitionKey.h"

namespace microbrowser::storage {

// Every storage area there is, keyed by ADR 0005's partition key.
//
// ADR 0021 §1: **every store takes a `url::PartitionKey` and there is no overload
// without one.** The architecture lint enforces that on this class by name, on this
// commit, which is what the ADR asks for -- the rule is only an invariant if
// forgetting it fails the build.
//
// The consequence, stated the way a page notices it: `example.com` embedded in `a.com`
// and `example.com` embedded in `b.com` have **different** storage. That breaks
// third-party single-sign-on flows that assume shared state, and under this project's
// priority order that breakage is a decision rather than a bug.
//
// One class serves both `sessionStorage` and `localStorage`; what differs is who owns
// the instance and therefore how long it lives. A session store is owned by the tab
// and dies with it; a local store is owned by the browser session. Neither reaches a
// disk -- ADR 0021 §2 makes persistence a per-site user act that lands together with
// encryption at rest, and a sign-in token in a plaintext file is the worst outcome
// available here.
class PartitionedStorage {
 public:
  PartitionedStorage() = default;
  explicit PartitionedStorage(std::size_t quota_per_area) : quota_(quota_per_area) {}

  // The area for this key, created empty on first use. A page that reads before it
  // writes gets an empty store rather than an error, which is what the API says and
  // is why this returns a reference rather than an optional.
  StorageArea& Lookup(const url::PartitionKey& key);

  // Whether anything has ever been stored for this key. Distinct from `Lookup`
  // because asking must not create: a caller counting partitions would otherwise
  // create the one it is asking about.
  bool Has(const url::PartitionKey& key) const;

  std::size_t Partitions() const { return areas_.size(); }

  // Everything in every partition, gone. What a "clear browsing data" act does, and
  // what the end of a browser session does to the local store -- there is no other
  // way for state to outlive this object, by construction.
  void Clear() { areas_.clear(); }

 private:
  // Keyed by the partition key's serialization rather than by the key itself: a map
  // needs an ordering, and a string comparison of the canonical form is one place to
  // be wrong instead of three fields' worth.
  std::map<std::string, StorageArea, std::less<>> areas_;
  std::size_t quota_ = StorageArea::kQuotaBytes;
};

}  // namespace microbrowser::storage

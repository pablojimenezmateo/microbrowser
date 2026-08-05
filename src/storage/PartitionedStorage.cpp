#include "storage/PartitionedStorage.h"

#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::storage {

StorageArea& PartitionedStorage::Lookup(const url::PartitionKey& key) {
  const std::string serialized = key.Serialize();
  const auto found = areas_.find(serialized);
  if (found != areas_.end()) {
    return found->second;
  }
  util::AddPerformanceCounter(util::PerfCounterId::StoragePartitionsCreated);
  return areas_.emplace(serialized, StorageArea(quota_)).first->second;
}

bool PartitionedStorage::Has(const url::PartitionKey& key) const {
  return areas_.find(key.Serialize()) != areas_.end();
}

}  // namespace microbrowser::storage

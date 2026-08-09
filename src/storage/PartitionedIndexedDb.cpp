#include "storage/PartitionedIndexedDb.h"

#include "util/PerformanceCounters.h"

namespace microbrowser::storage {

bool IndexedDbDatabase::CreateObjectStore(const std::string& name,
                                          std::vector<std::string> key_path) {
  if (stores_.contains(name)) {
    return false;
  }
  stores_.emplace(name, IndexedDbObjectStore(std::move(key_path)));
  return true;
}

IndexedDbObjectStore* IndexedDbDatabase::Store(const std::string& name) {
  const auto found = stores_.find(name);
  return found == stores_.end() ? nullptr : &found->second;
}

std::vector<std::string> IndexedDbDatabase::StoreNames() const {
  std::vector<std::string> names;
  names.reserve(stores_.size());
  for (const auto& [name, store] : stores_) {
    names.push_back(name);
  }
  return names;
}

PartitionedIndexedDb::Databases& PartitionedIndexedDb::Lookup(const url::PartitionKey& key) {
  const std::string serialized = key.Serialize();
  const auto found = partitions_.find(serialized);
  if (found != partitions_.end()) {
    return found->second.databases;
  }
  util::AddPerformanceCounter(util::PerfCounterId::IdbPartitionsCreated);
  return partitions_.emplace(serialized, Partition{}).first->second.databases;
}

bool PartitionedIndexedDb::Has(const url::PartitionKey& key) const {
  return partitions_.find(key.Serialize()) != partitions_.end();
}

std::size_t PartitionedIndexedDb::BytesUsed(const url::PartitionKey& key) const {
  const auto found = partitions_.find(key.Serialize());
  return found == partitions_.end() ? 0u : found->second.bytes_used;
}

bool PartitionedIndexedDb::ChargeQuota(const url::PartitionKey& key, std::int64_t delta) {
  const std::string serialized = key.Serialize();
  Partition& partition = partitions_[serialized];
  if (delta > 0 && partition.bytes_used + static_cast<std::size_t>(delta) > quota_) {
    util::AddPerformanceCounter(util::PerfCounterId::IdbQuotaRefusals);
    return false;
  }
  const std::int64_t next = static_cast<std::int64_t>(partition.bytes_used) + delta;
  partition.bytes_used = next < 0 ? 0u : static_cast<std::size_t>(next);
  return true;
}

}  // namespace microbrowser::storage

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "storage/IndexedDbObjectStore.h"
#include "url/PartitionKey.h"

namespace microbrowser::storage {

// One database: its own version number, and the object stores created in it.
// `src/bindings` names a database by string; this is what that name resolves
// to on this side of ADR 0038's seam.
class IndexedDbDatabase {
 public:
  explicit IndexedDbDatabase(std::string name) : name_(std::move(name)) {}

  const std::string& Name() const { return name_; }
  std::uint64_t Version() const { return version_; }
  void SetVersion(std::uint64_t version) { version_ = version; }

  bool CreateObjectStore(const std::string& name, std::vector<std::string> key_path);
  IndexedDbObjectStore* Store(const std::string& name);
  std::vector<std::string> StoreNames() const;

 private:
  std::string name_;
  std::uint64_t version_ = 0;
  std::map<std::string, IndexedDbObjectStore> stores_;
};

// Every IndexedDB database there is, keyed by ADR 0005's partition key.
//
// ADR 0021 §1 (extended by ADR 0038): **every store takes a `url::PartitionKey`
// and there is no overload without one.** `IndexedDbDatabases` -- the map this
// returns -- names the databases inside one partition; picking one by name is
// `IndexedDbDatabase::Store`'s job, not a second key this class would have to
// carry.
//
// In memory only, like `PartitionedStorage`. ADR 0021 §2 makes persistence a
// per-site user act that lands with encryption at rest or not at all, and a
// synced entity store written to a plaintext file is the same risk a
// plaintext sign-in token is.
class PartitionedIndexedDb {
 public:
  // 50 MiB per partition. An order of magnitude above `StorageArea`'s 5 MiB
  // because an offline entity store is exactly the kind of thing IndexedDB
  // exists for and a page that uses it is not storing a preferences blob --
  // and still a bound, because unbounded storage from a page is a denial of
  // service against this process.
  static constexpr std::size_t kQuotaBytes = 50u * 1024u * 1024u;

  using Databases = std::map<std::string, IndexedDbDatabase>;

  explicit PartitionedIndexedDb(std::size_t quota_per_partition = kQuotaBytes)
      : quota_(quota_per_partition) {}

  // The partition's databases, created empty on first use.
  Databases& Lookup(const url::PartitionKey& key);
  bool Has(const url::PartitionKey& key) const;

  std::size_t BytesUsed(const url::PartitionKey& key) const;
  // Charges `delta` (positive or negative) against the partition's quota.
  // False, and unchanged, when a positive delta would exceed it -- which is
  // what lets a caller pre-check with `IndexedDbObjectStore::RecordCost`
  // before writing anything and never have to undo a write.
  bool ChargeQuota(const url::PartitionKey& key, std::int64_t delta);

  void Clear() { partitions_.clear(); }

 private:
  struct Partition {
    Databases databases;
    std::size_t bytes_used = 0;
  };
  std::map<std::string, Partition, std::less<>> partitions_;
  std::size_t quota_;
};

}  // namespace microbrowser::storage

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "storage/IndexedDbKey.h"

namespace microbrowser::storage {

// One named index over a store: whether it refuses a second record under the
// same key, and every key currently pointing into the store. The keyPath
// itself is not here -- `src/bindings` is what extracts a key from a stored
// value, because this module may not see `js`, so an index's keyPath lives
// only as bindings-side metadata (see bindings/IndexedDb.h).
struct IndexedDbIndexDef {
  bool unique = false;

  struct Entry {
    IndexedDbKey key;
    // More than one primary key only when `unique` is false.
    std::vector<std::string> primary_keys;
  };
  // Keyed by `IndexedDbKey::Encode()`.
  std::map<std::string, Entry> entries;
};

// One object store: its records by primary key, and the indexes built over
// them. Every write updates a record and its indexes together, so the two
// can never disagree about what a key contains -- the failure mode a
// separate "reindex" pass would risk.
class IndexedDbObjectStore {
 public:
  explicit IndexedDbObjectStore(std::vector<std::string> key_path = std::vector<std::string>());

  const std::vector<std::string>& KeyPath() const { return key_path_; }

  bool CreateIndex(const std::string& name, bool unique);
  std::vector<std::string> IndexNames() const;
  bool HasIndex(const std::string& name) const { return indexes_.contains(name); }

  // The bytes a record with this primary key would add or remove against a
  // quota, before anything is written -- so a caller can refuse a write that
  // would exceed one without touching the store. Zero for a key that is not
  // present, which is also the answer `Put` needs to compute its own delta.
  std::size_t RecordCost(const IndexedDbKey& key) const;

  enum class PutResult { Stored, ConstraintError };
  // Replaces (or inserts) the record at `key` and every index entry named in
  // `index_keys` at once. `bytes_delta` is filled with how the quota changed
  // -- negative for a smaller replacement -- regardless of the result, so a
  // caller that pre-checked with `RecordCost` never has to undo a write.
  PutResult Put(const IndexedDbKey& key, std::vector<std::uint8_t> value,
               const std::vector<std::pair<std::string, IndexedDbKey>>& index_keys,
               std::int64_t& bytes_delta);
  std::optional<std::vector<std::uint8_t>> Get(const IndexedDbKey& key) const;
  bool Delete(const IndexedDbKey& key, std::int64_t& bytes_delta);

  struct QueryEntry {
    // The index key that matched, or the primary key again when `index` was
    // empty -- an object store's own cursor iterates by its primary key.
    IndexedDbKey key;
    IndexedDbKey primary_key;
    std::vector<std::uint8_t> value;
  };
  // `index` empty means the store's own primary key; `only_key` unset means
  // every record. The two shapes `IDBKeyRange` and a bare `openCursor()` need
  // here -- see docs/adr/0038-broadcast-channel-and-indexeddb.md. Empty (not
  // failure) when `index` names one that does not exist.
  std::vector<QueryEntry> Query(const std::string& index,
                                const std::optional<IndexedDbKey>& only_key) const;

 private:
  void RemoveFromIndexes(const std::string& encoded_primary_key);

  std::vector<std::string> key_path_;
  // Keyed by `IndexedDbKey::Encode()` of the primary key.
  std::map<std::string, std::pair<IndexedDbKey, std::vector<std::uint8_t>>> records_;
  std::map<std::string, IndexedDbIndexDef> indexes_;
};

}  // namespace microbrowser::storage

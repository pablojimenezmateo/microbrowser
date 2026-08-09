#include "storage/IndexedDbObjectStore.h"

#include <algorithm>

namespace microbrowser::storage {

namespace {

std::size_t Cost(const IndexedDbKey& key, std::size_t value_size) {
  return key.Encode().size() + value_size;
}

}  // namespace

IndexedDbObjectStore::IndexedDbObjectStore(std::vector<std::string> key_path)
    : key_path_(std::move(key_path)) {}

bool IndexedDbObjectStore::CreateIndex(const std::string& name, bool unique) {
  if (indexes_.contains(name)) {
    return false;
  }
  indexes_.emplace(name, IndexedDbIndexDef{unique, {}});
  return true;
}

std::vector<std::string> IndexedDbObjectStore::IndexNames() const {
  std::vector<std::string> names;
  names.reserve(indexes_.size());
  for (const auto& [name, def] : indexes_) {
    names.push_back(name);
  }
  return names;
}

std::size_t IndexedDbObjectStore::RecordCost(const IndexedDbKey& key) const {
  const auto found = records_.find(key.Encode());
  return found == records_.end() ? 0u : Cost(found->second.first, found->second.second.size());
}

void IndexedDbObjectStore::RemoveFromIndexes(const std::string& encoded_primary_key) {
  for (auto& [name, def] : indexes_) {
    for (auto entry = def.entries.begin(); entry != def.entries.end();) {
      auto& keys = entry->second.primary_keys;
      keys.erase(std::remove(keys.begin(), keys.end(), encoded_primary_key), keys.end());
      entry = keys.empty() ? def.entries.erase(entry) : std::next(entry);
    }
  }
}

IndexedDbObjectStore::PutResult IndexedDbObjectStore::Put(
    const IndexedDbKey& key, std::vector<std::uint8_t> value,
    const std::vector<std::pair<std::string, IndexedDbKey>>& index_keys,
    std::int64_t& bytes_delta) {
  const std::string encoded_primary = key.Encode();

  // Refused before anything is written: a unique index must not end up
  // pointing two primary keys at the same value.
  for (const auto& [index_name, index_key] : index_keys) {
    const auto index = indexes_.find(index_name);
    if (index == indexes_.end() || !index->second.unique) {
      continue;
    }
    const auto entry = index->second.entries.find(index_key.Encode());
    if (entry != index->second.entries.end() &&
        std::find(entry->second.primary_keys.begin(), entry->second.primary_keys.end(),
                  encoded_primary) == entry->second.primary_keys.end() &&
        !entry->second.primary_keys.empty()) {
      return PutResult::ConstraintError;
    }
  }

  const std::size_t old_cost = RecordCost(key);
  const std::size_t new_cost = Cost(key, value.size());
  bytes_delta = static_cast<std::int64_t>(new_cost) - static_cast<std::int64_t>(old_cost);

  RemoveFromIndexes(encoded_primary);
  records_[encoded_primary] = {key, std::move(value)};
  for (const auto& [index_name, index_key] : index_keys) {
    const auto index = indexes_.find(index_name);
    if (index == indexes_.end()) {
      continue;
    }
    IndexedDbIndexDef::Entry& entry = index->second.entries[index_key.Encode()];
    entry.key = index_key;
    if (std::find(entry.primary_keys.begin(), entry.primary_keys.end(), encoded_primary) ==
        entry.primary_keys.end()) {
      entry.primary_keys.push_back(encoded_primary);
    }
  }
  return PutResult::Stored;
}

std::optional<std::vector<std::uint8_t>> IndexedDbObjectStore::Get(const IndexedDbKey& key) const {
  const auto found = records_.find(key.Encode());
  if (found == records_.end()) {
    return std::nullopt;
  }
  return found->second.second;
}

bool IndexedDbObjectStore::Delete(const IndexedDbKey& key, std::int64_t& bytes_delta) {
  const std::string encoded = key.Encode();
  const auto found = records_.find(encoded);
  if (found == records_.end()) {
    bytes_delta = 0;
    return false;
  }
  bytes_delta = -static_cast<std::int64_t>(Cost(found->second.first, found->second.second.size()));
  RemoveFromIndexes(encoded);
  records_.erase(found);
  return true;
}

std::vector<IndexedDbObjectStore::QueryEntry> IndexedDbObjectStore::Query(
    const std::string& index, const std::optional<IndexedDbKey>& only_key) const {
  std::vector<QueryEntry> out;
  if (index.empty()) {
    for (const auto& [encoded, record] : records_) {
      if (only_key.has_value() && encoded != only_key->Encode()) {
        continue;
      }
      out.push_back(QueryEntry{record.first, record.first, record.second});
    }
    return out;
  }
  const auto found = indexes_.find(index);
  if (found == indexes_.end()) {
    return out;
  }
  for (const auto& [encoded_index_key, entry] : found->second.entries) {
    if (only_key.has_value() && encoded_index_key != only_key->Encode()) {
      continue;
    }
    for (const std::string& primary : entry.primary_keys) {
      const auto record = records_.find(primary);
      if (record != records_.end()) {
        out.push_back(QueryEntry{entry.key, record->second.first, record->second.second});
      }
    }
  }
  return out;
}

}  // namespace microbrowser::storage

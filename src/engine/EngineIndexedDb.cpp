// Where a page's `indexedDB` reads and writes land.
//
// ADR 0038, and the same shape EngineStorage.cpp is: this is the *only* place
// a partition key is derived for it, because `src/bindings` may not see
// `url::PartitionKey` at all. A binding names a database and a store by
// string; this file decides whose data those strings mean, from the
// document's own URL, exactly the way `AreaFor` decides for `sessionStorage`.
//
// **The conversion between the two key types lives here too.** `src/bindings`
// declares `IndexedDbKeyValue` and `src/storage` declares `IndexedDbKey` --
// deliberately two types rather than one shared across a boundary that is not
// allowed to be crossed -- and this is the one module that may see both.

#include <optional>
#include <string>
#include <vector>

#include "bindings/IndexedDb.h"
#include "engine/Engine.h"
#include "storage/IndexedDbKey.h"
#include "storage/IndexedDbObjectStore.h"
#include "storage/PartitionedIndexedDb.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::engine {

namespace {

storage::IndexedDbKey ToStorageKey(const bindings::IndexedDbKeyValue& key) {
  switch (key.type) {
    case bindings::IndexedDbKeyValue::Type::Number:
      return storage::IndexedDbKey::OfNumber(key.number);
    case bindings::IndexedDbKeyValue::Type::String:
      return storage::IndexedDbKey::OfString(key.text);
    case bindings::IndexedDbKeyValue::Type::Array: {
      std::vector<storage::IndexedDbKey> parts;
      parts.reserve(key.parts.size());
      for (const bindings::IndexedDbKeyValue& part : key.parts) {
        parts.push_back(ToStorageKey(part));
      }
      return storage::IndexedDbKey::OfArray(std::move(parts));
    }
  }
  return storage::IndexedDbKey::OfNumber(0.0);
}

bindings::IndexedDbKeyValue ToBindingsKey(const storage::IndexedDbKey& key) {
  bindings::IndexedDbKeyValue value;
  switch (key.type) {
    case storage::IndexedDbKey::Type::Number:
      value.type = bindings::IndexedDbKeyValue::Type::Number;
      value.number = key.number;
      break;
    case storage::IndexedDbKey::Type::String:
      value.type = bindings::IndexedDbKeyValue::Type::String;
      value.text = key.text;
      break;
    case storage::IndexedDbKey::Type::Array:
      value.type = bindings::IndexedDbKeyValue::Type::Array;
      value.parts.reserve(key.parts.size());
      for (const storage::IndexedDbKey& part : key.parts) {
        value.parts.push_back(ToBindingsKey(part));
      }
      break;
  }
  return value;
}

std::vector<std::string> ToKeyPathParts(const bindings::IndexedDbKeyPath& path) {
  return path.has_path ? path.parts : std::vector<std::string>{};
}

}  // namespace

storage::PartitionedIndexedDb::Databases* Engine::DatabasesFor() {
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return nullptr;
  }
  // Top-level only, exactly like `AreaFor` -- ADR 0027's nested-context case
  // widens this the same way it will widen storage, and not before then.
  const url::PartitionKey key = url::PartitionKey::ForTopLevel(url::ContainerId::Default(), *base);
  if (key.GetOrigin().IsOpaque()) {
    return nullptr;
  }
  return &indexed_db_.Lookup(key);
}

bool Engine::Available() { return DatabasesFor() != nullptr; }

Engine::OpenResult Engine::OpenDatabase(const std::string& name, std::uint64_t version) {
  OpenResult result;
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return result;
  }
  auto [entry, inserted] = databases->try_emplace(name, storage::IndexedDbDatabase(name));
  storage::IndexedDbDatabase& database = entry->second;
  result.old_version = database.Version();
  if (inserted) {
    // A database that did not exist opens at version 1, unless the page asked
    // for a specific one -- `indexedDB.open(name, 3)` on a first visit opens
    // straight at 3, which is what every browser does.
    database.SetVersion(version == 0 ? 1 : version);
    result.needs_upgrade = true;
  } else if (version != 0 && version > database.Version()) {
    database.SetVersion(version);
    result.needs_upgrade = true;
  } else if (version != 0 && version < database.Version()) {
    // The specified failure -- `VersionError` -- is the binding's to throw;
    // this just reports the version unchanged and lets `needs_upgrade` stay
    // false, which the binding reads as "refuse".
    result.old_version = database.Version();
    result.new_version = database.Version();
    return result;
  }
  result.new_version = database.Version();
  return result;
}

void Engine::DeleteDatabase(const std::string& name) {
  if (storage::PartitionedIndexedDb::Databases* databases = DatabasesFor()) {
    databases->erase(name);
  }
}

bool Engine::CreateObjectStore(const std::string& db, const std::string& store,
                               const bindings::IndexedDbKeyPath& key_path) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return false;
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return false;
  }
  return found->second.CreateObjectStore(store, ToKeyPathParts(key_path));
}

bool Engine::CreateIndex(const std::string& db, const std::string& store,
                         const std::string& index, bool unique) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return false;
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return false;
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  return target_store != nullptr && target_store->CreateIndex(index, unique);
}

std::vector<std::string> Engine::ObjectStoreNames(const std::string& db) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return {};
  }
  const auto found = databases->find(db);
  return found == databases->end() ? std::vector<std::string>{} : found->second.StoreNames();
}

std::vector<std::string> Engine::IndexNames(const std::string& db, const std::string& store) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return {};
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return {};
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  return target_store == nullptr ? std::vector<std::string>{} : target_store->IndexNames();
}

bindings::IndexedDbKeyPath Engine::ObjectStoreKeyPath(const std::string& db,
                                                      const std::string& store) {
  bindings::IndexedDbKeyPath path;
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return path;
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return path;
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  if (target_store == nullptr || target_store->KeyPath().empty()) {
    return path;
  }
  path.has_path = true;
  path.is_array = target_store->KeyPath().size() > 1;
  path.parts = target_store->KeyPath();
  return path;
}

Engine::PutResult Engine::Put(const std::string& db, const std::string& store,
                              const bindings::IndexedDbKeyValue& key,
                              std::vector<std::uint8_t> value,
                              std::vector<IndexKeyEntry> index_keys) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return PutResult::NotFound;
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return PutResult::NotFound;
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  if (target_store == nullptr) {
    return PutResult::NotFound;
  }
  const storage::IndexedDbKey storage_key = ToStorageKey(key);
  std::vector<std::pair<std::string, storage::IndexedDbKey>> storage_index_keys;
  storage_index_keys.reserve(index_keys.size());
  for (const IndexKeyEntry& entry : index_keys) {
    storage_index_keys.emplace_back(entry.index, ToStorageKey(entry.key));
  }
  // Charged before the write commits, against what the write would cost --
  // not what it costs now, which is `RecordCost`'s job so a replacement's
  // *delta* is what is checked rather than its full size.
  const std::size_t added_cost = storage_key.Encode().size() + value.size();
  const std::size_t existing_cost = target_store->RecordCost(storage_key);
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (base.has_value()) {
    const url::PartitionKey partition_key =
        url::PartitionKey::ForTopLevel(url::ContainerId::Default(), *base);
    if (!indexed_db_.ChargeQuota(
            partition_key, static_cast<std::int64_t>(added_cost) -
                              static_cast<std::int64_t>(existing_cost))) {
      return PutResult::QuotaExceeded;
    }
  }
  std::int64_t bytes_delta = 0;
  const storage::IndexedDbObjectStore::PutResult result =
      target_store->Put(storage_key, std::move(value), storage_index_keys, bytes_delta);
  return result == storage::IndexedDbObjectStore::PutResult::ConstraintError
             ? PutResult::ConstraintError
             : PutResult::Stored;
}

std::optional<std::vector<std::uint8_t>> Engine::Get(const std::string& db,
                                                     const std::string& store,
                                                     const bindings::IndexedDbKeyValue& key) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return std::nullopt;
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return std::nullopt;
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  return target_store == nullptr ? std::nullopt : target_store->Get(ToStorageKey(key));
}

bool Engine::Delete(const std::string& db, const std::string& store,
                    const bindings::IndexedDbKeyValue& key) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return false;
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return false;
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  if (target_store == nullptr) {
    return false;
  }
  std::int64_t bytes_delta = 0;
  const bool removed = target_store->Delete(ToStorageKey(key), bytes_delta);
  if (removed && bytes_delta != 0) {
    if (const std::optional<url::Url>& base = page_.BaseUrl(); base.has_value()) {
      indexed_db_.ChargeQuota(url::PartitionKey::ForTopLevel(url::ContainerId::Default(), *base),
                             bytes_delta);
    }
  }
  return removed;
}

std::vector<Engine::CursorEntry> Engine::Query(
    const std::string& db, const std::string& store, const std::string& index,
    const std::optional<bindings::IndexedDbKeyValue>& only_key) {
  storage::PartitionedIndexedDb::Databases* databases = DatabasesFor();
  if (databases == nullptr) {
    return {};
  }
  const auto found = databases->find(db);
  if (found == databases->end()) {
    return {};
  }
  storage::IndexedDbObjectStore* target_store = found->second.Store(store);
  if (target_store == nullptr) {
    return {};
  }
  const std::optional<storage::IndexedDbKey> storage_only =
      only_key.has_value() ? std::make_optional(ToStorageKey(*only_key)) : std::nullopt;
  std::vector<CursorEntry> out;
  for (const storage::IndexedDbObjectStore::QueryEntry& entry :
       target_store->Query(index, storage_only)) {
    out.push_back(CursorEntry{ToBindingsKey(entry.key), ToBindingsKey(entry.primary_key),
                              entry.value});
  }
  return out;
}

}  // namespace microbrowser::engine

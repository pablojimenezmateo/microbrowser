#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace microbrowser::bindings {

// A structured-clone key restricted to a number, a string, or an array of
// either -- a compound key, for EntityStore's `["parentEntityKey",
// "childEntityKey"]`. Mirrors `storage::IndexedDbKey` in shape but is its own
// type: `src/bindings` may not see `src/storage` (or `src/url`) at all, so
// the value that crosses this seam is one this module declares, exactly as
// `ScriptResponse` is bindings' own type and not a `net::` one. `src/engine`
// is what converts between the two, on the side of the seam that may see
// both.
struct IndexedDbKeyValue {
  enum class Type { Number, String, Array };

  Type type = Type::Number;
  double number = 0.0;
  std::string text;
  std::vector<IndexedDbKeyValue> parts;
};

// A store or index's keyPath: absent, one dotted path ("name.first"), or
// several for a compound key. `is_array` is what tells the two single-part
// shapes apart -- `keyPath: "key"` against `keyPath: ["key"]` -- which matters
// because the second produces an Array-typed key even with one element.
struct IndexedDbKeyPath {
  bool has_path = false;
  bool is_array = false;
  std::vector<std::string> parts;
};

// What `indexedDB` is answered through.
//
// ADR 0038, the same inversion ADR 0021 used for `sessionStorage` and
// `localStorage`: the interface is declared *here* and implemented by
// `src/engine`, so a binding cannot name a partition even by accident. It
// picks a database and a store by name; which partition's data that is comes
// from the document, entirely on the far side of this seam.
class IndexedDbSource {
 public:
  IndexedDbSource() = default;
  IndexedDbSource(const IndexedDbSource&) = delete;
  IndexedDbSource& operator=(const IndexedDbSource&) = delete;
  virtual ~IndexedDbSource() = default;

  // False for an opaque origin, exactly like `StorageSource::Available` --
  // there is no partition to key a database by, so `indexedDB` is not
  // declared at all. See ADR 0012.
  virtual bool Available() = 0;

  struct OpenResult {
    std::uint64_t old_version = 0;
    std::uint64_t new_version = 0;
    // True when `new_version` is higher than what this database already was,
    // which is the only thing that fires `upgradeneeded`.
    bool needs_upgrade = false;
  };
  // Opens `name`, creating it at version 1 if it does not exist yet. `version`
  // of 0 means "whatever version it already is" -- `IDBFactory.open(name)`
  // with no second argument. The version is committed immediately: this
  // browser has one connection and one script turn per document, so nothing
  // blocks the upgrade transaction on another tab's `versionchange`.
  virtual OpenResult OpenDatabase(const std::string& name, std::uint64_t version) = 0;
  virtual void DeleteDatabase(const std::string& name) = 0;

  virtual bool CreateObjectStore(const std::string& db, const std::string& store,
                                 const IndexedDbKeyPath& key_path) = 0;
  virtual bool CreateIndex(const std::string& db, const std::string& store,
                           const std::string& index, bool unique) = 0;
  virtual std::vector<std::string> ObjectStoreNames(const std::string& db) = 0;
  virtual std::vector<std::string> IndexNames(const std::string& db,
                                              const std::string& store) = 0;
  virtual IndexedDbKeyPath ObjectStoreKeyPath(const std::string& db,
                                              const std::string& store) = 0;

  enum class PutResult { Stored, ConstraintError, QuotaExceeded, NotFound };
  struct IndexKeyEntry {
    std::string index;
    IndexedDbKeyValue key;
  };
  virtual PutResult Put(const std::string& db, const std::string& store,
                       const IndexedDbKeyValue& key, std::vector<std::uint8_t> value,
                       std::vector<IndexKeyEntry> index_keys) = 0;
  virtual std::optional<std::vector<std::uint8_t>> Get(const std::string& db,
                                                       const std::string& store,
                                                       const IndexedDbKeyValue& key) = 0;
  virtual bool Delete(const std::string& db, const std::string& store,
                      const IndexedDbKeyValue& key) = 0;

  struct CursorEntry {
    // The index key that matched, or the primary key again for a store's own
    // cursor.
    IndexedDbKeyValue key;
    IndexedDbKeyValue primary_key;
    std::vector<std::uint8_t> value;
  };
  // `index` empty means the store's own primary key; `only_key` unset means
  // every record -- the two shapes `IDBKeyRange` and a bare `openCursor()`
  // need here. See docs/adr/0038-broadcast-channel-and-indexeddb.md.
  virtual std::vector<CursorEntry> Query(const std::string& db, const std::string& store,
                                        const std::string& index,
                                        const std::optional<IndexedDbKeyValue>& only_key) = 0;
};

}  // namespace microbrowser::bindings

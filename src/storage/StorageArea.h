#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::storage {

// One origin's storage, in one partition: the map behind `sessionStorage` and
// `localStorage`.
//
// ADR 0021. Three properties are the whole of it, and each is here rather than at a
// caller for a reason:
//
//   * **Insertion order is preserved**, because `key(n)` and `length` are part of the
//     API and a page that enumerates its own keys must see a stable order. A hash map
//     alone cannot answer `key(0)`.
//   * **The quota is enforced at the store**, not checked by a caller. Storage is
//     memory, and unbounded storage from a page is a denial of service against the
//     process -- so it is a security bound, and a security bound with an opt-out is
//     not one.
//   * **A failed write changes nothing.** `SetItem` that would exceed the quota
//     leaves the previous value in place; the specified behaviour is to throw, and a
//     page that catches `QuotaExceededError` and retries must not find a half-written
//     value.
//
// Nothing here touches a disk. ADR 0021 §2 makes persistence a per-site user act that
// lands *with* encryption at rest or not at all, and this class is the memory tier
// that everything else is built on.
class StorageArea {
 public:
  // 5 MiB, per area, which is the number the web is written against: it is what
  // Safari and Chrome have both enforced for `localStorage` for a decade, so a page
  // that exceeds it already handles the failure. ADR 0021 §3 asks for a measured
  // number with the margin written down -- the measurement is that the target sites
  // store kilobytes (Plex's largest single value is a sign-in token; reddit's is a
  // preferences blob), so the margin here is three orders of magnitude.
  static constexpr std::size_t kQuotaBytes = 5u * 1024u * 1024u;

  explicit StorageArea(std::size_t quota = kQuotaBytes) : quota_(quota) {}

  std::size_t Length() const { return entries_.size(); }
  std::size_t Bytes() const { return bytes_; }

  // The key at `index` in insertion order, or nothing past the end. `key(n)` for an
  // out-of-range n is `null` in the specification rather than an error.
  std::optional<std::string> KeyAt(std::size_t index) const;

  std::optional<std::string> GetItem(std::string_view key) const;

  // False when the write would exceed the quota, in which case nothing changed and
  // the caller throws `QuotaExceededError`. Replacing a value counts only the
  // difference, so a page that rewrites one key forever does not grow.
  bool SetItem(std::string_view key, std::string_view value);

  // True when something was removed, which is what decides whether a `storage` event
  // has anything to report.
  bool RemoveItem(std::string_view key);

  // True when the area was not already empty, for the same reason.
  bool Clear();

 private:
  struct Entry {
    std::string key;
    std::string value;
  };

  // A vector rather than a map plus an order list. Storage areas hold tens of keys on
  // the pages measured, and a linear scan over tens of short strings beats a hash map
  // plus the second structure `key(n)` would need. If a page appears with thousands
  // of keys this is the measurement to take, and the counter to take it with is
  // `storage.lookups`.
  std::vector<Entry> entries_;
  std::size_t bytes_ = 0;
  std::size_t quota_ = kQuotaBytes;
};

}  // namespace microbrowser::storage

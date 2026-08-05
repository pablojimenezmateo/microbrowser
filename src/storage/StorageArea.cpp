#include "storage/StorageArea.h"

#include <algorithm>

#include "util/PerformanceCounters.h"

namespace microbrowser::storage {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// What one entry costs against the quota. Key and value both, in UTF-8 bytes, which
// is what this browser actually holds -- the specification counts UTF-16 code units,
// so a page storing non-ASCII gets slightly *more* room here than in another browser
// rather than slightly less. Erring in that direction means no page that fits
// elsewhere fails here.
std::size_t Cost(std::string_view key, std::string_view value) {
  return key.size() + value.size();
}

}  // namespace

std::optional<std::string> StorageArea::KeyAt(std::size_t index) const {
  if (index >= entries_.size()) {
    return std::nullopt;
  }
  return entries_[index].key;
}

std::optional<std::string> StorageArea::GetItem(std::string_view key) const {
  AddPerformanceCounter(PerfCounterId::StorageLookups);
  for (const Entry& entry : entries_) {
    if (entry.key == key) {
      return entry.value;
    }
  }
  return std::nullopt;
}

bool StorageArea::SetItem(std::string_view key, std::string_view value) {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [key](const Entry& entry) { return entry.key == key; });
  const std::size_t existing =
      found == entries_.end() ? 0u : Cost(found->key, found->value);
  const std::size_t wanted = Cost(key, value);
  // Against the quota with the *old* cost removed first, so rewriting one key with a
  // value of the same size never fails however many times it happens.
  if (bytes_ - existing + wanted > quota_) {
    AddPerformanceCounter(PerfCounterId::StorageQuotaRefusals);
    return false;
  }
  bytes_ = bytes_ - existing + wanted;
  if (found != entries_.end()) {
    found->value = std::string(value);
  } else {
    entries_.push_back(Entry{std::string(key), std::string(value)});
  }
  AddPerformanceCounter(PerfCounterId::StorageWrites);
  return true;
}

bool StorageArea::RemoveItem(std::string_view key) {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [key](const Entry& entry) { return entry.key == key; });
  if (found == entries_.end()) {
    return false;
  }
  bytes_ -= Cost(found->key, found->value);
  // Erase rather than tombstone: insertion order is what `key(n)` walks, and a
  // removed key must not leave a hole an enumeration would report as a key.
  entries_.erase(found);
  return true;
}

bool StorageArea::Clear() {
  if (entries_.empty()) {
    return false;
  }
  entries_.clear();
  bytes_ = 0;
  return true;
}

}  // namespace microbrowser::storage

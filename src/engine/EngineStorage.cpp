// Where a page's `sessionStorage` and `localStorage` reads land.
//
// ADR 0021. Its own translation unit for the reason EngineFetch.cpp and
// EngineHistory.cpp are: Engine.cpp is at its module cap, and the seam is a real one
// -- everything here is the same question asked six times, and the *answer* to all six
// is one function, `AreaFor`, which is the only place in this browser that turns a
// document into a storage partition.
//
// The interesting property is what this file does *not* let happen. `src/bindings` may
// not see `url` or `storage`, so a binding names Session or Local and nothing else;
// which partition that is comes from the document's own URL, here. There is no
// argument a page's bindings could get wrong, because there is no argument.

#include <optional>
#include <string>
#include <string_view>

#include "engine/Engine.h"
#include "storage/StorageArea.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::engine {

storage::StorageArea* Engine::AreaFor(bindings::StorageSource::Kind kind) {
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return nullptr;
  }
  // A top-level key from the document's own URL. When nested browsing contexts land
  // (ADR 0027) this becomes `ForEmbedded` with the top-level site of the frame tree,
  // and *that* is the line that makes `example.com` in `a.com` and `example.com` in
  // `b.com` different stores. It is one call site rather than six because of AreaFor.
  const url::PartitionKey key = url::PartitionKey::ForTopLevel(url::ContainerId::Default(), *base);
  // An opaque origin has no keyed storage at all -- `data:` and `about:blank` are not
  // a site, and giving them one would make two unrelated documents share it.
  if (key.GetOrigin().IsOpaque()) {
    return nullptr;
  }
  storage::PartitionedStorage& store =
      kind == bindings::StorageSource::Kind::Local ? local_storage_ : session_storage_;
  return &store.Lookup(key);
}

bool Engine::Available(bindings::StorageSource::Kind kind) { return AreaFor(kind) != nullptr; }

std::size_t Engine::Length(bindings::StorageSource::Kind kind) {
  const storage::StorageArea* area = AreaFor(kind);
  return area == nullptr ? 0u : area->Length();
}

std::optional<std::string> Engine::KeyAt(bindings::StorageSource::Kind kind,
                                        std::size_t index) {
  const storage::StorageArea* area = AreaFor(kind);
  return area == nullptr ? std::nullopt : area->KeyAt(index);
}

std::optional<std::string> Engine::GetItem(bindings::StorageSource::Kind kind,
                                           std::string_view key) {
  const storage::StorageArea* area = AreaFor(kind);
  return area == nullptr ? std::nullopt : area->GetItem(key);
}

Engine::WriteResult Engine::SetItem(bindings::StorageSource::Kind kind, std::string_view key,
                                    std::string_view value) {
  storage::StorageArea* area = AreaFor(kind);
  if (area == nullptr) {
    // A document with no keyed storage reports the write as having changed nothing.
    // Not a quota failure: the page did nothing wrong and `QuotaExceededError` would
    // send it down a "clear some space and retry" path that cannot help.
    return WriteResult::Unchanged;
  }
  const std::optional<std::string> before = area->GetItem(key);
  if (!area->SetItem(key, value)) {
    return WriteResult::QuotaExceeded;
  }
  // Whether the value actually changed, which is what a `storage` event reports. The
  // event itself fires in *other* documents of the same origin and partition, and this
  // browser has one document -- so it fires nowhere, which is what the specification
  // says for one tab. The comparison is here rather than at the tab work so that
  // "which writes are observable" is answered once, now, instead of by auditing every
  // write after tabs exist (ADR 0021 §5).
  const bool changed = !before.has_value() || *before != value;
  return changed ? WriteResult::Stored : WriteResult::Unchanged;
}

bool Engine::RemoveItem(bindings::StorageSource::Kind kind, std::string_view key) {
  storage::StorageArea* area = AreaFor(kind);
  return area != nullptr && area->RemoveItem(key);
}

bool Engine::Clear(bindings::StorageSource::Kind kind) {
  storage::StorageArea* area = AreaFor(kind);
  return area != nullptr && area->Clear();
}

}  // namespace microbrowser::engine

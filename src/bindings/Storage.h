#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::bindings {

// What `sessionStorage` and `localStorage` are answered through.
//
// ADR 0021, and the same inversion ADR 0015 used for geometry and ADR 0020 for the
// network: the interface is *declared here* and implemented by `src/engine`, because
// this module may see `util`, `js`, `dom` and `html` -- and not `url`, and not
// `storage`. A page's bindings therefore cannot hold a store, cannot name a partition
// key, and cannot ask for another partition's data even by accident.
//
// **The partition key does not appear in this file at all.** That is the security
// content of the seam: the key is `(container, top-level site, origin)` and it is
// derived from the document by the implementation, once, so there is no argument a
// binding could get wrong and no overload that omits it. ADR 0021 §1 requires the key
// on every store, and the way to require it of a caller that cannot spell it is to
// give the caller no way to say which store it means.
class StorageSource {
 public:
  // Which of the two, and it is the *only* thing a binding gets to choose. The two
  // differ in lifetime -- a session store dies with the tab, a local store with the
  // browser session -- and that is the implementation's business.
  enum class Kind { Session, Local };

  // What a write did. `QuotaExceeded` becomes a `QuotaExceededError` in script, which
  // is the specified failure and one real pages handle because Safari's quotas have
  // trained them to.
  enum class WriteResult { Stored, Unchanged, QuotaExceeded };

  StorageSource() = default;
  StorageSource(const StorageSource&) = delete;
  StorageSource& operator=(const StorageSource&) = delete;
  virtual ~StorageSource() = default;

  // Whether this document has keyed storage at all. False for an opaque origin -- a
  // `data:` URL, `about:blank` -- which is not a site and therefore has no partition
  // to key a store by.
  //
  // A separate question rather than an empty store, because an empty store is a lie a
  // page acts on: it writes, believes it saved, and reads back nothing. Every other
  // browser throws `SecurityError` here, and a page that touches storage on an opaque
  // origin is already written to survive it -- Plex's own splash-screen read is inside
  // a bare `try`/`catch`.
  virtual bool Available(Kind kind) = 0;

  virtual std::size_t Length(Kind kind) = 0;
  virtual std::optional<std::string> KeyAt(Kind kind, std::size_t index) = 0;
  virtual std::optional<std::string> GetItem(Kind kind, std::string_view key) = 0;
  virtual WriteResult SetItem(Kind kind, std::string_view key, std::string_view value) = 0;
  // True when something was removed. The return value exists because a `storage`
  // event reports only real changes, and whether a change happened is a fact the
  // store has and the binding does not.
  virtual bool RemoveItem(Kind kind, std::string_view key) = 0;
  virtual bool Clear(Kind kind) = 0;
};

}  // namespace microbrowser::bindings

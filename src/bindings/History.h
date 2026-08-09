#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "js/StructuredClone.h"

namespace microbrowser::bindings {

// Where `history` goes.
//
// Declared here, in the module that asks, and implemented by `src/engine` -- the
// same inversion `GeometrySource` and `NetworkSource` use. Here it is
// *load-bearing* rather than stylistic, and this is the whole reason the
// interface has this shape:
//
// **`src/bindings` may not see `url`.** ADR 0026 §2 makes the same-origin check
// on `pushState`'s URL the load-bearing line in the whole feature -- it is the
// only thing between a page and a perfect address-bar spoof -- and `url::Origin`
// is the one correct way to make that comparison. So the check cannot happen in
// this module. `PushState` therefore takes the URL as the page wrote it and
// answers with what it decided, and the binding turns a refusal into a
// `SecurityError`. A binding that received a parsed URL and compared hosts
// itself would be a second origin comparison, which ADR 0008 exists to prevent.
//
// The state travels as bytes for the reason ADR 0026 §1 gives: a history entry
// outlives the document that created it.
class HistorySource {
 public:
  virtual ~HistorySource() = default;

  // Why a `pushState` or `replaceState` did not happen. `Ok` is the only value
  // that moved anything.
  enum class UrlOutcome : std::uint8_t {
    Ok,
    // Did not parse against the document's base URL. A `SyntaxError`, which is
    // what the specification says and is distinguishable by a page from the
    // refusal below.
    Unparseable,
    // Parsed, and is not same-origin with the document. A `SecurityError`, and
    // the one this feature exists to be careful about.
    NotSameOrigin,
  };

  virtual std::size_t HistoryLength() const = 0;
  // The current entry's state. A reference into the entry, valid for the
  // duration of the call: the binding deserializes it immediately.
  virtual const js::SerializedValue& HistoryState() const = 0;
  // Bumped by anything that changes what `HistoryState()` answers. The binding
  // caches the deserialized object against it, because
  // `history.state === history.state` has to hold within a document and
  // deserializing on every read would answer with a new object each time.
  virtual std::uint64_t HistoryStateGeneration() const = 0;

  // `url` is empty when the page passed none, which means "keep the current
  // one". `replace` chooses between `replaceState` and `pushState`.
  virtual UrlOutcome PushHistoryState(const js::SerializedValue& state, std::string_view url,
                                      bool replace) = 0;

  // `history.go(delta)`, `back()` and `forward()`. Recorded rather than
  // performed: a traversal can replace the document, and doing that with the
  // interpreter on the stack is the use-after-free ADR 0026 §3 is written to
  // prevent -- the same reason a form submission from script is taken after the
  // turn ends.
  virtual void RequestHistoryTraversal(int delta) = 0;

  // `location.assign` / `location.replace` / `location.href = …`. Same deferred
  // boundary as traversal and form submit: a navigation tears the document down,
  // and doing that while the script that asked is still on the stack is the
  // use-after-free ADR 0026 §3 names. `url` is whatever the page wrote
  // (relative, absolute, or the current href); the engine resolves it. `replace`
  // chooses between assign (push a history entry) and replace (rewrite the
  // current one) — youtube's consent Accept sets SOCS then `location.assign`s
  // `consent.youtube.com/save?…`, and without this the dialog never leaves.
  virtual void RequestNavigation(std::string_view url, bool replace) = 0;
};

}  // namespace microbrowser::bindings

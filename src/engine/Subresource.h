#pragma once

#include <optional>
#include <string>

namespace microbrowser::engine {

// One subresource the document named: the URL exactly as written, plus the two
// attributes that decide *how* it is fetched and whether the bytes are
// acceptable when they arrive.
//
// A struct rather than three parallel vectors, and the reason is the pair at the
// bottom. `integrity` on a cross-origin resource requires `crossorigin`: without
// it the response would be opaque in a browser that has the process split, and
// here -- where it is not yet -- an integrity check on bytes the page may not
// read is an oracle for them, one guess at a time. The two have to be read
// together or that rule has nowhere to live. ADR 0020 §4.
//
// Its own header because both Page and PageScript hold a list of these, and
// Page.h includes PageScript.h rather than the other way round.
struct SubresourceRequest {
  std::string url;
  // The `integrity` attribute, empty when absent.
  std::string integrity;
  // The `crossorigin` attribute: absent, "anonymous", or "use-credentials".
  // Absent and empty are different -- `crossorigin=""` is "anonymous" -- which
  // is why this is an optional rather than a string.
  std::optional<std::string> cross_origin;
};

}  // namespace microbrowser::engine

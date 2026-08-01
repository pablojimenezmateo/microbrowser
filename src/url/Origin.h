#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "url/Url.h"

namespace microbrowser::url {

// A web origin: either the tuple (scheme, host, port), or opaque.
//
// Opaque origins are a real value here rather than a null, and that is the
// whole design. A `data:` URL, a sandboxed iframe and a `file:` document all
// have origins that are equal to themselves and to nothing else. Modelling that
// as "no origin" produces code that skips the check, which is how a sandboxed
// frame ends up same-origin with its parent.
class Origin {
 public:
  // An opaque origin, unique and equal only to itself.
  Origin() = default;

  static Origin FromUrl(const Url& url);

  bool IsOpaque() const { return opaque_; }
  const std::string& Scheme() const { return scheme_; }
  const std::string& Host() const { return host_; }
  std::optional<std::uint16_t> Port() const { return port_; }

  // "scheme://host:port", or "null" for an opaque origin — the serialization
  // that goes in an `Origin:` header and in `document.origin`. Two opaque
  // origins both serialize to "null" and are *not* equal, which is exactly why
  // comparison must never go through the serialization.
  std::string Serialize() const;

  // The only correct way to compare origins.
  bool IsSameOrigin(const Origin& other) const;

  // Whether the origin counts as a secure context. Not a synonym for "https":
  // localhost is trusted because it cannot be reached by an attacker on the
  // network, and that carve-out is what makes local development possible
  // without teaching people to click through warnings.
  bool IsPotentiallyTrustworthy() const;

  friend bool operator==(const Origin& a, const Origin& b) { return a.IsSameOrigin(b); }

 private:
  bool opaque_ = true;
  std::string scheme_;
  std::string host_;
  std::optional<std::uint16_t> port_;
  // Distinguishes one opaque origin from another. Opaque origins must not
  // compare equal to each other, and they cannot be told apart by their
  // contents, because they have none.
  std::uint64_t nonce_ = 0;
};

}  // namespace microbrowser::url

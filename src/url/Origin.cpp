#include "url/Origin.h"

#include <atomic>

namespace microbrowser::url {

namespace {

// Distinct opaque origins must not compare equal. A counter rather than a
// random number: the value never leaves the process, uniqueness within one run
// is the entire requirement, and a counter is trivially auditable where a
// generator invites questions about its seeding.
std::uint64_t NextOpaqueNonce() {
  static std::atomic<std::uint64_t> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

Origin Origin::FromUrl(const Url& url) {
  Origin origin;
  if (!url.IsValid()) {
    origin.nonce_ = NextOpaqueNonce();
    return origin;
  }

  // Only these schemes have tuple origins. Everything else — data:, blob: with
  // no resolvable inner URL, javascript:, and any scheme we do not know — is
  // opaque, which is the safe default rather than an omission.
  if (url.Scheme() == "http" || url.Scheme() == "https" || url.Scheme() == "ws" ||
      url.Scheme() == "wss" || url.Scheme() == "ftp") {
    origin.opaque_ = false;
    origin.scheme_ = url.Scheme();
    origin.host_ = url.HostSerialized();
    origin.port_ = url.Port();
    return origin;
  }

  // file: URLs get an opaque origin. Treating all local files as one origin is
  // how a downloaded HTML file reads the rest of the disk.
  origin.nonce_ = NextOpaqueNonce();
  return origin;
}

std::string Origin::Serialize() const {
  if (opaque_) {
    return "null";
  }
  std::string out = scheme_;
  out += "://";
  out += host_;
  if (port_.has_value()) {
    out.push_back(':');
    out += std::to_string(*port_);
  }
  return out;
}

bool Origin::IsSameOrigin(const Origin& other) const {
  if (opaque_ || other.opaque_) {
    // Two opaque origins are the same origin only if they are the *same* opaque
    // origin. Comparing serializations would make every one of them equal,
    // since they all serialize to "null".
    return opaque_ && other.opaque_ && nonce_ == other.nonce_;
  }
  return scheme_ == other.scheme_ && host_ == other.host_ && port_ == other.port_;
}

bool Origin::IsPotentiallyTrustworthy() const {
  if (opaque_) {
    return false;
  }
  if (scheme_ == "https" || scheme_ == "wss") {
    return true;
  }
  // Loopback. Parsed rather than string-matched, because `http://127.1` and
  // `http://0x7f.0.0.1` are loopback too and a string comparison misses both —
  // the Host parser has already canonicalized them by the time they reach here.
  if (host_ == "localhost" || host_ == "127.0.0.1" || host_ == "[::1]") {
    return true;
  }
  constexpr std::string_view kLocalSuffix = ".localhost";
  return host_.size() > kLocalSuffix.size() &&
         host_.compare(host_.size() - kLocalSuffix.size(), kLocalSuffix.size(), kLocalSuffix) == 0;
}

}  // namespace microbrowser::url

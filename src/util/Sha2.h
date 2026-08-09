#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace microbrowser::util {

// SHA-256, SHA-384 and SHA-512 (FIPS 180-4).
//
// **Why this is here and not a call into OpenSSL.** The TLS stack has these
// already, and ADR 0020 §4 says reaching them is a `MODULE.deps` question. It
// is, and the answer is no: `src/csp` may see `util` and `url` and nothing
// else, because a policy engine that could see `net` would be one `allow:` line
// from opening a socket -- and CSP's hash-sources need exactly this function.
// A digest in `net` would be a digest two of its three callers cannot reach.
//
// It is also the cheapest thing on this roadmap to be sure about: the algorithm
// is fully specified, it has published vectors, and the tests run them. What it
// must never grow into is a general crypto module -- HMAC lives in Hmac.h, not
// here, and nothing whose failure mode is silent.
//
// Bytes in, raw digest bytes out. Callers that want text encode it with
// util::Base64Encode.

enum class HashAlgorithm : std::uint8_t {
  Sha256,
  Sha384,
  Sha512,
};

// Digest length in bytes: 32, 48 or 64.
std::size_t HashLength(HashAlgorithm algorithm);

// The digest of `data`, as raw bytes.
std::string Sha2(HashAlgorithm algorithm, std::string_view data);

}  // namespace microbrowser::util

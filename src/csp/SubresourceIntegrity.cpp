#include "csp/SubresourceIntegrity.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "util/Base64.h"
#include "util/Sha2.h"
#include "util/StringUtil.h"

namespace microbrowser::csp {

namespace {

// One `<alg>-<base64>` token, parsed. The digest is decoded rather than kept as
// text for the reason a CSP hash-source is: base64 has more than one spelling of
// the same bytes, and an integrity check that compared strings would be one a
// second spelling of the right answer fails.
struct Hash {
  util::HashAlgorithm algorithm = util::HashAlgorithm::Sha256;
  std::string digest;
};

// Which algorithm wins when a page names several. The specification calls this
// the "strongest" metadata and leaves the ordering to the implementation; every
// browser uses this one, and it matters because a page that writes both
// sha256 and sha512 means "either of these bytes" only among equals.
int Strength(util::HashAlgorithm algorithm) {
  switch (algorithm) {
    case util::HashAlgorithm::Sha256:
      return 1;
    case util::HashAlgorithm::Sha384:
      return 2;
    case util::HashAlgorithm::Sha512:
      return 3;
  }
  return 0;
}

std::optional<Hash> ParseToken(std::string_view token) {
  // `?options` after the digest is allowed by the grammar and has no defined
  // meaning yet. Dropped rather than rejected: a page that writes one must not
  // lose its integrity check over it.
  if (const std::size_t question = token.find('?'); question != std::string_view::npos) {
    token = token.substr(0, question);
  }
  const std::size_t dash = token.find('-');
  if (dash == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view algorithm_name = token.substr(0, dash);
  util::HashAlgorithm algorithm{};
  if (util::EqualsAsciiCaseInsensitive(algorithm_name, "sha256")) {
    algorithm = util::HashAlgorithm::Sha256;
  } else if (util::EqualsAsciiCaseInsensitive(algorithm_name, "sha384")) {
    algorithm = util::HashAlgorithm::Sha384;
  } else if (util::EqualsAsciiCaseInsensitive(algorithm_name, "sha512")) {
    algorithm = util::HashAlgorithm::Sha512;
  } else {
    // `sha1-`, or anything else. Not a hash this browser will accept, and
    // dropping it is what makes an `integrity` of nothing but weak algorithms
    // the same as no `integrity` -- which is the specification's answer.
    return std::nullopt;
  }
  const std::optional<std::string> digest = util::Base64Decode(token.substr(dash + 1));
  if (!digest.has_value() || digest->size() != util::HashLength(algorithm)) {
    return std::nullopt;
  }
  return Hash{algorithm, *digest};
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::vector<Hash> ParseMetadata(std::string_view metadata) {
  std::vector<Hash> hashes;
  std::size_t i = 0;
  while (i < metadata.size()) {
    while (i < metadata.size() && IsSpace(metadata[i])) {
      ++i;
    }
    const std::size_t start = i;
    while (i < metadata.size() && !IsSpace(metadata[i])) {
      ++i;
    }
    if (i == start) {
      continue;
    }
    if (std::optional<Hash> hash = ParseToken(metadata.substr(start, i - start))) {
      hashes.push_back(std::move(*hash));
    }
  }
  return hashes;
}

}  // namespace

bool HasIntegrityMetadata(std::string_view metadata) {
  return !ParseMetadata(metadata).empty();
}

IntegrityResult CheckIntegrity(std::string_view metadata, std::string_view bytes) {
  const std::vector<Hash> hashes = ParseMetadata(metadata);
  if (hashes.empty()) {
    return IntegrityResult::NoMetadata;
  }
  int strongest = 0;
  for (const Hash& hash : hashes) {
    strongest = std::max(strongest, Strength(hash.algorithm));
  }
  // One digest per algorithm at most, and only for the strongest one present: a
  // page that lists four sha512 hashes for four acceptable builds costs one
  // hash of the resource, not four.
  std::optional<std::string> computed;
  for (const Hash& hash : hashes) {
    if (Strength(hash.algorithm) != strongest) {
      continue;
    }
    if (!computed.has_value()) {
      computed = util::Sha2(hash.algorithm, bytes);
    }
    if (*computed == hash.digest) {
      return IntegrityResult::Match;
    }
  }
  return IntegrityResult::Mismatch;
}

}  // namespace microbrowser::csp

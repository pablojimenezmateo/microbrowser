#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::privacy {

// What a page asked the network for. Named exactly as filter lists name them,
// because the options in a rule (`$script`, `$image`, `$third-party`) are
// matched against these and a mismatch in vocabulary becomes a mismatch in
// behavior.
enum class ResourceType : std::uint16_t {
  Document = 1u << 0,
  Subdocument = 1u << 1,
  Stylesheet = 1u << 2,
  Script = 1u << 3,
  Image = 1u << 4,
  Font = 1u << 5,
  Media = 1u << 6,
  Xhr = 1u << 7,
  WebSocket = 1u << 8,
  Ping = 1u << 9,
  Other = 1u << 10,
};

constexpr std::uint16_t kAllResourceTypes = 0x07FF;

constexpr std::uint16_t ResourceTypeBit(ResourceType type) {
  return static_cast<std::uint16_t>(type);
}

// One request, with everything a policy decision needs and nothing else.
//
// The partition key is carried rather than derived, because deriving it here
// would let two call sites disagree about what partition a request belongs to —
// and a partition key that is computed in more than one place is a partition
// key that is eventually computed two ways.
struct Request {
  url::Url url;
  // The document that caused the request. Opaque for a top-level navigation,
  // which is not a special case: a navigation has no initiator page.
  url::Origin initiator;
  url::Site top_level_site;
  url::ContainerId container;
  ResourceType type = ResourceType::Other;
  // False only for a user-caused navigation. Everything else a page asks for is
  // a subresource, and the distinction decides whether HTTPS-only can show an
  // interstitial or must simply refuse.
  bool is_subresource = true;

  // Third-party is by *site*, not origin: `a.example.com` embedded in
  // `b.example.com` is first-party, which is what filter lists mean by `$1p`
  // and what cookie policy means by same-site.
  bool IsThirdParty() const;
};

}  // namespace microbrowser::privacy

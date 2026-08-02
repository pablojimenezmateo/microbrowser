#pragma once

#include <cstdint>
#include <string>

#include "privacy/Request.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::privacy {

// The privacy layer's decision about one request.
//
// `net::Fetch` takes one of these **by value, with no overload that omits it**.
// That is deliberately not a convention: a convention is a thing a reviewer has
// to notice, and this is a signature that cannot be bypassed without editing
// the signature. `guidelines/privacy.md` states the rule; this type is what
// makes it enforceable.
//
// A Verdict is also the *only* place a request's final URL is decided. The
// upgraded scheme, the stripped tracking parameters and the trimmed referrer
// all live here rather than being applied by whoever happens to build the
// socket write, because a second place that rewrites URLs is a second place
// that can forget to.
class Verdict {
 public:
  enum class Decision : std::uint8_t { Allow, Block, BlockInsecure };

  static Verdict Allowed(url::Url final_url, url::PartitionKey key, ResourceType type,
                         bool is_subresource);
  static Verdict Blocked(std::string_view reason);
  static Verdict BlockedInsecure(std::string_view reason);

  Decision GetDecision() const { return decision_; }
  bool IsAllowed() const { return decision_ == Decision::Allow; }

  const url::Url& FinalUrl() const { return final_url_; }
  const url::PartitionKey& Partition() const { return partition_; }

  const std::string& Referrer() const { return referrer_; }
  void SetReferrer(std::string referrer);

  ResourceType Type() const { return type_; }
  bool IsSubresource() const;

  bool WasUpgraded() const;
  bool WasSanitized() const;
  void MarkUpgraded();
  void MarkSanitized();

  const std::string& Reason() const { return reason_; }

 private:
  enum Flag : std::uint8_t { Upgraded = 1u << 0, Sanitized = 1u << 1, Subresource = 1u << 2 };

  bool HasFlag(Flag flag) const;
  void SetFlag(Flag flag, bool enabled);

  Decision decision_ = Decision::Block;
  url::Url final_url_;
  url::PartitionKey partition_;
  std::string referrer_;
  std::string reason_;
  ResourceType type_ = ResourceType::Other;
  std::uint8_t flags_ = 0;
};

}  // namespace microbrowser::privacy

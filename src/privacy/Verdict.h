#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
  enum class Decision : std::uint8_t {
    Allow,
    // Blocked by a filter rule. The request is never made.
    Block,
    // Refused because it could not be made securely — HTTPS-only with no
    // upgrade available, or a downgrade the user has not permitted.
    BlockInsecure,
  };

  static Verdict Allowed(url::Url final_url, url::PartitionKey key);
  static Verdict Blocked(std::string_view reason);
  static Verdict BlockedInsecure(std::string_view reason);

  Decision GetDecision() const { return decision_; }
  bool IsAllowed() const { return decision_ == Decision::Allow; }

  // Valid only when allowed. This is the URL that will actually be requested,
  // after the HTTPS upgrade and after tracking parameters were removed — not
  // the one the page asked for.
  const url::Url& FinalUrl() const { return final_url_; }

  // The partition every piece of state this request touches must be keyed by:
  // the connection, the cookies, the cache entry, the TLS session ticket.
  const url::PartitionKey& Partition() const { return partition_; }

  // What to send as `Referer`, already trimmed. Empty means send none.
  const std::string& Referrer() const { return referrer_; }
  void SetReferrer(std::string referrer) { referrer_ = std::move(referrer); }

  // True when the URL was rewritten from what the page asked for. Only
  // observability wants this; the decision is in FinalUrl either way.
  bool WasUpgraded() const { return upgraded_; }
  bool WasSanitized() const { return sanitized_; }
  void MarkUpgraded() { upgraded_ = true; }
  void MarkSanitized() { sanitized_ = true; }

  // Why, for a log line and a test message. Never shown to a page: telling a
  // page which rule blocked it turns the filter list into a fingerprinting
  // surface.
  const std::string& Reason() const { return reason_; }

 private:
  Decision decision_ = Decision::Block;
  url::Url final_url_;
  url::PartitionKey partition_;
  std::string referrer_;
  std::string reason_;
  bool upgraded_ = false;
  bool sanitized_ = false;
};

}  // namespace microbrowser::privacy

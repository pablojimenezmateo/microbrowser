#include "privacy/Verdict.h"

#include <utility>

namespace microbrowser::privacy {

Verdict Verdict::Allowed(url::Url final_url, url::PartitionKey key, ResourceType type,
                         bool is_subresource) {
  Verdict verdict;
  verdict.decision_ = Decision::Allow;
  verdict.final_url_ = std::move(final_url);
  verdict.partition_ = std::move(key);
  verdict.type_ = type;
  verdict.SetFlag(Subresource, is_subresource);
  return verdict;
}

Verdict Verdict::Blocked(std::string_view reason) {
  Verdict verdict;
  verdict.decision_ = Decision::Block;
  verdict.reason_ = reason;
  return verdict;
}

Verdict Verdict::BlockedInsecure(std::string_view reason) {
  Verdict verdict;
  verdict.decision_ = Decision::BlockInsecure;
  verdict.reason_ = reason;
  return verdict;
}

void Verdict::SetReferrer(std::string referrer) {
  referrer_ = std::move(referrer);
}

bool Verdict::IsSubresource() const {
  return HasFlag(Subresource);
}

bool Verdict::WasUpgraded() const {
  return HasFlag(Upgraded);
}

bool Verdict::WasSanitized() const {
  return HasFlag(Sanitized);
}

void Verdict::MarkUpgraded() {
  SetFlag(Upgraded, true);
}

void Verdict::MarkSanitized() {
  SetFlag(Sanitized, true);
}

bool Verdict::HasFlag(Flag flag) const {
  return (flags_ & flag) != 0;
}

void Verdict::SetFlag(Flag flag, bool enabled) {
  if (enabled) {
    flags_ |= flag;
  } else {
    flags_ &= static_cast<std::uint8_t>(~flag);
  }
}

}  // namespace microbrowser::privacy

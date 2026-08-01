#include "privacy/Verdict.h"

namespace microbrowser::privacy {

Verdict Verdict::Allowed(url::Url final_url, url::PartitionKey key) {
  Verdict verdict;
  verdict.decision_ = Decision::Allow;
  verdict.final_url_ = std::move(final_url);
  verdict.partition_ = std::move(key);
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

}  // namespace microbrowser::privacy

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "privacy/Request.h"
#include "util/TransparentStringHash.h"

namespace microbrowser::privacy {

// One compiled filter rule, as a flat record.
//
// Every field is an offset or a bitmask into arenas held by the engine — no
// pointers, no `std::string` per rule. Three hundred thousand rules as three
// hundred thousand heap objects is the difference between a few megabytes and a
// few hundred, and the size of this struct is multiplied by the list, so it
// carries a budget for the reason ADR 0002 gives.
struct CompiledRule {
  std::uint32_t pattern_offset = 0;
  std::uint32_t pattern_length = 0;
  // Into the domain arena; `domain_count` entries of `$domain=`.
  std::uint32_t domain_offset = 0;
  std::uint32_t parameter_offset = 0;  // `$removeparam=`
  std::uint16_t domain_count = 0;
  std::uint16_t parameter_length = 0;
  std::uint16_t resource_types = kAllResourceTypes;
  std::uint8_t flags = 0;
  std::uint8_t reserved = 0;

  enum Flag : std::uint8_t {
    Exception = 1u << 0,       // @@
    HostAnchored = 1u << 1,    // ||
    StartAnchored = 1u << 2,   // |http...
    EndAnchored = 1u << 3,     // ...|
    ThirdPartyOnly = 1u << 4,  // $third-party
    FirstPartyOnly = 1u << 5,  // $1p
    Important = 1u << 6,       // $important, beats an exception
    RemoveParam = 1u << 7,     // $removeparam, rewrites rather than blocks
  };

  bool Has(Flag flag) const { return (flags & static_cast<std::uint8_t>(flag)) != 0; }
};

static_assert(sizeof(CompiledRule) <= 24, "a rule's size is multiplied by the whole list");

// What matching a request produced.
struct MatchResult {
  bool blocked = false;
  // Parameters a `$removeparam` rule asked to strip. Borrowed from the engine's
  // arena, so valid only while the engine is alive and unmodified.
  std::vector<std::string_view> removed_parameters;
  // Index of the deciding rule, for a log line. Never given to a page.
  std::uint32_t rule = 0;
};

// The content blocking engine: compile once, probe a few buckets per request.
//
// The architecture is uBlock Origin's rather than merely its syntax — see
// ADR 0006. A request does not test three hundred thousand patterns; it walks
// the hostname's suffixes against the host index, probes only the token buckets
// for tokens the URL actually contains, and consults the exception set only
// after something matched.
//
// **A filter list may never supply code.** This engine returns decisions and
// parameter names. Scriptlets and redirect resources are named entries in a
// table compiled into the binary, and a list contributes a name and arguments —
// because a list is third-party text and script execution on every site is a
// better position than most browser exploits achieve.
class BlockingEngine {
 public:
  // Adds rules from filter list text. Unrecognized rules are skipped and
  // counted, never guessed at: a partially-understood filter fails open on the
  // request it was written to block, silently, which is worse than no rule.
  void AddRules(std::string_view list_text);

  MatchResult Match(const Request& request) const;

  std::size_t RuleCount() const { return rules_.size(); }
  std::size_t SkippedRuleCount() const { return skipped_; }

 private:
  struct Index {
    // Hostname to rule indices, walked over the request host's suffixes so that
    // `||example.com^` covers every subdomain without a loop over patterns.
    std::unordered_map<std::string, std::vector<std::uint32_t>, util::TransparentStringHash,
                       std::equal_to<>>
        by_host;
    // The uBO trick: each pattern contributes its most selective literal token,
    // and a request probes only the buckets for tokens it actually contains.
    std::unordered_map<std::string, std::vector<std::uint32_t>, util::TransparentStringHash,
                       std::equal_to<>>
        by_token;
    // Patterns with no usable token. Tested every time, so kept small on
    // purpose — a list that grows these grows the part with no index.
    std::vector<std::uint32_t> unindexed;
  };

  void AddRule(std::string_view line);
  bool MatchesRule(const CompiledRule& rule, const Request& request, std::string_view url,
                   std::string_view host) const;
  void Collect(const Index& index, std::string_view url, std::string_view host,
               std::vector<std::uint32_t>& out) const;

  std::string arena_;
  std::vector<CompiledRule> rules_;
  std::vector<std::string> domains_;
  Index block_;
  Index allow_;
  std::size_t skipped_ = 0;
};

// Whether `pattern` matches `text`, with `*` wildcards and `^` separators, per
// the Adblock Plus pattern syntax. Exposed so it can be tested directly: it is
// the one piece of the engine where an off-by-one is a rule that silently stops
// matching.
bool PatternMatches(std::string_view pattern, std::string_view text, bool start_anchored,
                    bool end_anchored);

}  // namespace microbrowser::privacy

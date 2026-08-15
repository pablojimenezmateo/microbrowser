#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/StyleSheet.h"

namespace microbrowser::css {

// CSSOM's CSSRule.type. Unknown at-rules are kept (type 0) rather than dropped,
// because a cssRules list that omitted them would be short -- ADR 0012.
enum class CssomRuleType : std::uint16_t {
  Unknown = 0,
  Style = 1,
  Import = 3,
  Media = 4,
  FontFace = 5,
  Keyframes = 7,
  Namespace = 10,
  Supports = 12,
};

struct CssomRule {
  CssomRuleType type = CssomRuleType::Unknown;
  std::string css_text;
  std::string prelude;
  std::string at_name;
  std::vector<Declaration> declarations;
  std::vector<CssomRule> children;

  friend bool operator==(const CssomRule&, const CssomRule&) = default;
};

// Top-level CSSOM rules, with `@media` / `@supports` nested rather than flattened.
std::vector<CssomRule> ParseCssom(std::string_view source);

}  // namespace microbrowser::css

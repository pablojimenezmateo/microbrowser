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
  Page = 6,
  Keyframes = 7,
  Margin = 9,
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

// CSSOM's serialize-a-selector-list. Empty when `text` does not parse.
std::string SerializeSelectorList(const std::vector<Selector>& selectors);
std::string SerializeSelectorList(std::string_view text);

std::string JoinCssomRules(const std::vector<CssomRule>& rules);
void RefreshCssomCssText(CssomRule& rule);

// CSSStyleRule.selectorText. False when the selector does not parse -- the
// assignment is then a no-op, which is what the CSSOM says.
bool SetCssomSelectorText(CssomRule& rule, std::string_view selector);

// CSSPageRule.selectorText. False when the selector is not a page selector --
// the assignment is then a no-op.
bool SetCssomPageSelectorText(CssomRule& rule, std::string_view selector);

}  // namespace microbrowser::css

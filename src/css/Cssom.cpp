#include "css/Cssom.h"

#include "css/CssText.h"
#include "css/Tokenizer.h"

namespace microbrowser::css {
namespace {

std::size_t FindBlockEnd(const std::vector<Token>& tokens, std::size_t open, std::size_t end) {
  int depth = 1;
  std::size_t at = open + 1;
  while (at < end && tokens[at].kind != Token::Kind::EndOfFile && depth > 0) {
    if (tokens[at].kind == Token::Kind::LeftBrace) {
      ++depth;
    } else if (tokens[at].kind == Token::Kind::RightBrace) {
      --depth;
      if (depth == 0) {
        break;
      }
    }
    ++at;
  }
  return at;
}

CssomRuleType AtRuleType(std::string_view name) {
  if (name == "media") {
    return CssomRuleType::Media;
  }
  if (name == "supports") {
    return CssomRuleType::Supports;
  }
  if (name == "font-face") {
    return CssomRuleType::FontFace;
  }
  if (name == "keyframes") {
    return CssomRuleType::Keyframes;
  }
  if (name == "import") {
    return CssomRuleType::Import;
  }
  if (name == "namespace") {
    return CssomRuleType::Namespace;
  }
  return CssomRuleType::Unknown;
}

std::string SerializeDeclarations(const std::vector<Declaration>& declarations) {
  std::string out;
  for (const Declaration& declaration : declarations) {
    if (!out.empty()) {
      out += ' ';
    }
    out += declaration.property;
    out += ": ";
    out += declaration.value;
    if (declaration.important) {
      out += " !important";
    }
    out += ';';
  }
  return out;
}

std::string StyleCssText(std::string_view selector, const std::vector<Declaration>& declarations) {
  std::string out;
  out += selector;
  out += " { ";
  out += SerializeDeclarations(declarations);
  if (!declarations.empty()) {
    out += ' ';
  }
  out += '}';
  return out;
}

std::vector<CssomRule> ParseRuleList(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to);

CssomRule AtRule(const std::vector<Token>& tokens, std::size_t at_index, std::size_t prelude_end,
                 std::size_t block_start, std::size_t block_end, bool has_block) {
  CssomRule rule;
  rule.at_name = Lowered(tokens[at_index].value);
  rule.type = AtRuleType(rule.at_name);
  rule.prelude = ReconstructTokens(tokens, at_index + 1, prelude_end);
  if (rule.at_name == "charset") {
    rule.type = CssomRuleType::Unknown;
  }
  if (has_block) {
    if (rule.type == CssomRuleType::Media || rule.type == CssomRuleType::Supports) {
      rule.children = ParseRuleList(tokens, block_start, block_end);
    } else if (rule.type == CssomRuleType::FontFace) {
      rule.declarations = ParseDeclarationList(ReconstructTokens(tokens, block_start, block_end));
    }
    std::string inner;
    if (!rule.children.empty()) {
      for (const CssomRule& child : rule.children) {
        if (!inner.empty()) {
          inner += ' ';
        }
        inner += child.css_text;
      }
    } else {
      inner = SerializeDeclarations(rule.declarations);
    }
    rule.css_text = "@" + rule.at_name;
    if (!rule.prelude.empty()) {
      rule.css_text += ' ';
      rule.css_text += rule.prelude;
    }
    rule.css_text += " { ";
    rule.css_text += inner;
    if (!inner.empty()) {
      rule.css_text += ' ';
    }
    rule.css_text += '}';
  } else {
    rule.css_text = "@" + rule.at_name;
    if (!rule.prelude.empty()) {
      rule.css_text += ' ';
      rule.css_text += rule.prelude;
    }
    rule.css_text += ';';
  }
  return rule;
}

std::vector<CssomRule> ParseRuleList(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to) {
  std::vector<CssomRule> rules;
  std::size_t at = from;
  while (at < to && tokens[at].kind != Token::Kind::EndOfFile) {
    if (tokens[at].kind == Token::Kind::Whitespace || tokens[at].kind == Token::Kind::Cdo ||
        tokens[at].kind == Token::Kind::Cdc) {
      ++at;
      continue;
    }
    if (tokens[at].kind == Token::Kind::AtKeyword) {
      const std::size_t at_index = at;
      ++at;
      while (at < to && tokens[at].kind != Token::Kind::LeftBrace &&
             tokens[at].kind != Token::Kind::Semicolon &&
             tokens[at].kind != Token::Kind::EndOfFile) {
        ++at;
      }
      if (at >= to || tokens[at].kind == Token::Kind::EndOfFile) {
        break;
      }
      if (tokens[at].kind == Token::Kind::Semicolon) {
        const std::string name = Lowered(tokens[at_index].value);
        // `@charset` is not a CSSRule. Omitting it is the spec, not a short list.
        if (name != "charset") {
          rules.push_back(AtRule(tokens, at_index, at, 0, 0, false));
        }
        ++at;
        continue;
      }
      const std::size_t prelude_end = at;
      const std::size_t block_start = at + 1;
      const std::size_t block_end = FindBlockEnd(tokens, at, to);
      at = block_end;
      if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
        ++at;
      }
      rules.push_back(AtRule(tokens, at_index, prelude_end, block_start, block_end, true));
      continue;
    }

    const std::size_t prelude_start = at;
    while (at < to && tokens[at].kind != Token::Kind::LeftBrace &&
           tokens[at].kind != Token::Kind::EndOfFile) {
      ++at;
    }
    if (at >= to || tokens[at].kind != Token::Kind::LeftBrace) {
      break;
    }
    const std::size_t prelude_end = at;
    const std::size_t block_start = at + 1;
    const std::size_t block_end = FindBlockEnd(tokens, at, to);
    at = block_end;
    if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
      ++at;
    }
    CssomRule rule;
    rule.type = CssomRuleType::Style;
    rule.prelude = ReconstructTokens(tokens, prelude_start, prelude_end);
    if (rule.prelude.empty()) {
      continue;
    }
    rule.declarations = ParseDeclarationList(ReconstructTokens(tokens, block_start, block_end));
    rule.css_text = StyleCssText(rule.prelude, rule.declarations);
    rules.push_back(std::move(rule));
  }
  return rules;
}

}  // namespace

std::vector<CssomRule> ParseCssom(std::string_view source) {
  const std::vector<Token> tokens = Tokenize(source);
  return ParseRuleList(tokens, 0, tokens.size());
}

}  // namespace microbrowser::css

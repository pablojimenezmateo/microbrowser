#include "css/Cssom.h"

#include <cstdio>

#include "css/CssText.h"
#include "css/DeclarationText.h"
#include "css/Tokenizer.h"
#include "util/StringUtil.h"

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
  if (name == "page") {
    return CssomRuleType::Page;
  }
  if (name == "keyframes") {
    return CssomRuleType::Keyframes;
  }
  if (name == "top-left-corner" || name == "top-left" || name == "top-center" ||
      name == "top-right" || name == "top-right-corner" || name == "bottom-left-corner" ||
      name == "bottom-left" || name == "bottom-center" || name == "bottom-right" ||
      name == "bottom-right-corner" || name == "left-top" || name == "left-middle" ||
      name == "left-bottom" || name == "right-top" || name == "right-middle" ||
      name == "right-bottom") {
    return CssomRuleType::Margin;
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

bool IsIdentStart(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

bool IsIdent(unsigned char c) {
  return IsIdentStart(c) || (c >= '0' && c <= '9') || c == '-';
}

void AppendHexEscape(std::string& out, unsigned value) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "\\%x ", value);
  out += buf;
}

std::string SerializeCssIdent(std::string_view ident) {
  std::string out;
  for (std::size_t i = 0; i < ident.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(ident[i]);
    if (c == 0) {
      out += "\xEF\xBF\xBD";
      continue;
    }
    const bool leading_digit = i == 0 && c >= '0' && c <= '9';
    const bool dash_digit = i == 1 && ident[0] == '-' && c >= '0' && c <= '9';
    if (c < 0x20 || c == 0x7F || leading_digit || dash_digit || !IsIdent(c) ||
        (i == 0 && c == '-' && ident.size() == 1)) {
      AppendHexEscape(out, c);
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  return out;
}

std::string SerializeCssString(std::string_view text) {
  std::string out = "\"";
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == 0) {
      out += "\\0 ";
    } else if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      AppendHexEscape(out, c);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out += '"';
  return out;
}

std::string SerializeNth(const NthPattern& nth) {
  if (nth.a == 0) {
    return std::to_string(nth.b);
  }
  std::string out;
  if (nth.a == 1) {
    out += 'n';
  } else if (nth.a == -1) {
    out += "-n";
  } else {
    out += std::to_string(nth.a);
    out += 'n';
  }
  if (nth.b > 0) {
    out += '+';
    out += std::to_string(nth.b);
  } else if (nth.b < 0) {
    out += std::to_string(nth.b);
  }
  return out;
}

std::string SerializeSelectorListInner(const std::vector<Selector>& selectors);

std::string SerializeNamespacePrefix(SelectorPart::NamespaceMatch name_space, bool attribute) {
  if (name_space == SelectorPart::NamespaceMatch::Any) {
    // Types: this engine has no default namespace, so `*|div` and `div` match
    // the same set and CSSOM serializes the shorter form. Attributes keep `*|`
    // because `[attr]` is the null namespace, not any.
    return attribute ? "*|" : "";
  }
  if (name_space == SelectorPart::NamespaceMatch::None) {
    // `[|attr]` and `[attr]` match the same set (attributes have no default
    // namespace). CSSOM serializes the shorter form.
    return attribute ? "" : "|";
  }
  return {};
}

bool IsLegacyPseudoElementName(std::string_view name) {
  return name == "before" || name == "after" || name == "first-letter" || name == "first-line";
}

std::string SerializeSelectorPart(const SelectorPart& part) {
  switch (part.kind) {
    case SelectorPart::Kind::Universal:
      return SerializeNamespacePrefix(part.name_space, false) + "*";
    case SelectorPart::Kind::Type:
      return SerializeNamespacePrefix(part.name_space, false) + SerializeCssIdent(part.name);
    case SelectorPart::Kind::Class:
      return "." + SerializeCssIdent(part.name);
    case SelectorPart::Kind::Id:
      return "#" + SerializeCssIdent(part.name);
    case SelectorPart::Kind::Attribute: {
      std::string out = "[";
      out += SerializeNamespacePrefix(part.name_space, true);
      out += SerializeCssIdent(part.name);
      if (part.match != SelectorPart::AttributeMatch::Exists) {
        switch (part.match) {
          case SelectorPart::AttributeMatch::Equals:
            out += '=';
            break;
          case SelectorPart::AttributeMatch::Includes:
            out += "~=";
            break;
          case SelectorPart::AttributeMatch::DashMatch:
            out += "|=";
            break;
          case SelectorPart::AttributeMatch::Prefix:
            out += "^=";
            break;
          case SelectorPart::AttributeMatch::Suffix:
            out += "$=";
            break;
          case SelectorPart::AttributeMatch::Substring:
            out += "*=";
            break;
          case SelectorPart::AttributeMatch::Exists:
            break;
        }
        out += SerializeCssString(part.value);
        if (part.attribute_case == SelectorPart::AttributeCase::Insensitive) {
          out += " i";
        } else if (part.attribute_case == SelectorPart::AttributeCase::Sensitive) {
          out += " s";
        }
      }
      out += ']';
      return out;
    }
    case SelectorPart::Kind::PseudoClass:
      if (IsLegacyPseudoElementName(part.name)) {
        return "::" + part.name;
      }
      return ":" + part.name;
    case SelectorPart::Kind::PseudoElement:
      return "::" + part.name;
    case SelectorPart::Kind::Is:
      return ":is(" + SerializeSelectorListInner(part.arguments) + ")";
    case SelectorPart::Kind::Where:
      return ":where(" + SerializeSelectorListInner(part.arguments) + ")";
    case SelectorPart::Kind::Not:
      return ":not(" + SerializeSelectorListInner(part.arguments) + ")";
    case SelectorPart::Kind::Has:
      return ":has(" + SerializeSelectorListInner(part.arguments) + ")";
    case SelectorPart::Kind::Nth: {
      const char* name = "nth-child";
      if (part.nth.of_type && part.nth.from_end) {
        name = "nth-last-of-type";
      } else if (part.nth.of_type) {
        name = "nth-of-type";
      } else if (part.nth.from_end) {
        name = "nth-last-child";
      }
      std::string out = ":";
      out += name;
      out += '(';
      out += SerializeNth(part.nth);
      if (!part.arguments.empty()) {
        out += " of ";
        out += SerializeSelectorListInner(part.arguments);
      }
      out += ')';
      return out;
    }
    case SelectorPart::Kind::Lang:
      return ":lang(" + part.value + ")";
    case SelectorPart::Kind::Dir:
      return ":dir(" + part.value + ")";
    case SelectorPart::Kind::Host:
      if (part.arguments.empty()) {
        return ":host";
      }
      return ":host(" + SerializeSelectorListInner(part.arguments) + ")";
    case SelectorPart::Kind::Scope:
      return ":scope";
    case SelectorPart::Kind::Slotted:
      return "::slotted(" + SerializeSelectorListInner(part.arguments) + ")";
  }
  return {};
}

std::string SerializeCompound(const CompoundSelector& compound) {
  std::string out;
  const bool drop_universal = compound.parts.size() > 1;
  for (const SelectorPart& part : compound.parts) {
    if (drop_universal && part.kind == SelectorPart::Kind::Universal &&
        part.name_space != SelectorPart::NamespaceMatch::None) {
      continue;
    }
    out += SerializeSelectorPart(part);
  }
  return out.empty() ? "*" : out;
}

std::string SerializeOneSelector(const Selector& selector) {
  std::string out;
  for (const CompoundSelector& compound : selector.compounds) {
    if (!out.empty()) {
      switch (compound.combinator) {
        case Combinator::Child:
          out += " > ";
          break;
        case Combinator::NextSibling:
          out += " + ";
          break;
        case Combinator::LaterSibling:
          out += " ~ ";
          break;
        case Combinator::Descendant:
        case Combinator::None:
          out += ' ';
          break;
      }
    } else if (compound.combinator == Combinator::Child) {
      out += "> ";
    } else if (compound.combinator == Combinator::NextSibling) {
      out += "+ ";
    } else if (compound.combinator == Combinator::LaterSibling) {
      out += "~ ";
    }
    out += SerializeCompound(compound);
  }
  return out;
}

std::string SerializeSelectorListInner(const std::vector<Selector>& selectors) {
  std::string out;
  for (const Selector& selector : selectors) {
    if (!out.empty()) {
      out += ", ";
    }
    out += SerializeOneSelector(selector);
  }
  return out;
}

std::string CanonicalSelectorText(std::string_view prelude) {
  const std::vector<Selector> selectors = ParseSelectorList(prelude);
  if (!selectors.empty()) {
    return SerializeSelectorListInner(selectors);
  }
  return {};
}

void WriteCssomCssText(CssomRule& rule) {
  if (rule.type == CssomRuleType::Style) {
    rule.css_text = StyleCssText(rule.prelude, rule.declarations);
    return;
  }
  if (rule.at_name.empty()) {
    return;
  }
  std::string inner;
  if (!rule.children.empty()) {
    for (CssomRule& child : rule.children) {
      WriteCssomCssText(child);
      if (!inner.empty()) {
        inner += ' ';
      }
      inner += child.css_text;
    }
  } else {
    inner = SerializeDeclarations(rule.declarations);
  }
  const bool has_block = rule.type != CssomRuleType::Import && rule.type != CssomRuleType::Namespace;
  rule.css_text = "@" + rule.at_name;
  if (!rule.prelude.empty()) {
    rule.css_text += ' ';
    rule.css_text += rule.prelude;
  }
  if (has_block) {
    rule.css_text += " { ";
    rule.css_text += inner;
    if (!inner.empty()) {
      rule.css_text += ' ';
    }
    rule.css_text += '}';
  } else {
    rule.css_text += ';';
  }
}

std::vector<CssomRule> ParseRuleList(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to);
CssomRule AtRule(const std::vector<Token>& tokens, std::size_t at_index, std::size_t prelude_end,
                 std::size_t block_start, std::size_t block_end, bool has_block);

void ParsePageBlock(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                    CssomRule& rule) {
  std::size_t at = from;
  std::string declarations;
  const auto flush = [&]() {
    if (declarations.empty()) {
      return;
    }
    const std::vector<Declaration> parsed = ParseDeclarationList(declarations);
    rule.declarations.insert(rule.declarations.end(), parsed.begin(), parsed.end());
    declarations.clear();
  };
  while (at < to && tokens[at].kind != Token::Kind::EndOfFile) {
    if (tokens[at].kind == Token::Kind::Whitespace) {
      ++at;
      continue;
    }
    if (tokens[at].kind == Token::Kind::AtKeyword) {
      flush();
      const std::size_t at_index = at;
      ++at;
      while (at < to && tokens[at].kind != Token::Kind::LeftBrace &&
             tokens[at].kind != Token::Kind::Semicolon &&
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
      rule.children.push_back(AtRule(tokens, at_index, prelude_end, block_start, block_end, true));
      continue;
    }
    const std::size_t start = at;
    while (at < to && tokens[at].kind != Token::Kind::AtKeyword &&
           tokens[at].kind != Token::Kind::EndOfFile) {
      ++at;
    }
    if (!declarations.empty()) {
      declarations += ' ';
    }
    declarations += ReconstructTokens(tokens, start, at);
  }
  flush();
}

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
    } else if (rule.type == CssomRuleType::Page) {
      ParsePageBlock(tokens, block_start, block_end, rule);
    } else if (rule.type == CssomRuleType::FontFace || rule.type == CssomRuleType::Margin) {
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
    rule.prelude = CanonicalSelectorText(ReconstructTokens(tokens, prelude_start, prelude_end));
    if (rule.prelude.empty()) {
      continue;
    }
    rule.declarations = ParseDeclarationList(ReconstructTokens(tokens, block_start, block_end));
    std::vector<Declaration> kept;
    kept.reserve(rule.declarations.size());
    for (Declaration& declaration : rule.declarations) {
      std::string canonical;
      switch (CanonicaliseDeclaration(declaration.property, declaration.value, &canonical)) {
        case DeclarationValidity::Invalid:
          continue;
        case DeclarationValidity::Canonical:
          declaration.value = std::move(canonical);
          break;
        case DeclarationValidity::Unknown:
          break;
      }
      kept.push_back(std::move(declaration));
    }
    rule.declarations = std::move(kept);
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

std::string SerializeSelectorList(const std::vector<Selector>& selectors) {
  return SerializeSelectorListInner(selectors);
}

std::string SerializeSelectorList(std::string_view text) {
  const std::vector<Selector> selectors = ParseSelectorList(text);
  return selectors.empty() ? std::string() : SerializeSelectorListInner(selectors);
}

std::string JoinCssomRules(const std::vector<CssomRule>& rules) {
  std::string out;
  for (const CssomRule& rule : rules) {
    if (!out.empty()) {
      out += '\n';
    }
    out += rule.css_text;
  }
  return out;
}

void RefreshCssomCssText(CssomRule& rule) {
  WriteCssomCssText(rule);
}

bool SetCssomSelectorText(CssomRule& rule, std::string_view selector) {
  if (rule.type != CssomRuleType::Style) {
    return false;
  }
  const std::string serialized = SerializeSelectorList(selector);
  if (serialized.empty()) {
    return false;
  }
  rule.prelude = serialized;
  WriteCssomCssText(rule);
  return true;
}

bool SetCssomPageSelectorText(CssomRule& rule, std::string_view selector) {
  if (rule.type != CssomRuleType::Page) {
    return false;
  }
  std::string_view text = util::TrimAscii(selector);
  std::string canonical;
  std::size_t i = 0;
  const auto is_ident_start = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
  };
  const auto is_ident = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
  };
  if (!text.empty() && text[0] != ':') {
    if (!is_ident_start(text[0])) {
      return false;
    }
    while (i < text.size() && is_ident(text[i])) {
      canonical.push_back(text[i]);
      ++i;
    }
  }
  while (i < text.size()) {
    if (text[i] != ':') {
      return false;
    }
    canonical.push_back(':');
    ++i;
    const std::size_t start = i;
    while (i < text.size() && is_ident(text[i])) {
      ++i;
    }
    if (start == i) {
      return false;
    }
    const std::string pseudo = util::AsciiLowerCase(text.substr(start, i - start));
    if (pseudo != "left" && pseudo != "right" && pseudo != "first" && pseudo != "blank") {
      return false;
    }
    canonical += pseudo;
  }
  rule.prelude = std::move(canonical);
  WriteCssomCssText(rule);
  return true;
}

}  // namespace microbrowser::css

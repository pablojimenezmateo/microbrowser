#include "css/StyleSheet.h"

#include <algorithm>
#include <utility>

#include "css/Tokenizer.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::css {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r' || text.front() == '\f')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                           text.back() == '\r' || text.back() == '\f')) {
    text.remove_suffix(1);
  }
  return text;
}

// Whitespace-separated words of an attribute, for `~=` and for class matching.
bool ContainsWord(std::string_view haystack, std::string_view word) {
  if (word.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start < haystack.size()) {
    while (start < haystack.size() && (haystack[start] == ' ' || haystack[start] == '\t' ||
                                       haystack[start] == '\n' || haystack[start] == '\f' ||
                                       haystack[start] == '\r')) {
      ++start;
    }
    std::size_t end = start;
    while (end < haystack.size() && haystack[end] != ' ' && haystack[end] != '\t' &&
           haystack[end] != '\n' && haystack[end] != '\f' && haystack[end] != '\r') {
      ++end;
    }
    if (end > start && haystack.substr(start, end - start) == word) {
      return true;
    }
    start = end;
  }
  return false;
}

// Turns a token run back into text, for a declaration's value. The value is
// kept as text at this layer: parsing `1px solid red` into a typed value needs
// the property's own grammar, which is the property database's job.
std::string Reconstruct(const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
  std::string out;
  for (std::size_t i = from; i < to && i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    switch (token.kind) {
      case Token::Kind::Whitespace:
        if (!out.empty()) {
          out.push_back(' ');
        }
        break;
      case Token::Kind::Ident:
      case Token::Kind::Url:
        out += token.value;
        break;
      case Token::Kind::Function:
        out += token.value;
        out.push_back('(');
        break;
      case Token::Kind::Hash:
        out.push_back('#');
        out += token.value;
        break;
      case Token::Kind::String:
        out.push_back('"');
        out += token.value;
        out.push_back('"');
        break;
      case Token::Kind::AtKeyword:
        out.push_back('@');
        out += token.value;
        break;
      case Token::Kind::Number:
      case Token::Kind::Percentage:
      case Token::Kind::Dimension: {
        if (token.is_integer) {
          out += std::to_string(static_cast<long long>(token.number));
        } else {
          std::string text = std::to_string(token.number);
          // Trailing zeros from to_string make `1.5` into `1.500000`.
          while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
          }
          if (!text.empty() && text.back() == '.') {
            text.pop_back();
          }
          out += text;
        }
        if (token.kind == Token::Kind::Percentage) {
          out.push_back('%');
        } else if (token.kind == Token::Kind::Dimension) {
          out += token.value;
        }
        break;
      }
      case Token::Kind::Delim:
        out += token.value;
        break;
      case Token::Kind::Colon:
        out.push_back(':');
        break;
      case Token::Kind::Comma:
        out.push_back(',');
        break;
      case Token::Kind::LeftParen:
        out.push_back('(');
        break;
      case Token::Kind::RightParen:
        out.push_back(')');
        break;
      case Token::Kind::LeftSquare:
        out.push_back('[');
        break;
      case Token::Kind::RightSquare:
        out.push_back(']');
        break;
      default:
        break;
    }
  }
  return std::string(Trim(out));
}

}  // namespace

Specificity Selector::ComputeSpecificity() const {
  Specificity result;
  for (const CompoundSelector& compound : compounds) {
    for (const SelectorPart& part : compound.parts) {
      switch (part.kind) {
        case SelectorPart::Kind::Id:
          ++result.ids;
          break;
        case SelectorPart::Kind::Class:
        case SelectorPart::Kind::Attribute:
        case SelectorPart::Kind::PseudoClass:
          ++result.classes;
          break;
        case SelectorPart::Kind::Type:
          ++result.types;
          break;
        case SelectorPart::Kind::Universal:
          break;  // the universal selector contributes nothing
      }
    }
  }
  return result;
}

namespace {

bool AttributeMatches(const SelectorPart& part, const dom::Element& element) {
  const std::string* value = element.GetAttribute(part.name);
  if (value == nullptr) {
    return false;
  }
  switch (part.match) {
    case SelectorPart::AttributeMatch::Exists:
      return true;
    case SelectorPart::AttributeMatch::Equals:
      return *value == part.value;
    case SelectorPart::AttributeMatch::Includes:
      return ContainsWord(*value, part.value);
    case SelectorPart::AttributeMatch::DashMatch:
      return *value == part.value ||
             (value->size() > part.value.size() &&
              value->compare(0, part.value.size(), part.value) == 0 &&
              (*value)[part.value.size()] == '-');
    case SelectorPart::AttributeMatch::Prefix:
      return !part.value.empty() && value->size() >= part.value.size() &&
             value->compare(0, part.value.size(), part.value) == 0;
    case SelectorPart::AttributeMatch::Suffix:
      return !part.value.empty() && value->size() >= part.value.size() &&
             value->compare(value->size() - part.value.size(), part.value.size(), part.value) == 0;
    case SelectorPart::AttributeMatch::Substring:
      return !part.value.empty() && value->find(part.value) != std::string::npos;
  }
  return false;
}

std::size_t IndexAmongElementSiblings(const dom::Element& element, bool& only_child) {
  const dom::Node* parent = element.Parent();
  if (parent == nullptr) {
    only_child = true;
    return 0;
  }
  std::size_t index = 0;
  std::size_t count = 0;
  for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
    if (!sibling->IsElement()) {
      continue;
    }
    if (sibling.get() == &element) {
      index = count;
    }
    ++count;
  }
  only_child = count == 1;
  return index;
}

bool MatchesCompound(const CompoundSelector& compound, const dom::Element& element) {
  for (const SelectorPart& part : compound.parts) {
    switch (part.kind) {
      case SelectorPart::Kind::Universal:
        break;
      case SelectorPart::Kind::Type:
        if (element.TagName() != part.name) {
          return false;
        }
        break;
      case SelectorPart::Kind::Class: {
        const std::string* classes = element.GetAttribute("class");
        if (classes == nullptr || !ContainsWord(*classes, part.name)) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Id: {
        const std::string* id = element.GetAttribute("id");
        if (id == nullptr || *id != part.name) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Attribute:
        if (!AttributeMatches(part, element)) {
          return false;
        }
        break;
      case SelectorPart::Kind::PseudoClass: {
        bool only_child = false;
        const std::size_t index = IndexAmongElementSiblings(element, only_child);
        if (part.name == "root") {
          if (element.Parent() == nullptr ||
              element.Parent()->GetKind() != dom::Node::Kind::Document) {
            return false;
          }
        } else if (part.name == "first-child") {
          if (index != 0) {
            return false;
          }
        } else if (part.name == "last-child") {
          const dom::Node* parent = element.Parent();
          if (parent == nullptr) {
            return false;
          }
          const dom::Node* last_element = nullptr;
          for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
            if (sibling->IsElement()) {
              last_element = sibling.get();
            }
          }
          if (last_element != &element) {
            return false;
          }
        } else if (part.name == "only-child") {
          if (!only_child) {
            return false;
          }
        } else if (part.name == "empty") {
          if (!element.Children().empty()) {
            return false;
          }
        } else {
          // A pseudo-class we do not implement must not match. Matching would
          // apply a rule the author scoped to a state we cannot observe.
          return false;
        }
        break;
      }
    }
  }
  return true;
}

// Matches right to left, which is what every engine does: starting from the
// element and walking up is bounded by the tree depth, where starting from the
// leftmost compound would search the whole subtree.
bool MatchesFrom(const std::vector<CompoundSelector>& compounds, std::size_t index,
                 const dom::Element& element) {
  if (!MatchesCompound(compounds[index], element)) {
    return false;
  }
  if (index == 0) {
    return true;
  }

  const CompoundSelector& current = compounds[index];
  const dom::Node* parent = element.Parent();
  switch (current.combinator) {
    case Combinator::None:
    case Combinator::Descendant: {
      for (const dom::Node* at = parent; at != nullptr; at = at->Parent()) {
        if (at->IsElement() &&
            MatchesFrom(compounds, index - 1, static_cast<const dom::Element&>(*at))) {
          return true;
        }
      }
      return false;
    }
    case Combinator::Child: {
      return parent != nullptr && parent->IsElement() &&
             MatchesFrom(compounds, index - 1, static_cast<const dom::Element&>(*parent));
    }
    case Combinator::NextSibling:
    case Combinator::LaterSibling: {
      if (parent == nullptr) {
        return false;
      }
      const dom::Element* previous = nullptr;
      for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
        if (sibling.get() == &element) {
          break;
        }
        if (sibling->IsElement()) {
          const dom::Element* candidate = static_cast<const dom::Element*>(sibling.get());
          if (current.combinator == Combinator::LaterSibling &&
              MatchesFrom(compounds, index - 1, *candidate)) {
            return true;
          }
          previous = candidate;
        }
      }
      if (current.combinator == Combinator::NextSibling) {
        return previous != nullptr && MatchesFrom(compounds, index - 1, *previous);
      }
      return false;
    }
  }
  return false;
}

}  // namespace

bool Selector::Matches(const dom::Element& element) const {
  if (compounds.empty()) {
    return false;
  }
  return MatchesFrom(compounds, compounds.size() - 1, element);
}

namespace {

// Parses a selector list out of a token run. Returns empty on anything it does
// not understand, so the whole rule is dropped rather than applied to the wrong
// elements.
std::vector<Selector> ParseSelectors(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to) {
  std::vector<Selector> selectors;
  Selector current;
  CompoundSelector compound;
  Combinator pending = Combinator::None;
  bool saw_part = false;
  bool failed = false;

  const auto finish_compound = [&] {
    if (saw_part) {
      compound.combinator = pending;
      current.compounds.push_back(compound);
      compound = CompoundSelector{};
      saw_part = false;
      pending = Combinator::None;
    }
  };
  const auto finish_selector = [&] {
    finish_compound();
    if (!current.compounds.empty()) {
      selectors.push_back(current);
    }
    current = Selector{};
  };

  for (std::size_t i = from; i < to && !failed; ++i) {
    const Token& token = tokens[i];
    switch (token.kind) {
      case Token::Kind::Whitespace: {
        // Whitespace is a descendant combinator only if a real compound
        // follows; trailing whitespace is not a combinator.
        std::size_t ahead = i + 1;
        while (ahead < to && tokens[ahead].kind == Token::Kind::Whitespace) {
          ++ahead;
        }
        if (ahead < to && tokens[ahead].kind != Token::Kind::Comma &&
            !(tokens[ahead].kind == Token::Kind::Delim &&
              (tokens[ahead].value == ">" || tokens[ahead].value == "+" ||
               tokens[ahead].value == "~"))) {
          finish_compound();
          // Only when no combinator is already pending. The whitespace *after*
          // a `>` is not a descendant combinator, and overwriting it here turns
          // `div > p` into `div p` — which matches strictly more elements, so
          // it fails open.
          if (pending == Combinator::None) {
            pending = Combinator::Descendant;
          }
        }
        break;
      }
      case Token::Kind::Comma:
        finish_selector();
        break;
      case Token::Kind::Ident: {
        SelectorPart part;
        part.kind = SelectorPart::Kind::Type;
        part.name = Lowered(token.value);
        compound.parts.push_back(part);
        saw_part = true;
        break;
      }
      case Token::Kind::Hash: {
        if (!token.hash_is_id) {
          failed = true;
          break;
        }
        SelectorPart part;
        part.kind = SelectorPart::Kind::Id;
        part.name = token.value;
        compound.parts.push_back(part);
        saw_part = true;
        break;
      }
      case Token::Kind::Delim: {
        if (token.value == "*") {
          SelectorPart part;
          part.kind = SelectorPart::Kind::Universal;
          compound.parts.push_back(part);
          saw_part = true;
        } else if (token.value == ".") {
          if (i + 1 >= to || tokens[i + 1].kind != Token::Kind::Ident) {
            failed = true;
            break;
          }
          SelectorPart part;
          part.kind = SelectorPart::Kind::Class;
          part.name = tokens[++i].value;
          compound.parts.push_back(part);
          saw_part = true;
        } else if (token.value == ">" || token.value == "+" || token.value == "~") {
          finish_compound();
          pending = token.value == ">"   ? Combinator::Child
                    : token.value == "+" ? Combinator::NextSibling
                                         : Combinator::LaterSibling;
        } else {
          failed = true;
        }
        break;
      }
      case Token::Kind::Colon: {
        // `::before` is a pseudo-*element*, which needs box generation rather
        // than matching. Unsupported means the rule is dropped.
        if (i + 1 < to && tokens[i + 1].kind == Token::Kind::Colon) {
          failed = true;
          break;
        }
        if (i + 1 >= to || tokens[i + 1].kind != Token::Kind::Ident) {
          failed = true;
          break;
        }
        SelectorPart part;
        part.kind = SelectorPart::Kind::PseudoClass;
        part.name = Lowered(tokens[++i].value);
        compound.parts.push_back(part);
        saw_part = true;
        break;
      }
      case Token::Kind::LeftSquare: {
        SelectorPart part;
        part.kind = SelectorPart::Kind::Attribute;
        std::size_t at = i + 1;
        while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
          ++at;
        }
        if (at >= to || tokens[at].kind != Token::Kind::Ident) {
          failed = true;
          break;
        }
        part.name = Lowered(tokens[at++].value);
        while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
          ++at;
        }
        if (at < to && tokens[at].kind == Token::Kind::RightSquare) {
          part.match = SelectorPart::AttributeMatch::Exists;
          i = at;
        } else if (at < to && tokens[at].kind == Token::Kind::Delim) {
          const std::string op = tokens[at].value;
          if (op == "=") {
            part.match = SelectorPart::AttributeMatch::Equals;
          } else if (op == "~") {
            part.match = SelectorPart::AttributeMatch::Includes;
          } else if (op == "|") {
            part.match = SelectorPart::AttributeMatch::DashMatch;
          } else if (op == "^") {
            part.match = SelectorPart::AttributeMatch::Prefix;
          } else if (op == "$") {
            part.match = SelectorPart::AttributeMatch::Suffix;
          } else if (op == "*") {
            part.match = SelectorPart::AttributeMatch::Substring;
          } else {
            failed = true;
            break;
          }
          ++at;
          if (part.match != SelectorPart::AttributeMatch::Equals) {
            if (at >= to || tokens[at].kind != Token::Kind::Delim || tokens[at].value != "=") {
              failed = true;
              break;
            }
            ++at;
          }
          while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
            ++at;
          }
          if (at >= to || (tokens[at].kind != Token::Kind::Ident &&
                           tokens[at].kind != Token::Kind::String)) {
            failed = true;
            break;
          }
          part.value = tokens[at++].value;
          while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
            ++at;
          }
          if (at >= to || tokens[at].kind != Token::Kind::RightSquare) {
            failed = true;
            break;
          }
          i = at;
        } else {
          failed = true;
          break;
        }
        compound.parts.push_back(part);
        saw_part = true;
        break;
      }
      default:
        failed = true;
        break;
    }
  }

  if (failed) {
    return {};
  }
  finish_selector();
  return selectors;
}

std::vector<Declaration> ParseDeclarations(const std::vector<Token>& tokens, std::size_t from,
                                           std::size_t to) {
  std::vector<Declaration> declarations;
  std::size_t at = from;
  while (at < to) {
    while (at < to && (tokens[at].kind == Token::Kind::Whitespace ||
                       tokens[at].kind == Token::Kind::Semicolon)) {
      ++at;
    }
    if (at >= to) {
      break;
    }
    if (tokens[at].kind != Token::Kind::Ident) {
      // Not a declaration. Skip to the next semicolon, which is the spec's
      // recovery: one bad declaration does not lose the rest of the block.
      while (at < to && tokens[at].kind != Token::Kind::Semicolon) {
        ++at;
      }
      continue;
    }

    Declaration declaration;
    declaration.property = Lowered(tokens[at++].value);
    while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
      ++at;
    }
    if (at >= to || tokens[at].kind != Token::Kind::Colon) {
      while (at < to && tokens[at].kind != Token::Kind::Semicolon) {
        ++at;
      }
      continue;
    }
    ++at;

    const std::size_t value_start = at;
    bool bad = false;
    while (at < to && tokens[at].kind != Token::Kind::Semicolon) {
      if (tokens[at].kind == Token::Kind::BadString || tokens[at].kind == Token::Kind::BadUrl) {
        bad = true;
      }
      ++at;
    }
    if (bad) {
      continue;  // a bad-string or bad-url invalidates its declaration
    }

    std::size_t value_end = at;
    // `!important`, which is a delim followed by the ident.
    for (std::size_t i = value_start; i + 1 < value_end; ++i) {
      if (tokens[i].kind == Token::Kind::Delim && tokens[i].value == "!") {
        std::size_t ahead = i + 1;
        while (ahead < value_end && tokens[ahead].kind == Token::Kind::Whitespace) {
          ++ahead;
        }
        if (ahead < value_end && tokens[ahead].kind == Token::Kind::Ident &&
            Lowered(tokens[ahead].value) == "important") {
          declaration.important = true;
          value_end = i;
          break;
        }
      }
    }

    declaration.value = Reconstruct(tokens, value_start, value_end);
    if (!declaration.value.empty()) {
      declarations.push_back(std::move(declaration));
    }
  }
  return declarations;
}

bool MediaListItemMatches(const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
  while (from < to && tokens[from].kind == Token::Kind::Whitespace) {
    ++from;
  }
  while (to > from && tokens[to - 1].kind == Token::Kind::Whitespace) {
    --to;
  }
  if (from + 1 != to || tokens[from].kind != Token::Kind::Ident) {
    return false;
  }
  const std::string media = Lowered(tokens[from].value);
  return media == "all" || media == "screen";
}

bool MediaPreludeMatches(const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
  std::size_t item_start = from;
  for (std::size_t at = from; at <= to; ++at) {
    if (at == to || tokens[at].kind == Token::Kind::Comma) {
      if (MediaListItemMatches(tokens, item_start, at)) {
        return true;
      }
      item_start = at + 1;
    }
  }
  return false;
}

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

void ParseRuleList(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                   StyleSheet& sheet) {
  std::size_t at = from;
  while (at < to && tokens[at].kind != Token::Kind::EndOfFile) {
    if (tokens[at].kind == Token::Kind::Whitespace || tokens[at].kind == Token::Kind::Cdo ||
        tokens[at].kind == Token::Kind::Cdc) {
      ++at;
      continue;
    }

    if (tokens[at].kind == Token::Kind::AtKeyword) {
      const std::string at_rule = Lowered(tokens[at].value);
      const std::size_t prelude_start = at + 1;
      ++at;
      while (at < to && tokens[at].kind != Token::Kind::LeftBrace &&
             tokens[at].kind != Token::Kind::Semicolon &&
             tokens[at].kind != Token::Kind::EndOfFile) {
        ++at;
      }
      if (at >= to || tokens[at].kind == Token::Kind::EndOfFile) {
        ++sheet.skipped;
        break;
      }
      if (tokens[at].kind == Token::Kind::Semicolon) {
        ++sheet.skipped;
        ++at;
        continue;
      }

      const std::size_t block_start = at + 1;
      const std::size_t block_end = FindBlockEnd(tokens, at, to);
      at = block_end;
      if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
        ++at;
      }

      if (at_rule == "media" && MediaPreludeMatches(tokens, prelude_start, block_start - 1)) {
        ParseRuleList(tokens, block_start, block_end, sheet);
      } else {
        ++sheet.skipped;
      }
      continue;
    }

    const std::size_t prelude_start = at;
    while (at < to && tokens[at].kind != Token::Kind::LeftBrace &&
           tokens[at].kind != Token::Kind::EndOfFile) {
      ++at;
    }
    if (at >= to || tokens[at].kind != Token::Kind::LeftBrace) {
      ++sheet.skipped;
      break;  // a rule with no block, at the end of the sheet
    }
    const std::size_t prelude_end = at;
    const std::size_t block_start = at + 1;
    const std::size_t block_end = FindBlockEnd(tokens, at, to);
    at = block_end;
    if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
      ++at;
    }

    StyleRule rule;
    rule.selectors = ParseSelectors(tokens, prelude_start, prelude_end);
    if (rule.selectors.empty()) {
      ++sheet.skipped;
      continue;
    }
    rule.declarations = ParseDeclarations(tokens, block_start, block_end);
    sheet.rules.push_back(std::move(rule));
    AddPerformanceCounter(PerfCounterId::CssRulesParsed);
  }
}

}  // namespace

std::vector<Declaration> ParseDeclarationList(std::string_view input) {
  const std::vector<Token> tokens = Tokenize(input);
  return ParseDeclarations(tokens, 0, tokens.size());
}

std::vector<Selector> ParseSelectorList(std::string_view input) {
  const std::vector<Token> tokens = Tokenize(input);
  std::size_t end = tokens.size();
  while (end > 0 && tokens[end - 1].kind == Token::Kind::EndOfFile) {
    --end;
  }
  return ParseSelectors(tokens, 0, end);
}

StyleSheet ParseStyleSheet(std::string_view input) {
  StyleSheet sheet;
  const std::vector<Token> tokens = Tokenize(input);
  AddPerformanceCounter(PerfCounterId::CssSheetsParsed);
  ParseRuleList(tokens, 0, tokens.size(), sheet);
  return sheet;
}

}  // namespace microbrowser::css

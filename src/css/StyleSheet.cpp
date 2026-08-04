#include "css/StyleSheet.h"

#include <utility>

#include "css/CssText.h"
#include "css/Selectors.h"
#include "css/Tokenizer.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::css {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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
        out += token.value;
        break;
      case Token::Kind::Url:
        // An unquoted `url(x.png)` scans as one token holding just the target,
        // and it has to be written back out as a url or it reconstructs to a
        // bare `x.png` -- which reads as an identifier, not a resource. The
        // quoted form is a Function token plus a String and round-trips on its
        // own, so before this the two spellings of the same value produced
        // different declarations.
        out += "url(\"";
        out += token.value;
        out += "\")";
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

StyleSheet ParseStyleSheet(std::string_view input) {
  StyleSheet sheet;
  const std::vector<Token> tokens = Tokenize(input);
  AddPerformanceCounter(PerfCounterId::CssSheetsParsed);
  ParseRuleList(tokens, 0, tokens.size(), sheet);
  return sheet;
}

}  // namespace microbrowser::css

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace microbrowser::js {

// One token of ECMAScript source.
//
// Two fields here are not decoration and cannot be recovered later, which is
// why they are on every token rather than computed by whoever needs them:
//
//   * `newline_before` — automatic semicolon insertion is defined in terms of
//     whether a line terminator preceded a token. Whitespace is discarded by
//     the lexer, so if this is not recorded here it is gone, and ASI becomes a
//     guess. It is the single most common reason a hand-written JS parser is
//     subtly wrong.
//   * `start`/`end` — every error a user sees points at source, and a parser
//     that reconstructs positions from a token stream gets them wrong the
//     first time a string contains a newline.
enum class TokenType : std::uint8_t {
  EndOfFile,
  Invalid,

  Identifier,
  PrivateIdentifier,  // #x, in class bodies
  Keyword,
  NumericLiteral,
  StringLiteral,
  TemplateString,  // the whole literal, including its substitutions, unparsed
  RegExpLiteral,
  Punctuator,
};

struct Token {
  TokenType type = TokenType::EndOfFile;
  // The exact source text, un-unescaped. The cooked value of a string is
  // computed by the parser, because a raw and a cooked form are both needed
  // (template literals expose both) and recomputing the raw one is impossible.
  std::string_view lexeme;
  // For a string or numeric literal, the value it denotes. Empty otherwise.
  std::string value;
  double number = 0.0;

  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t line = 1;
  bool newline_before = false;

  bool Is(TokenType kind) const { return type == kind; }
  bool IsPunctuator(std::string_view text) const {
    return type == TokenType::Punctuator && lexeme == text;
  }
  bool IsKeyword(std::string_view text) const {
    return type == TokenType::Keyword && lexeme == text;
  }
};

// True for the reserved words. Contextual keywords -- `of`, `async`, `get`,
// `set`, `static` -- are deliberately absent: they are identifiers except in
// positions the parser knows about, and lexing them as keywords would break
// `var get = 1`.
bool IsReservedWord(std::string_view text);

}  // namespace microbrowser::js

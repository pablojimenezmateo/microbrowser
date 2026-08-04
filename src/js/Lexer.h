#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "js/Token.h"

namespace microbrowser::js {

// Turns ECMAScript source into tokens.
//
// One token at a time rather than a vector up front, for a reason that is
// specific to this language: `/` is division or the start of a regular
// expression depending on what the *parser* expects, and no amount of
// lookahead settles it — `a /b/ g` is three divisions or one regex depending
// on whether `a` is a value or a keyword. So the lexer cannot run ahead of the
// parser, and the parser asks for a regex when the grammar allows one.
//
// The other input-driven hazard is that source is attacker-controlled: a page
// serves it. Every read is bounds-checked against the end of the input, and an
// unterminated literal is a token of type Invalid rather than a read past the
// buffer or a loop that never ends.
class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  // The next token. At the end of input it returns EndOfFile forever, which is
  // what lets a parser's error paths stop asking without a separate check.
  Token Next();

  // Re-reads the token that was just returned as a regular expression literal.
  // Called by the parser when it hit a `/` in a position where a value was
  // expected. Returns an Invalid token if the source there is not a regex.
  //
  // This exists because the alternative -- a lexer that guesses from the
  // previous token -- is a heuristic that is wrong for `}` (block end versus
  // object literal end) and for a handful of keywords, and being wrong means
  // silently lexing a program as something else.
  Token RescanAsRegExp(const Token& slash);

  std::size_t Offset() const { return offset_; }
  // Rewinds to a previously recorded offset. Used for the lookahead a few
  // productions need (arrow functions, mostly), where the decision cannot be
  // made without reading ahead.
  void SeekTo(std::size_t offset, std::size_t line);
  std::size_t Line() const { return line_; }

  bool AtEnd() const { return offset_ >= source_.size(); }

 private:
  char Peek(std::size_t ahead = 0) const {
    return offset_ + ahead < source_.size() ? source_[offset_ + ahead] : '\0';
  }
  bool SkipWhitespaceAndComments();  // returns whether a line terminator was crossed

  Token MakeToken(TokenType type, std::size_t start, bool newline) const;
  Token LexIdentifierOrKeyword(std::size_t start, bool newline);
  // Rewrites `token.lexeme` to the name its `\uXXXX` escapes denote. False
  // when one of them is malformed, which makes the token Invalid rather than a
  // name nobody wrote.
  bool DecodeIdentifier(Token& token);
  Token LexNumber(std::size_t start, bool newline);
  Token LexString(std::size_t start, bool newline);
  Token LexTemplate(std::size_t start, bool newline);
  Token LexPunctuator(std::size_t start, bool newline);

  std::string_view source_;
  std::size_t offset_ = 0;
  std::size_t line_ = 1;
  // Decoded names for identifiers written with `\uXXXX` escapes, which are the
  // same identifier as the characters they denote -- `ɵprov` and `ɵprov`
  // must resolve to one binding.
  //
  // Owned here, and a deque because the addresses have to stay put: a token's
  // `lexeme` is a view, and pointing it at the decoded name is what keeps the
  // escape invisible to the parser rather than adding a second spelling to
  // twenty-five identifier sites. The lexer outlives every token it made.
  std::deque<std::string> decoded_names_;
};

// Every token in `source`, for a test or a tool. Stops at the first EndOfFile.
// Uses division rather than regex at every ambiguity, so it is not a substitute
// for parsing -- see the note on Lexer.
std::vector<Token> TokenizeAll(std::string_view source);

}  // namespace microbrowser::js

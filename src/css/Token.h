#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microbrowser::css {

// CSS tokens, per CSS Syntax Level 3 §4. Named as the specification names them.
//
// Same reasoning as the HTML tokenizer: CSS error handling is normative. A
// stylesheet with a syntax error is not rejected, it is *recovered from* by
// rules the spec spells out, and every browser recovers identically. A parser
// that is merely reasonable drops different declarations from every other
// engine, which shows up as a page that looks broken in this browser alone.
struct Token {
  enum class Kind : std::uint8_t {
    Ident,
    Function,     // ident immediately followed by (
    AtKeyword,
    Hash,
    String,
    BadString,
    Url,
    BadUrl,
    Delim,
    Number,
    Percentage,
    Dimension,
    Whitespace,
    Cdo,          // <!--
    Cdc,          // -->
    Colon,
    Semicolon,
    Comma,
    LeftSquare,
    RightSquare,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    EndOfFile,
  };

  Kind kind = Kind::EndOfFile;
  // Ident, function name, at-keyword, hash, string, url, dimension unit, or the
  // single character of a delim.
  std::string value;
  double number = 0.0;
  bool is_integer = false;
  // Hash tokens carry whether they could be an id selector. `#1x` is a valid
  // hash and not a valid id, and the difference decides whether a selector
  // parses.
  bool hash_is_id = false;

  friend bool operator==(const Token&, const Token&) = default;
};

}  // namespace microbrowser::css

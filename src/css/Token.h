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
    UnicodeRange,  // U+0-7F, U+4??, U+0100-024F
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
  // Whether a numeric token was written with an explicit `+` or `-`. The value
  // alone cannot say: `+3` and `3` are the same number. The An+B microsyntax of
  // `:nth-child()` is the one grammar that cares — `2n +3` is a valid selector
  // and `2n 3` is not — and reading the difference back out of the number is
  // impossible once the sign is gone.
  bool has_sign = false;
  // A unicode-range's two ends, inclusive. Scanned in the tokenizer rather than
  // assembled by the consumer because the generic tokens *destroy* the value:
  // `U+0100-02BA` scans as Ident("U"), Number(+100), Dimension(-2, "BA"), and
  // neither the leading zeros nor the hex reading survive that. CSS Syntax 3 asks
  // the consumer to work from each token's original text, which is the same
  // information by a longer route -- this is the short one.
  std::uint32_t range_start = 0;
  std::uint32_t range_end = 0;
  // Hash tokens carry whether they could be an id selector. `#1x` is a valid
  // hash and not a valid id, and the difference decides whether a selector
  // parses.
  bool hash_is_id = false;
  // Which quote a string was written with (`'` or `"`). Custom-property
  // serialization has to round-trip the two; ReconstructTokens used to emit
  // double quotes for every string and the consecutive-token tests noticed.
  char quote = '\0';

  friend bool operator==(const Token&, const Token&) = default;
};

}  // namespace microbrowser::css

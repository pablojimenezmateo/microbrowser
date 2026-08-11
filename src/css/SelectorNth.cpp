#include "css/Selectors.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/CssText.h"
#include "css/StyleSheet.h"
#include "css/Token.h"

// The `An+B` microsyntax of CSS Syntax Level 3 §9, on its own because it is a
// grammar rather than a piece of the selector grammar: it has its own token
// shapes (`2n-3` is *one* dimension token whose unit carries the B), its own
// sign rules, and it is reachable from four function names that otherwise share
// nothing. Splitting it out of `SelectorParse.cpp` is what kept that file under
// its line cap when `An+B of S` landed, and the seam is the right one -- nothing
// here needs to know what a compound selector is.

namespace microbrowser::css {

namespace {

// What may follow the `n` of an An+B, once the leading identifier or dimension
// unit has been read.
enum class NthTail : std::uint8_t {
  None,             // `2n-3` — the B was inside the unit, nothing else may follow
  OptionalSigned,   // `2n` — an explicitly signed integer may follow, or nothing
  RequiredSignless, // `2n-` — a signless integer must follow, and is negative
};

bool IntegerValue(const Token& token, std::int32_t& out) {
  if (!token.is_integer) {
    return false;
  }
  // Rejected rather than clamped: a clamped An+B is a selector that matches a
  // different set of elements than the stylesheet asked for, which is exactly
  // the silent wrongness this module refuses elsewhere.
  const double value = token.number;
  if (!(value >= -2147483648.0 && value <= 2147483647.0)) {
    return false;
  }
  out = static_cast<std::int32_t>(value);
  return true;
}

bool ParseDigits(std::string_view text, std::int32_t& out) {
  if (text.empty()) {
    return false;
  }
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
    if (value > 2147483647LL) {
      return false;
    }
  }
  out = static_cast<std::int32_t>(value);
  return true;
}

// The tail of an An+B identifier or dimension unit, from its `n` onwards.
// `n-3` and `2n-3` arrive as *one* identifier and one dimension unit, because a
// CSS name may contain both a hyphen and a digit — splitting them back apart is
// the part of this grammar every implementation writes by hand.
bool ParseNSuffix(std::string_view text, std::int32_t& b, NthTail& tail) {
  if (text.empty() || text.front() != 'n') {
    return false;
  }
  text.remove_prefix(1);
  if (text.empty()) {
    tail = NthTail::OptionalSigned;
    return true;
  }
  if (text.front() != '-') {
    return false;
  }
  text.remove_prefix(1);
  if (text.empty()) {
    tail = NthTail::RequiredSignless;
    return true;
  }
  std::int32_t digits = 0;
  if (!ParseDigits(text, digits)) {
    return false;
  }
  b = -digits;
  tail = NthTail::None;
  return true;
}

// An identifier standing where an An+B may begin: `n`, `-n`, `n-`, `-n-`,
// `n-3`, `-n-3`.
bool ParseNIdent(std::string_view ident, std::int32_t& a, std::int32_t& b, NthTail& tail) {
  if (!ident.empty() && ident.front() == '-') {
    a = -1;
    ident.remove_prefix(1);
  } else {
    a = 1;
  }
  return ParseNSuffix(ident, b, tail);
}

}  // namespace

// The An+B microsyntax of CSS Syntax Level 3 §9, over the tokens between a
// `:nth-child(` and its `)`. Sets only `a` and `b`; which sequence they count
// over was decided by the function name. The `of S` half is split off by the
// caller before this runs, so anything left over here is a syntax error and
// drops the rule rather than silently counting over the wrong siblings.
bool ParseAnPlusB(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                  NthPattern& out) {
  const auto skip_whitespace = [&](std::size_t at) {
    while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
      ++at;
    }
    return at;
  };
  while (to > from && tokens[to - 1].kind == Token::Kind::Whitespace) {
    --to;
  }
  std::size_t at = skip_whitespace(from);
  if (at >= to) {
    return false;
  }

  std::int32_t a = 0;
  std::int32_t b = 0;
  NthTail tail = NthTail::None;

  const Token& first = tokens[at];
  if (first.kind == Token::Kind::Ident) {
    const std::string ident = Lowered(first.value);
    ++at;
    if (ident == "odd") {
      a = 2;
      b = 1;
    } else if (ident == "even") {
      a = 2;
      b = 0;
    } else if (!ParseNIdent(ident, a, b, tail)) {
      return false;
    }
  } else if (first.kind == Token::Kind::Delim && first.value == "+") {
    // `+n`, and the `+` must touch the identifier: whitespace is its own token,
    // so `+ n` fails here exactly as the specification says it should.
    ++at;
    if (at >= to || tokens[at].kind != Token::Kind::Ident) {
      return false;
    }
    const std::string ident = Lowered(tokens[at].value);
    ++at;
    if (ident.empty() || ident.front() == '-' || !ParseNIdent(ident, a, b, tail)) {
      return false;
    }
  } else if (first.kind == Token::Kind::Dimension) {
    if (!IntegerValue(first, a)) {
      return false;
    }
    ++at;
    if (!ParseNSuffix(Lowered(first.value), b, tail)) {
      return false;
    }
  } else if (first.kind == Token::Kind::Number) {
    if (!IntegerValue(first, b)) {
      return false;
    }
    a = 0;
    ++at;
  } else {
    return false;
  }

  at = skip_whitespace(at);
  switch (tail) {
    case NthTail::None:
      break;
    case NthTail::RequiredSignless: {
      std::int32_t value = 0;
      if (at >= to || tokens[at].kind != Token::Kind::Number || tokens[at].has_sign ||
          !IntegerValue(tokens[at], value)) {
        return false;
      }
      b = -value;
      at = skip_whitespace(at + 1);
      break;
    }
    case NthTail::OptionalSigned: {
      if (at >= to) {
        break;
      }
      if (tokens[at].kind == Token::Kind::Number) {
        // `2n 3` is not a selector and `2n +3` is; only the sign says which.
        if (!tokens[at].has_sign || !IntegerValue(tokens[at], b)) {
          return false;
        }
        at = skip_whitespace(at + 1);
        break;
      }
      if (tokens[at].kind != Token::Kind::Delim ||
          (tokens[at].value != "+" && tokens[at].value != "-")) {
        return false;
      }
      const std::int32_t sign = tokens[at].value == "-" ? -1 : 1;
      at = skip_whitespace(at + 1);
      std::int32_t value = 0;
      if (at >= to || tokens[at].kind != Token::Kind::Number || tokens[at].has_sign ||
          !IntegerValue(tokens[at], value)) {
        return false;
      }
      b = sign * value;
      at = skip_whitespace(at + 1);
      break;
    }
  }
  if (at != to) {
    return false;
  }

  out.a = a;
  out.b = b;
  return true;
}


}  // namespace microbrowser::css

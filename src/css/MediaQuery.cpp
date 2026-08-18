#include "css/MediaQuery.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/Token.h"
#include "css/Tokenizer.h"
#include "util/StringUtil.h"

namespace microbrowser::css {
namespace {

// A condition may nest -- `not ((min-width: 10px) and (max-width: 20px))` --
// and the nesting is attacker-chosen, so it is bounded here rather than by the
// stack. The same number the selector parser uses for the same reason: a page
// that needs more than sixteen levels of parentheses in one media query does
// not exist, and a page that writes ten thousand of them does.
constexpr int kMaxDepth = 16;

// A cursor over the token vector. Every function below either advances it or
// fails, which is what keeps a malformed prelude from looping.
struct Cursor {
  const std::vector<Token>& tokens;
  std::size_t at = 0;

  bool AtEnd() const {
    return at >= tokens.size() || tokens[at].kind == Token::Kind::EndOfFile;
  }
  const Token& Peek() const { return tokens[at]; }
  void SkipWhitespace() {
    while (!AtEnd() && tokens[at].kind == Token::Kind::Whitespace) {
      ++at;
    }
  }
};

bool HasPrefixCaseInsensitive(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         util::EqualsAsciiCaseInsensitive(text.substr(0, prefix.size()), prefix);
}

bool IsIdent(const Token& token, std::string_view name) {
  return token.kind == Token::Kind::Ident &&
         util::EqualsAsciiCaseInsensitive(token.value, name);
}

// A media *type*. `all` and `screen` are what this browser is; `print` and the
// rest are what it is not. An unknown type is false rather than true, so a
// query naming a medium nobody has heard of does not apply.
bool MediaTypeMatches(std::string_view name) {
  return util::EqualsAsciiCaseInsensitive(name, "all") ||
         util::EqualsAsciiCaseInsensitive(name, "screen");
}

// CSS pixels for one dimension token. The unit set is deliberately the same one
// css::ParseLength accepts, plus the viewport units -- two length parsers that
// accept different units is two answers to "is this a length", and the whole
// reason this one exists separately is that a viewport unit resolves here and
// cannot survive into the cascade.
// CSS pixels for one dimension token, into `out`. The unit set is deliberately
// the same one css::ParseLength accepts, plus the viewport units -- two length
// parsers that accept different units is two answers to "is this a length", and
// the whole reason this one exists separately is that a viewport unit resolves
// here and cannot survive into the cascade.
//
// An out-parameter and a bool rather than an optional: this is inlined into
// every feature comparison, and GCC cannot see through an optional's engaged
// flag across that inlining -- it warns that the value may be used
// uninitialized, at -O2 only, which is a diagnostic nobody should have to
// re-derive from an asan log.
bool LengthFromToken(const Token& token, const MediaContext& context, float& out) {
  if (token.kind == Token::Kind::Number) {
    // A unitless number is a length only when it is zero, exactly as in a
    // declaration. `(min-width: 600)` is not 600 pixels; it is invalid.
    out = 0.0f;
    return token.number == 0.0;
  }
  if (token.kind != Token::Kind::Dimension) {
    return false;
  }
  const std::string& unit = token.value;
  // Pixels per unit. Everything below is one multiplication, which is what
  // makes the viewport units no more special than `pt`.
  double per_unit = 0.0;
  if (const std::optional<double> scale = AbsoluteUnitScale(unit)) {
    per_unit = *scale;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "em") ||
             util::EqualsAsciiCaseInsensitive(unit, "rem")) {
    // In a media query there is no element, so `em` is the initial font size --
    // which is what `rem` is everywhere else, and why the two are one branch.
    per_unit = static_cast<double>(kRootFontSize);
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vw")) {
    per_unit = static_cast<double>(context.viewport_width) / 100.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vh")) {
    per_unit = static_cast<double>(context.viewport_height) / 100.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vmin")) {
    per_unit = static_cast<double>(std::min(context.viewport_width, context.viewport_height)) / 100.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vmax")) {
    per_unit = static_cast<double>(std::max(context.viewport_width, context.viewport_height)) / 100.0;
  } else {
    return false;
  }
  out = static_cast<float>(token.number * per_unit);
  return true;
}

// Dots per CSS pixel for one token. `dppx` and `x` are the same unit; `dpi` and
// `dpcm` are the print spellings of it.
bool ResolutionFromToken(const Token& token, float& out) {
  if (token.kind != Token::Kind::Dimension) {
    return false;
  }
  const std::string& unit = token.value;
  double per_unit = 0.0;
  if (util::EqualsAsciiCaseInsensitive(unit, "dppx") ||
      util::EqualsAsciiCaseInsensitive(unit, "x")) {
    per_unit = 1.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "dpi")) {
    per_unit = 1.0 / 96.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "dpcm")) {
    per_unit = 2.54 / 96.0;
  } else {
    return false;
  }
  out = static_cast<float>(token.number * per_unit);
  return true;
}

enum class Compare { Exact, Min, Max };

bool CompareValues(Compare compare, double actual, double bound) {
  switch (compare) {
    case Compare::Min:
      return actual >= bound;
    case Compare::Max:
      return actual <= bound;
    case Compare::Exact:
      break;
  }
  return std::abs(actual - bound) < 0.001;
}

// `(feature)` or `(feature: value)`, with the cursor just past the `(` and
// `end` the index of its `)`.
//
// A feature this browser does not implement is false, per the spec's rule for an
// unknown feature. That rule is doing real work here: every feature added is one
// more true fact about the user's machine that any page may ask for, so the
// short list below is a privacy decision as much as a compatibility one.
bool FeatureMatches(const std::vector<Token>& tokens, std::size_t at, std::size_t end,
                    const MediaContext& context) {
  Cursor cursor{tokens, at};
  cursor.SkipWhitespace();
  if (cursor.at >= end || cursor.Peek().kind != Token::Kind::Ident) {
    return false;
  }
  const std::string name = cursor.Peek().value;
  ++cursor.at;
  cursor.SkipWhitespace();

  Compare compare = Compare::Exact;
  if (HasPrefixCaseInsensitive(name, "min-")) {
    compare = Compare::Min;
  } else if (HasPrefixCaseInsensitive(name, "max-")) {
    compare = Compare::Max;
  }
  std::string_view feature{name};
  if (compare != Compare::Exact) {
    feature.remove_prefix(4);
  }

  // The boolean form: `(width)` asks whether the feature is non-zero. A
  // min-/max- prefix has no boolean form.
  if (cursor.at >= end) {
    if (compare != Compare::Exact) {
      return false;
    }
    if (util::EqualsAsciiCaseInsensitive(feature, "width")) {
      return context.viewport_width != 0.0f;
    }
    if (util::EqualsAsciiCaseInsensitive(feature, "height")) {
      return context.viewport_height != 0.0f;
    }
    if (util::EqualsAsciiCaseInsensitive(feature, "resolution")) {
      return context.device_pixel_ratio != 0.0f;
    }
    if (util::EqualsAsciiCaseInsensitive(feature, "orientation")) {
      return true;
    }
    // The boolean form of the two `prefers-*` features. `@media (prefers-reduced-motion)` with no value
    // is the common spelling and means `reduce`, which is why it is answered here rather than only in
    // the value form below.
    if (util::EqualsAsciiCaseInsensitive(feature, "prefers-reduced-motion")) {
      return context.prefers_reduced_motion;
    }
    if (util::EqualsAsciiCaseInsensitive(feature, "prefers-color-scheme")) {
      // `(prefers-color-scheme)` with no value is *always true* per the specification -- the feature is
      // always available, and a page asking whether it exists gets yes. Not the dark bit: a page that
      // wrote this and got the preference would style light users darkly.
      return true;
    }
    return false;
  }

  if (cursor.Peek().kind != Token::Kind::Colon) {
    return false;
  }
  ++cursor.at;
  cursor.SkipWhitespace();
  if (cursor.at >= end) {
    return false;
  }
  const Token& value = cursor.Peek();
  ++cursor.at;
  cursor.SkipWhitespace();
  if (cursor.at != end) {
    return false;  // trailing tokens: not a value this grammar accepts
  }

  float bound = 0.0f;
  if (util::EqualsAsciiCaseInsensitive(feature, "width") ||
      util::EqualsAsciiCaseInsensitive(feature, "height")) {
    if (!LengthFromToken(value, context, bound)) {
      return false;
    }
    const float actual = util::EqualsAsciiCaseInsensitive(feature, "width")
                             ? context.viewport_width
                             : context.viewport_height;
    return CompareValues(compare, static_cast<double>(actual), static_cast<double>(bound));
  }
  if (util::EqualsAsciiCaseInsensitive(feature, "resolution")) {
    return ResolutionFromToken(value, bound) &&
           CompareValues(compare, static_cast<double>(context.device_pixel_ratio),
                         static_cast<double>(bound));
  }
  // **ADR 0029 §6's two deliberate exceptions to the constant rule.** Each is one bit, and each changes
  // whether a page is usable (reduced motion) or comfortable (dark). Paying a bit for that is a better
  // trade than paying one for `deviceMemory`, and the ADR says so in as many words.
  if (util::EqualsAsciiCaseInsensitive(feature, "prefers-color-scheme") &&
      compare == Compare::Exact) {
    if (IsIdent(value, "dark")) {
      return context.prefers_dark;
    }
    if (IsIdent(value, "light")) {
      // `light` is the answer for a user with no preference *and* for one who chose light, which is what
      // the specification says: `no-preference` was removed from the feature precisely because a third
      // state was a third bit.
      return !context.prefers_dark;
    }
    return false;
  }
  if (util::EqualsAsciiCaseInsensitive(feature, "prefers-reduced-motion") &&
      compare == Compare::Exact) {
    if (IsIdent(value, "reduce")) {
      return context.prefers_reduced_motion;
    }
    if (IsIdent(value, "no-preference")) {
      return !context.prefers_reduced_motion;
    }
    return false;
  }
  if (util::EqualsAsciiCaseInsensitive(feature, "orientation") && compare == Compare::Exact) {
    if (IsIdent(value, "portrait")) {
      return context.viewport_height >= context.viewport_width;
    }
    if (IsIdent(value, "landscape")) {
      return context.viewport_width > context.viewport_height;
    }
  }
  return false;
}

// The index of the `)` closing the `(` at `open`, or npos. Parenthesis depth
// only: a media query has no braces or brackets to balance.
std::size_t FindClose(const std::vector<Token>& tokens, std::size_t open) {
  int depth = 0;
  for (std::size_t at = open; at < tokens.size(); ++at) {
    if (tokens[at].kind == Token::Kind::EndOfFile) {
      return std::string::npos;
    }
    if (tokens[at].kind == Token::Kind::LeftParen ||
        tokens[at].kind == Token::Kind::Function) {
      ++depth;
    } else if (tokens[at].kind == Token::Kind::RightParen) {
      if (--depth == 0) {
        return at;
      }
    }
  }
  return std::string::npos;
}

bool ConditionMatches(Cursor& cursor, std::size_t end, const MediaContext& context, int depth,
                      bool& ok);

// `( condition )` or `( feature )`. Whichever it is decides by what is inside:
// a nested condition begins with `(` or with `not`.
bool InParensMatches(Cursor& cursor, const MediaContext& context, int depth, bool& ok) {
  cursor.SkipWhitespace();
  if (cursor.AtEnd() || cursor.Peek().kind != Token::Kind::LeftParen) {
    ok = false;
    return false;
  }
  const std::size_t close = FindClose(cursor.tokens, cursor.at);
  if (close == std::string::npos) {
    ok = false;
    return false;
  }
  Cursor inner{cursor.tokens, cursor.at + 1};
  inner.SkipWhitespace();
  const bool nested = !inner.AtEnd() && inner.at < close &&
                      (inner.Peek().kind == Token::Kind::LeftParen || IsIdent(inner.Peek(), "not"));
  bool result = false;
  if (nested) {
    result = ConditionMatches(inner, close, context, depth + 1, ok);
    inner.SkipWhitespace();
    if (inner.at != close) {
      ok = false;
    }
  } else {
    result = FeatureMatches(cursor.tokens, cursor.at + 1, close, context);
  }
  cursor.at = close + 1;
  return result;
}

// `not <in-parens>` | `<in-parens> [and <in-parens>]*` | `<in-parens> [or <in-parens>]*`.
// Mixing `and` with `or` at one level is a syntax error, not a precedence
// question, which is why the operator is latched on first sight.
bool ConditionMatches(Cursor& cursor, std::size_t end, const MediaContext& context, int depth,
                      bool& ok) {
  if (depth > kMaxDepth) {
    ok = false;
    return false;
  }
  cursor.SkipWhitespace();
  if (cursor.AtEnd() || cursor.at >= end) {
    ok = false;
    return false;
  }
  if (IsIdent(cursor.Peek(), "not")) {
    ++cursor.at;
    return !InParensMatches(cursor, context, depth, ok);
  }
  bool result = InParensMatches(cursor, context, depth, ok);
  std::string_view latched;
  while (ok) {
    cursor.SkipWhitespace();
    if (cursor.AtEnd() || cursor.at >= end) {
      break;
    }
    const bool is_and = IsIdent(cursor.Peek(), "and");
    const bool is_or = IsIdent(cursor.Peek(), "or");
    if (!is_and && !is_or) {
      break;
    }
    const std::string_view op = is_and ? "and" : "or";
    if (latched.empty()) {
      latched = op;
    } else if (latched != op) {
      ok = false;
      break;
    }
    ++cursor.at;
    const bool right = InParensMatches(cursor, context, depth, ok);
    result = is_and ? (result && right) : (result || right);
  }
  return result;
}

// One `<media-query>`: a condition on its own, or a media type with conditions
// `and`ed onto it.
bool QueryMatches(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                  const MediaContext& context) {
  Cursor cursor{tokens, from};
  cursor.SkipWhitespace();
  if (cursor.at >= to) {
    // An empty query in a list -- `screen,,print` -- is not "matches
    // everything", it is one malformed entry among several.
    return false;
  }

  bool negated = false;
  if (IsIdent(cursor.Peek(), "not")) {
    // `not (min-width: 10px)` negates a condition; `not screen` negates a type.
    // Which one it is decides by what follows, and nothing else.
    Cursor lookahead{tokens, cursor.at + 1};
    lookahead.SkipWhitespace();
    if (!lookahead.AtEnd() && lookahead.Peek().kind == Token::Kind::LeftParen) {
      bool ok = true;
      const bool result = ConditionMatches(cursor, to, context, 0, ok);
      cursor.SkipWhitespace();
      return ok && cursor.at >= to && result;
    }
    negated = true;
    cursor.at = lookahead.at;
  } else if (IsIdent(cursor.Peek(), "only")) {
    // `only` exists to hide a query from a CSS2 user agent. To one that
    // understands media queries it means nothing at all.
    ++cursor.at;
    cursor.SkipWhitespace();
  }

  if (cursor.AtEnd() || cursor.at >= to) {
    return false;
  }
  if (cursor.Peek().kind != Token::Kind::Ident) {
    if (negated) {
      return false;  // `not` followed by neither a type nor a condition
    }
    bool ok = true;
    const bool result = ConditionMatches(cursor, to, context, 0, ok);
    cursor.SkipWhitespace();
    return ok && cursor.at >= to && result;
  }

  bool result = MediaTypeMatches(cursor.Peek().value);
  ++cursor.at;
  cursor.SkipWhitespace();
  while (cursor.at < to) {
    if (!IsIdent(cursor.Peek(), "and")) {
      return false;  // a type followed by something that is not `and`
    }
    ++cursor.at;
    bool ok = true;
    const bool right = InParensMatches(cursor, context, 0, ok);
    if (!ok) {
      return false;
    }
    result = result && right;
    cursor.SkipWhitespace();
  }
  return negated ? !result : result;
}

}  // namespace

bool MediaQueryListMatches(std::string_view text, const MediaContext& context) {
  if (util::TrimAscii(text).empty()) {
    // An empty media query list matches every medium. `<source media="">` and
    // a `sizes` entry with no condition in front of it both arrive here.
    return true;
  }
  const std::vector<Token> tokens = Tokenize(text);
  std::size_t end = 0;
  while (end < tokens.size() && tokens[end].kind != Token::Kind::EndOfFile) {
    ++end;
  }
  // A list matches when any query in it does, which is why a malformed entry is
  // dropped rather than poisoning the list. Commas at this level are the only
  // separator: one inside parentheses belongs to whatever is in them.
  std::size_t item_start = 0;
  int depth = 0;
  for (std::size_t at = 0; at <= end; ++at) {
    if (at != end) {
      if (tokens[at].kind == Token::Kind::LeftParen ||
          tokens[at].kind == Token::Kind::Function) {
        ++depth;
      } else if (tokens[at].kind == Token::Kind::RightParen) {
        depth = std::max(0, depth - 1);
      }
      if (depth > 0 || tokens[at].kind != Token::Kind::Comma) {
        continue;
      }
    }
    if (QueryMatches(tokens, item_start, at, context)) {
      return true;
    }
    item_start = at + 1;
  }
  return false;
}

std::optional<double> AbsoluteUnitScale(std::string_view unit) {
  if (util::EqualsAsciiCaseInsensitive(unit, "px")) {
    return 1.0;
  }
  if (util::EqualsAsciiCaseInsensitive(unit, "in")) {
    return 96.0;
  }
  if (util::EqualsAsciiCaseInsensitive(unit, "pc")) {
    return 16.0;  // a pica is twelve points
  }
  if (util::EqualsAsciiCaseInsensitive(unit, "pt")) {
    return 4.0 / 3.0;
  }
  if (util::EqualsAsciiCaseInsensitive(unit, "cm")) {
    return 96.0 / 2.54;
  }
  if (util::EqualsAsciiCaseInsensitive(unit, "mm")) {
    return 96.0 / 25.4;
  }
  if (util::EqualsAsciiCaseInsensitive(unit, "q")) {
    return 96.0 / 101.6;  // a quarter of a millimetre
  }
  return std::nullopt;
}

std::optional<float> AbsoluteLengthFromUnit(double magnitude, std::string_view unit,
                                            const MediaContext& context) {
  double per_unit = 0.0;
  if (const std::optional<double> scale = AbsoluteUnitScale(unit)) {
    per_unit = *scale;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "em") ||
             util::EqualsAsciiCaseInsensitive(unit, "rem")) {
    per_unit = static_cast<double>(kRootFontSize);
  } else if (util::EqualsAsciiCaseInsensitive(unit, "ex") ||
             util::EqualsAsciiCaseInsensitive(unit, "ch")) {
    // Half an em: the fallback CSS Values 4 names when the font's x-height and `0` advance are
    // unavailable, which here they always are -- the face is chosen at paint time.
    per_unit = static_cast<double>(kRootFontSize) * 0.5;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vw")) {
    if (context.viewport_width == 0.0f) {
      return std::nullopt;
    }
    per_unit = static_cast<double>(context.viewport_width) / 100.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vh")) {
    if (context.viewport_height == 0.0f) {
      return std::nullopt;
    }
    per_unit = static_cast<double>(context.viewport_height) / 100.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vmin")) {
    if (context.viewport_width == 0.0f || context.viewport_height == 0.0f) {
      return std::nullopt;
    }
    per_unit = static_cast<double>(std::min(context.viewport_width, context.viewport_height)) /
               100.0;
  } else if (util::EqualsAsciiCaseInsensitive(unit, "vmax")) {
    if (context.viewport_width == 0.0f || context.viewport_height == 0.0f) {
      return std::nullopt;
    }
    per_unit = static_cast<double>(std::max(context.viewport_width, context.viewport_height)) /
               100.0;
  } else {
    return std::nullopt;
  }
  const double pixels = magnitude * per_unit;
  constexpr double kFloatLimit = 3.0e38;
  if (!(pixels >= -kFloatLimit && pixels <= kFloatLimit)) {
    return std::nullopt;
  }
  return static_cast<float>(pixels);
}

std::optional<float> ResolveAbsoluteLength(std::string_view text, const MediaContext& context) {
  const std::vector<Token> tokens = Tokenize(text);
  Cursor cursor{tokens, 0};
  cursor.SkipWhitespace();
  if (cursor.AtEnd()) {
    return std::nullopt;
  }
  float pixels = 0.0f;
  const bool ok = LengthFromToken(cursor.Peek(), context, pixels);
  ++cursor.at;
  cursor.SkipWhitespace();
  if (!ok || !cursor.AtEnd()) {
    return std::nullopt;  // not a length, or more than one token
  }
  return pixels;
}

}  // namespace microbrowser::css

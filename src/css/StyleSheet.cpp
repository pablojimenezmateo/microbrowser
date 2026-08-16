#include "css/StyleSheet.h"

#include "css/MediaQuery.h"
#include "util/Parse.h"

#include <optional>
#include <utility>

#include "css/CssText.h"
#include "util/StringUtil.h"
#include "css/Selectors.h"
#include "css/StyleResolver.h"
#include "css/Tokenizer.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::css {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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
    if (tokens[at].kind == Token::Kind::AtKeyword) {
      // Consume an at-rule: prelude until `;` or a matching `{}` block. Skipping
      // to the next semicolon would swallow `color: green` after `@at {}`.
      ++at;
      int depth = 0;
      while (at < to) {
        if (depth == 0 && tokens[at].kind == Token::Kind::Semicolon) {
          ++at;
          break;
        }
        if (tokens[at].kind == Token::Kind::LeftBrace) {
          ++depth;
        } else if (tokens[at].kind == Token::Kind::RightBrace) {
          if (depth == 0) {
            break;
          }
          --depth;
          ++at;
          if (depth == 0) {
            break;
          }
          continue;
        }
        ++at;
      }
      continue;
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

    declaration.value = ReconstructTokens(tokens, value_start, value_end);
    if (!declaration.value.empty()) {
      declarations.push_back(std::move(declaration));
    }
  }
  return declarations;
}

// --- @supports ---------------------------------------------------------------
//
// The condition grammar of CSS Conditional 3 §2, over the token run between
// `@supports` and its block.
//
// What makes this feature worth having is also what makes it dangerous: it is
// the page asking what we can do, and every wrong answer sends it down a branch
// written for a different browser. So there is exactly one source for the
// answer -- SupportsDeclaration, which applies the declaration and reports
// whether it took. Nothing here has a list of properties in it.

// How deep a condition may nest. Same reasoning and same order of magnitude as
// kMaxSelectorNestingDepth: this is a recursive descent over a stylesheet, and
// a stylesheet is hostile input.
constexpr int kMaxSupportsNestingDepth = 8;

bool SupportsConditionMatches(const std::vector<Token>& tokens, std::size_t& at, std::size_t to,
                              int depth);

void SkipWhitespace(const std::vector<Token>& tokens, std::size_t& at, std::size_t to) {
  while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
    ++at;
  }
}

// The index just past the parenthesis or function opened at `open`. Nullopt
// when it is never closed — which is a syntax error rather than a condition
// that happens to end at the block, and the difference is the whole prelude:
// `@supports (display: flex {` must not apply its block.
std::optional<std::size_t> MatchingParen(const std::vector<Token>& tokens, std::size_t open,
                                         std::size_t to) {
  int depth = 0;
  for (std::size_t at = open; at < to; ++at) {
    if (tokens[at].kind == Token::Kind::LeftParen || tokens[at].kind == Token::Kind::Function) {
      ++depth;
    } else if (tokens[at].kind == Token::Kind::RightParen) {
      if (--depth == 0) {
        return at + 1;
      }
    }
  }
  return std::nullopt;
}

// `(property: value)`, which is the only thing in this grammar that consults
// the engine. Unsupported when it is not a declaration at all.
bool SupportsDeclarationInParens(const std::vector<Token>& tokens, std::size_t from,
                                 std::size_t to) {
  SkipWhitespace(tokens, from, to);
  if (from >= to || tokens[from].kind != Token::Kind::Ident) {
    return false;
  }
  const std::string property = tokens[from++].value;
  SkipWhitespace(tokens, from, to);
  if (from >= to || tokens[from].kind != Token::Kind::Colon) {
    return false;
  }
  ++from;
  return SupportsDeclaration(property, ReconstructTokens(tokens, from, to));
}

// `( … )` or `function( … )`. The second is `<general-enclosed>`: a form this
// engine does not recognize, which the specification says evaluates to unknown
// and which a boolean context reads as false. `selector()` and `font-tech()`
// land there, and false is the answer that sends a page to its fallback rather
// than into a branch nothing here implements.
bool SupportsInParens(const std::vector<Token>& tokens, std::size_t& at, std::size_t to,
                      int depth) {
  SkipWhitespace(tokens, at, to);
  if (at >= to) {
    return false;
  }
  if (tokens[at].kind == Token::Kind::Function) {
    at = MatchingParen(tokens, at, to).value_or(to);
    return false;
  }
  if (tokens[at].kind != Token::Kind::LeftParen) {
    return false;
  }
  const std::optional<std::size_t> close = MatchingParen(tokens, at, to);
  const std::size_t inner_from = at + 1;
  at = close.value_or(to);
  if (!close.has_value() || depth >= kMaxSupportsNestingDepth) {
    return false;
  }
  const std::size_t inner_to = *close > inner_from ? *close - 1 : inner_from;

  // Inside the parentheses is either a declaration or another condition. The
  // two are told apart by what a condition must start with -- `not`, or another
  // parenthesis -- because a declaration always starts with an ident followed
  // by a colon, and `not` is not a property.
  std::size_t probe = inner_from;
  SkipWhitespace(tokens, probe, inner_to);
  const bool nested = probe < inner_to &&
                      (tokens[probe].kind == Token::Kind::LeftParen ||
                       tokens[probe].kind == Token::Kind::Function ||
                       (tokens[probe].kind == Token::Kind::Ident &&
                        Lowered(tokens[probe].value) == "not"));
  if (!nested) {
    return SupportsDeclarationInParens(tokens, inner_from, inner_to);
  }
  std::size_t inner_at = inner_from;
  const bool result = SupportsConditionMatches(tokens, inner_at, inner_to, depth + 1);
  SkipWhitespace(tokens, inner_at, inner_to);
  return inner_at == inner_to && result;
}

// `not X`, `X and Y and …`, `X or Y or …`. The specification forbids mixing
// `and` with `or` without parentheses, and a mixed prelude is a syntax error
// rather than a precedence question -- which is why the first keyword seen
// fixes the operator and a different one ends the condition unconsumed, leaving
// the caller to reject the trailing tokens.
bool SupportsConditionMatches(const std::vector<Token>& tokens, std::size_t& at, std::size_t to,
                              int depth) {
  SkipWhitespace(tokens, at, to);
  if (at < to && tokens[at].kind == Token::Kind::Ident &&
      Lowered(tokens[at].value) == "not") {
    ++at;
    return !SupportsInParens(tokens, at, to, depth);
  }

  bool result = SupportsInParens(tokens, at, to, depth);
  std::string op;
  while (true) {
    const std::size_t mark = at;
    SkipWhitespace(tokens, at, to);
    if (at >= to || tokens[at].kind != Token::Kind::Ident) {
      at = mark;
      return result;
    }
    const std::string keyword = Lowered(tokens[at].value);
    if (keyword != "and" && keyword != "or") {
      at = mark;
      return result;
    }
    if (op.empty()) {
      op = keyword;
    } else if (op != keyword) {
      at = mark;
      return result;  // mixed without parentheses; the caller rejects the rest
    }
    ++at;
    // Both sides are evaluated: there is no cost to it, and short-circuiting
    // would leave the token cursor mid-condition on the side not taken.
    const bool right = SupportsInParens(tokens, at, to, depth);
    result = op == "and" ? (result && right) : (result || right);
  }
}

bool SupportsPreludeMatches(const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
  std::size_t at = from;
  const bool result = SupportsConditionMatches(tokens, at, to, 0);
  SkipWhitespace(tokens, at, to);
  // Trailing tokens mean the prelude is not a condition this grammar accepts,
  // and an unparsable prelude is false rather than "whatever we got so far".
  return at == to && result;
}

// Whether an `@media` prelude matches, through the one evaluator.
//
// This used to accept a single Ident and nothing else, so `@media (min-width:
// 600px)` dropped its whole block -- on every page ever rendered by this
// browser. `css::MediaQueryListMatches` landed with `srcset` in session 6 and
// answers exactly this grammar, including `and`/`or`/`not` and the comma-
// separated list; the only thing missing was the call. ADR 0014 counts `@media`
// at 791 occurrences and calls it supported.
//
// Evaluated at *parse* time rather than kept on the rule, which is the crude
// part and is written down rather than hidden: a sheet parsed at one viewport
// holds the rules that matched then, so a resize has to re-parse. `Page`
// re-parses when the viewport changes (see Page::SetViewport), which makes the
// behaviour correct at the cost of doing the work again. Keeping the condition
// on the rule and asking it during the cascade is the right end state and is a
// bigger change than the bug deserved.
// The index of the `)` closing the function at `open`, or `to`.
using util::ParseInt;

std::size_t FunctionEnd(const std::vector<Token>& tokens, std::size_t open, std::size_t to) {
  int depth = 1;
  std::size_t at = open + 1;
  while (at < to && tokens[at].kind != Token::Kind::EndOfFile) {
    if (tokens[at].kind == Token::Kind::Function || tokens[at].kind == Token::Kind::LeftParen) {
      ++depth;
    } else if (tokens[at].kind == Token::Kind::RightParen) {
      if (--depth == 0) {
        return at;
      }
    }
    ++at;
  }
  return to;
}

// `src: url(a.woff2) format("woff2"), url(a.woff)`. One entry per comma, each a
// URL and an optional format hint.
//
// Anything else in an entry -- `local(...)`, which names a font on the machine --
// is skipped rather than guessed at: this browser has a system font database and
// answering `local()` from it would let a page ask which fonts are installed,
// which is the fingerprinting surface ADR 0029 prices separately.
std::vector<FontFaceSource> ParseFontFaceSources(std::string_view value) {
  std::vector<FontFaceSource> sources;
  const std::vector<Token> tokens = Tokenize(value);
  std::size_t at = 0;
  FontFaceSource current;
  const auto flush = [&sources, &current]() {
    if (!current.url.empty()) {
      sources.push_back(current);
    }
    current = FontFaceSource{};
  };
  while (at < tokens.size() && tokens[at].kind != Token::Kind::EndOfFile) {
    const Token& token = tokens[at];
    if (token.kind == Token::Kind::Comma) {
      flush();
      ++at;
      continue;
    }
    if (token.kind == Token::Kind::Url) {
      current.url = token.value;
      ++at;
      continue;
    }
    if (token.kind == Token::Kind::Function) {
      const std::string function = Lowered(token.value);
      const std::size_t close = FunctionEnd(tokens, at, tokens.size());
      if (function == "url" && at + 1 < close) {
        current.url = tokens[at + 1].value;
      } else if (function == "format" && at + 1 < close) {
        current.format = Lowered(tokens[at + 1].value);
      }
      at = close < tokens.size() ? close + 1 : tokens.size();
      continue;
    }
    ++at;
  }
  flush();
  return sources;
}

// The `unicode-range` descriptor. The tokenizer has already done the hard part --
// every range arrives as one token with both ends resolved -- so this is a filter
// over the value's tokens. A value with something else in it keeps the ranges it
// did name: a face that lists eight subsets and one typo covers the eight.
std::vector<UnicodeRange> ParseUnicodeRanges(std::string_view value) {
  std::vector<UnicodeRange> ranges;
  for (const Token& token : Tokenize(value)) {
    if (token.kind == Token::Kind::UnicodeRange) {
      ranges.push_back(UnicodeRange{token.range_start, token.range_end});
    }
  }
  return ranges;
}


void ParseFontFace(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                   StyleSheet& sheet) {
  FontFace face;
  for (const Declaration& declaration : ParseDeclarations(tokens, from, to)) {
    const std::string name = Lowered(declaration.property);
    if (name == "font-family") {
      // Unquoted or quoted; the tokenizer has already taken the quotes off, so
      // this is the family exactly as a `font-family` stack will name it.
      face.family = declaration.value;
    } else if (name == "src") {
      face.sources = ParseFontFaceSources(declaration.value);
    } else if (name == "font-weight") {
      // One number, or `normal`/`bold`. A *range* -- `font-weight: 100 900`, a
      // variable font -- is deliberately taken as its first value rather than
      // rejected: the face still renders, and refusing it would drop a font that
      // works for a descriptor this browser cannot vary along.
      const std::string lowered = Lowered(declaration.value);
      if (lowered == "bold") {
        face.weight = 700;
      } else if (lowered == "normal") {
        face.weight = 400;
      } else if (const std::optional<int> parsed =
                     ParseInt(std::string_view(lowered).substr(0, lowered.find(' ')))) {
        face.weight = *parsed;
      }
    } else if (name == "font-style") {
      const std::string lowered = Lowered(declaration.value);
      face.italic = lowered == "italic" || lowered.rfind("oblique", 0) == 0;
    } else if (name == "font-display") {
      face.display = Lowered(declaration.value);
    } else if (name == "unicode-range") {
      face.unicode_ranges = ParseUnicodeRanges(declaration.value);
    }
  }
  // A face with no family or no source names nothing and fetches nothing. Counted
  // as skipped, because a page whose font never appears wants to know that the
  // browser read the block and found it empty.
  if (face.family.empty() || face.sources.empty()) {
    ++sheet.skipped;
    return;
  }
  sheet.font_faces.push_back(std::move(face));
}

bool MediaPreludeMatches(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                         const MediaContext& context) {
  // Through ReconstructTokens, which already turns a token run back into text
  // for a declaration's value and handles every token a media prelude can contain.
  return MediaQueryListMatches(ReconstructTokens(tokens, from, to), context);
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

// One keyframe selector: `from`, `to`, or a percentage. Nothing else is legal, and a block whose
// prelude is something else is dropped rather than placed at zero -- an unrecognised offset silently
// becoming the first keyframe would rewrite the animation's start.
bool KeyframeOffset(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                    double& out) {
  std::size_t at = from;
  while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
    ++at;
  }
  if (at >= to) {
    return false;
  }
  if (tokens[at].kind == Token::Kind::Ident) {
    const std::string word = Lowered(tokens[at].value);
    if (word == "from") {
      out = 0.0;
      return true;
    }
    if (word == "to") {
      out = 1.0;
      return true;
    }
    return false;
  }
  if (tokens[at].kind == Token::Kind::Percentage) {
    // Outside 0..100 is invalid rather than clamped: the specification says so, and a `-50%` keyframe
    // clamped to zero would replace the animation's real start.
    if (tokens[at].number < 0.0 || tokens[at].number > 100.0) {
      return false;
    }
    out = tokens[at].number / 100.0;
    return true;
  }
  return false;
}

void ParseKeyframes(const std::vector<Token>& tokens, std::size_t prelude_from,
                    std::size_t prelude_to, std::size_t from, std::size_t to, StyleSheet& sheet) {
  KeyframesRule rule;
  for (std::size_t at = prelude_from; at < prelude_to; ++at) {
    if (tokens[at].kind == Token::Kind::Ident || tokens[at].kind == Token::Kind::String) {
      rule.name = tokens[at].value;
      break;
    }
  }
  if (rule.name.empty()) {
    ++sheet.skipped;
    return;
  }
  std::size_t at = from;
  while (at < to) {
    while (at < to && (tokens[at].kind == Token::Kind::Whitespace ||
                       tokens[at].kind == Token::Kind::Semicolon)) {
      ++at;
    }
    if (at >= to) {
      break;
    }
    const std::size_t selector_start = at;
    while (at < to && tokens[at].kind != Token::Kind::LeftBrace &&
           tokens[at].kind != Token::Kind::EndOfFile) {
      ++at;
    }
    if (at >= to || tokens[at].kind != Token::Kind::LeftBrace) {
      ++sheet.skipped;
      break;
    }
    const std::size_t selector_end = at;
    const std::size_t block_start = at + 1;
    const std::size_t block_end = FindBlockEnd(tokens, at, to);
    // A keyframe selector may be a *list*: `0%, 50% { … }` is two keyframes with one declaration
    // block, which is how a page writes an animation that pauses in the middle.
    std::vector<double> offsets;
    std::size_t piece = selector_start;
    while (piece <= selector_end) {
      std::size_t comma = piece;
      while (comma < selector_end && tokens[comma].kind != Token::Kind::Comma) {
        ++comma;
      }
      double offset = 0.0;
      if (KeyframeOffset(tokens, piece, comma, offset)) {
        offsets.push_back(offset);
      } else {
        ++sheet.skipped;
      }
      if (comma >= selector_end) {
        break;
      }
      piece = comma + 1;
    }
    if (!offsets.empty()) {
      const std::vector<Declaration> declarations =
          ParseDeclarations(tokens, block_start, block_end);
      for (const double offset : offsets) {
        Keyframe frame;
        frame.offset = offset;
        for (const Declaration& declaration : declarations) {
          frame.declarations.emplace_back(declaration.property, declaration.value);
        }
        rule.frames.push_back(std::move(frame));
      }
    }
    at = block_end;
    if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
      ++at;
    }
  }
  // Sorted by offset, stably, so that two blocks at the same offset keep document order -- the later
  // one wins per property, which is the cascade rule applied inside an animation.
  std::stable_sort(rule.frames.begin(), rule.frames.end(),
                   [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
  if (rule.frames.empty()) {
    ++sheet.skipped;
    return;
  }
  // A later block with the same name *replaces* an earlier one rather than merging with it: a page
  // that redefines `spin` means the new one.
  const auto existing = std::find_if(sheet.keyframes.begin(), sheet.keyframes.end(),
                                     [&rule](const KeyframesRule& held) {
                                       return util::EqualsAsciiCaseInsensitive(held.name, rule.name);
                                     });
  if (existing != sheet.keyframes.end()) {
    *existing = std::move(rule);
  } else {
    sheet.keyframes.push_back(std::move(rule));
  }
}

void ParseRuleList(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                   const MediaContext& context,
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

      if (at_rule == "keyframes") {
        // `@keyframes <name> { <offset> { declarations } ... }`, which is the only at-rule whose block
        // contains *rules whose preludes are percentages* rather than selectors -- so it is parsed here
        // rather than by ParseRuleList, which would try to read `0%` as a selector and drop the block.
        //
        // The declarations are kept as text. See the note on `Keyframe`: a keyframe's values resolve
        // against the element the animation runs on, and resolving them here would freeze `50%` and
        // `2em` against the wrong one.
        ParseKeyframes(tokens, prelude_start, block_start - 1, block_start, block_end, sheet);
        at = block_end;
        if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
          ++at;
        }
        continue;
      }

      if (at_rule == "font-face") {
        // A descriptor block, not a rule: it matches nothing and adds a face to
        // the font database. ADR 0024, and the reason it is parsed here rather
        // than by whoever loads fonts is that the declaration grammar is this
        // file's and a second parser for `src:` would be a second answer about
        // what `format("woff2")` means.
        ParseFontFace(tokens, block_start, block_end, sheet);
        at = block_end;
        if (at < to && tokens[at].kind == Token::Kind::RightBrace) {
          ++at;
        }
        continue;
      }

      const bool conditional_holds =
          at_rule == "media"
              ? MediaPreludeMatches(tokens, prelude_start, block_start - 1, context)
          : at_rule == "supports"
              ? SupportsPreludeMatches(tokens, prelude_start, block_start - 1)
              : false;
      if (conditional_holds) {
        ParseRuleList(tokens, block_start, block_end, context, sheet);
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

bool SupportsConditionTextImpl(std::string_view condition) {
  auto usable_end = [](const std::vector<Token>& tokens) {
    std::size_t end = tokens.size();
    while (end > 0 && tokens[end - 1].kind == Token::Kind::EndOfFile) {
      --end;
    }
    return end;
  };
  const std::vector<Token> tokens = Tokenize(condition);
  if (SupportsPreludeMatches(tokens, 0, usable_end(tokens))) {
    return true;
  }
  // CSS.supports(conditionText): if the bare text is not a supports-condition,
  // wrap it in parentheses and try again (CSS Conditional Rules §6.1). That is
  // why `CSS.supports('display: flex')` works while `@supports display: flex`
  // does not — the API is the forgiving entry point.
  std::string wrapped;
  wrapped.reserve(condition.size() + 2);
  wrapped.push_back('(');
  wrapped.append(condition);
  wrapped.push_back(')');
  const std::vector<Token> wrapped_tokens = Tokenize(wrapped);
  return SupportsPreludeMatches(wrapped_tokens, 0, usable_end(wrapped_tokens));
}

}  // namespace

std::vector<Declaration> ParseDeclarationList(std::string_view input) {
  const std::vector<Token> tokens = Tokenize(input);
  return ParseDeclarations(tokens, 0, tokens.size());
}

StyleSheet ParseStyleSheet(std::string_view input, const MediaContext& context) {
  StyleSheet sheet;
  const std::vector<Token> tokens = Tokenize(input);
  AddPerformanceCounter(PerfCounterId::CssSheetsParsed);
  ParseRuleList(tokens, 0, tokens.size(), context, sheet);
  return sheet;
}

bool SupportsConditionText(std::string_view condition) {
  return SupportsConditionTextImpl(condition);
}

}  // namespace microbrowser::css

#include "css/Selectors.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/CssText.h"
#include "css/StyleSheet.h"
#include "css/Token.h"
#include "css/Tokenizer.h"

namespace microbrowser::css {

namespace {

// The index of the `)` closing the function whose token sits at `open`, or `to`
// if the run ends first. A nested function and a bare `(` both raise the depth,
// so `:not(:is(a))` and a selector holding a stray paren each find their own
// close rather than the first one they meet.
std::size_t FindFunctionEnd(const std::vector<Token>& tokens, std::size_t open, std::size_t to) {
  int depth = 1;
  for (std::size_t at = open + 1; at < to; ++at) {
    switch (tokens[at].kind) {
      case Token::Kind::Function:
      case Token::Kind::LeftParen:
        ++depth;
        break;
      case Token::Kind::RightParen:
        if (--depth == 0) {
          return at;
        }
        break;
      case Token::Kind::EndOfFile:
        return to;
      default:
        break;
    }
  }
  return to;
}

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

// The An+B microsyntax of CSS Syntax Level 3 §9, over the tokens between a
// `:nth-child(` and its `)`. Sets only `a` and `b`; which sequence they count
// over was decided by the function name. Returns false for anything it does not
// recognise — including `An+B of S`, which is a real selector this engine does
// not implement, and which therefore drops its rule rather than silently
// counting over the wrong siblings.
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

}  // namespace

// The contract, and the reason for the range form, are on the declaration in
// `Selectors.h`.
std::vector<Selector> ParseSelectors(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to, int depth) {
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
      // `::before` / `::after` are only legal on the subject, and only once.
      for (std::size_t c = 0; c < current.compounds.size(); ++c) {
        int pseudos = 0;
        for (const SelectorPart& part : current.compounds[c].parts) {
          if (part.kind == SelectorPart::Kind::PseudoElement) {
            ++pseudos;
          }
        }
        if (pseudos > 1 || (pseudos > 0 && c + 1 != current.compounds.size())) {
          failed = true;
          current = Selector{};
          return;
        }
      }
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
        // `::before` / `::after` are pseudo-*elements*: they style a generated
        // box, so the rest of the subject still matches the originating element
        // and layout invents the box. `::slotted(sel)` is the other
        // double-colon form and is different -- it selects a real light-DOM node.
        if (i + 1 < to && tokens[i + 1].kind == Token::Kind::Colon) {
          if (i + 2 < to && tokens[i + 2].kind == Token::Kind::Function &&
              Lowered(tokens[i + 2].value) == "slotted") {
            const std::size_t close = FindFunctionEnd(tokens, i + 2, to);
            if (close >= to || depth >= kMaxSelectorNestingDepth) {
              failed = true;
              break;
            }
            SelectorPart part;
            part.kind = SelectorPart::Kind::Slotted;
            part.name = "slotted";
            part.arguments = ParseSelectors(tokens, i + 3, close, depth + 1);
            if (part.arguments.empty()) {
              failed = true;
              break;
            }
            compound.parts.push_back(std::move(part));
            saw_part = true;
            i = close;
            break;
          }
          if (i + 2 < to && tokens[i + 2].kind == Token::Kind::Ident) {
            const std::string name = Lowered(tokens[i + 2].value);
            if (name == "before" || name == "after") {
              // Only on the subject compound, and only once: `div::before::after`
              // and `div::before span` are invalid. Enforced when the compound is
              // finished, by rejecting a non-subject that carries one.
              SelectorPart part;
              part.kind = SelectorPart::Kind::PseudoElement;
              part.name = name;
              compound.parts.push_back(std::move(part));
              saw_part = true;
              i += 2;
              break;
            }
          }
          failed = true;
          break;
        }
        if (i + 1 < to && tokens[i + 1].kind == Token::Kind::Function) {
          const std::string function = Lowered(tokens[i + 1].value);
          const std::size_t close = FindFunctionEnd(tokens, i + 1, to);
          if (close >= to) {
            failed = true;  // unterminated: the rule goes, per CSS recovery
            break;
          }
          SelectorPart part;
          if (function == "is" || function == "where" || function == "not") {
            if (depth >= kMaxSelectorNestingDepth) {
              failed = true;
              break;
            }
            part.kind = function == "is"      ? SelectorPart::Kind::Is
                        : function == "where" ? SelectorPart::Kind::Where
                                              : SelectorPart::Kind::Not;
            part.arguments = ParseSelectors(tokens, i + 2, close, depth + 1);
            if (part.arguments.empty()) {
              failed = true;  // `:not()` and `:is(!)` are both invalid selectors
              break;
            }
          } else if (function == "nth-child" || function == "nth-last-child" ||
                     function == "nth-of-type" || function == "nth-last-of-type") {
            part.kind = SelectorPart::Kind::Nth;
            part.nth.from_end =
                function == "nth-last-child" || function == "nth-last-of-type";
            part.nth.of_type = function == "nth-of-type" || function == "nth-last-of-type";
            if (!ParseAnPlusB(tokens, i + 2, close, part.nth)) {
              failed = true;
              break;
            }
          } else if (function == "host") {
            // `:host(sel)`. The argument is matched against the *host*, so it is
            // an ordinary nested selector list -- the only unusual thing is which
            // element it is asked about, and that is the resolver's business.
            if (depth >= kMaxSelectorNestingDepth) {
              failed = true;
              break;
            }
            part.kind = SelectorPart::Kind::Host;
            part.arguments = ParseSelectors(tokens, i + 2, close, depth + 1);
            if (part.arguments.empty()) {
              failed = true;
              break;
            }
          } else {
            // `:has()`, `:lang()`, and every other functional pseudo-class this
            // engine does not implement. Dropped rather than guessed at: ADR
            // 0016 prices `:has()` separately and ADR 0012 says why a stub is
            // worse than an absence.
            failed = true;
            break;
          }
          part.name = function;
          compound.parts.push_back(std::move(part));
          saw_part = true;
          i = close;
          break;
        }
        if (i + 1 >= to || tokens[i + 1].kind != Token::Kind::Ident) {
          failed = true;
          break;
        }
        SelectorPart part;
        part.name = Lowered(tokens[++i].value);
        // Bare `:host`, which is the common spelling -- `:host(sel)` is the
        // qualified one and is parsed above. Its own kind rather than a
        // pseudo-class name the matcher special-cases, because *which root the
        // rule came from* is not a question the matcher may ask. ADR 0019 §3.
        //
        // Legacy single-colon `:before` / `:after` are still in the wild (and
        // on youtube's sheet): same generated boxes as the double-colon form.
        if (part.name == "host") {
          part.kind = SelectorPart::Kind::Host;
        } else if (part.name == "before" || part.name == "after") {
          part.kind = SelectorPart::Kind::PseudoElement;
        } else {
          part.kind = SelectorPart::Kind::PseudoClass;
        }
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

std::vector<Selector> ParseSelectorList(std::string_view input) {
  const std::vector<Token> tokens = Tokenize(input);
  std::size_t end = tokens.size();
  while (end > 0 && tokens[end - 1].kind == Token::Kind::EndOfFile) {
    --end;
  }
  return ParseSelectors(tokens, 0, end);
}

}  // namespace microbrowser::css

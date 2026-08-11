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

// The index of the `of` that separates `An+B` from the selector list of
// `:nth-child(An+B of S)`, or `to` when there is none.
//
// Found by scanning for a top-level `of` identifier rather than by parsing the
// An+B first: `of` is a perfectly good type selector, so `:nth-child(2n of of)`
// splits at the *first* one, which is what the grammar says -- An+B is greedy
// only up to the keyword.
std::size_t FindNthOf(const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
  int depth = 0;
  for (std::size_t at = from; at < to; ++at) {
    switch (tokens[at].kind) {
      case Token::Kind::Function:
      case Token::Kind::LeftParen:
      case Token::Kind::LeftSquare:
        ++depth;
        break;
      case Token::Kind::RightParen:
      case Token::Kind::RightSquare:
        --depth;
        break;
      case Token::Kind::Ident:
        if (depth == 0 && Lowered(tokens[at].value) == "of") {
          return at;
        }
        break;
      default:
        break;
    }
  }
  return to;
}

// Splits `tokens[from, to)` at its top-level commas. Used by the *forgiving*
// selector lists of `:is()` and `:where()`, where an argument this engine
// cannot parse is dropped and the rest of the list survives -- which is the
// entire reason those two exist. `:not()` and `:has()` do not get this: they
// are unforgiving by definition, and one bad argument invalidates the whole
// selector.
std::vector<std::pair<std::size_t, std::size_t>> SplitOnCommas(const std::vector<Token>& tokens,
                                                              std::size_t from, std::size_t to) {
  std::vector<std::pair<std::size_t, std::size_t>> parts;
  int depth = 0;
  std::size_t start = from;
  for (std::size_t at = from; at < to; ++at) {
    switch (tokens[at].kind) {
      case Token::Kind::Function:
      case Token::Kind::LeftParen:
      case Token::Kind::LeftSquare:
        ++depth;
        break;
      case Token::Kind::RightParen:
      case Token::Kind::RightSquare:
        --depth;
        break;
      case Token::Kind::Comma:
        if (depth == 0) {
          parts.emplace_back(start, at);
          start = at + 1;
        }
        break;
      default:
        break;
    }
  }
  parts.emplace_back(start, to);
  return parts;
}

// The argument of `:lang()`: one or more language *ranges*, each an identifier,
// a string, or a `*` standing for any subtag. Written into `out` comma-joined
// and folded to lower case, because a language tag is case-insensitive and the
// matcher should not have to know that twice.
//
// The joining is what the grammar needs rather than an economy: `fr-*` arrives
// as the identifier `fr-` and the delimiter `*`, and `*-CH` as `*` and `-ch`,
// so a range is a run of adjacent tokens with no whitespace between them.
bool ParseLangRanges(const std::vector<Token>& tokens, std::size_t from, std::size_t to,
                     std::string& out) {
  for (const auto& [start, end] : SplitOnCommas(tokens, from, to)) {
    std::size_t at = start;
    std::size_t stop = end;
    while (at < stop && tokens[at].kind == Token::Kind::Whitespace) {
      ++at;
    }
    while (stop > at && tokens[stop - 1].kind == Token::Kind::Whitespace) {
      --stop;
    }
    std::string range;
    for (; at < stop; ++at) {
      const Token& token = tokens[at];
      if (token.kind == Token::Kind::Ident || token.kind == Token::Kind::String) {
        range += Lowered(token.value);
      } else if (token.kind == Token::Kind::Delim && token.value == "*") {
        range += '*';
      } else {
        return false;  // whitespace inside a range included: `:lang(en us)`
      }
    }
    if (range.empty()) {
      return false;  // `:lang()` and `:lang(en,)` are not selectors
    }
    if (!out.empty()) {
      out += ',';
    }
    out += range;
  }
  return !out.empty();
}

// The pseudo-classes that are *only* functional. Written without an argument
// they are not selectors at all -- `:has` and `:nth-child` name nothing -- and
// the bare-identifier branch would otherwise turn each into a pseudo-class this
// engine does not implement, which matches nothing and therefore looks right
// while parsing a rule the author did not write.
bool RequiresAnArgument(std::string_view name) {
  return name == "is" || name == "where" || name == "not" || name == "has" || name == "lang" ||
         name == "dir" || name == "nth-child" || name == "nth-last-child" ||
         name == "nth-of-type" || name == "nth-last-of-type" || name == "slotted" ||
         name == "part" || name == "state" || name == "nth-col" || name == "nth-last-col";
}

// Whether `text` is a bare `i`/`I` or `s`/`S` -- the attribute case flags.
SelectorPart::AttributeCase CaseFlag(std::string_view text) {
  if (text == "i" || text == "I") {
    return SelectorPart::AttributeCase::Insensitive;
  }
  if (text == "s" || text == "S") {
    return SelectorPart::AttributeCase::Sensitive;
  }
  return SelectorPart::AttributeCase::Default;
}

}  // namespace

// The contract, and the reason for the range form, are on the declaration in
// `Selectors.h`.
std::vector<Selector> ParseSelectors(const std::vector<Token>& tokens, std::size_t from,
                                     std::size_t to, SelectorParseMode mode) {
  const int depth = mode.depth;
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

  // The local half of a qualified name, with `at` on the token after the `|`.
  // `*|div`, `|div`, `*|*` and `|*` all come through here; a *named* prefix
  // never does, because resolving one needs an `@namespace` rule this engine
  // does not have and a prefix resolved to nothing would match elements the
  // author did not name.
  const auto take_local_name = [&](std::size_t& at, SelectorPart::NamespaceMatch name_space) {
    if (at >= to) {
      failed = true;
      return;
    }
    SelectorPart part;
    part.name_space = name_space;
    if (tokens[at].kind == Token::Kind::Ident) {
      part.kind = SelectorPart::Kind::Type;
      part.name = Lowered(tokens[at].value);
    } else if (tokens[at].kind == Token::Kind::Delim && tokens[at].value == "*") {
      part.kind = SelectorPart::Kind::Universal;
    } else {
      failed = true;
      return;
    }
    compound.parts.push_back(std::move(part));
    saw_part = true;
  };

  // A combinator, with the two ways of writing one that are not selectors:
  // `> .b` outside a `:has()` argument, and `div > > p` anywhere.
  const auto take_combinator = [&](Combinator combinator) {
    const bool leading = current.compounds.empty() && !saw_part;
    if ((leading && !mode.relative) || (!saw_part && pending != Combinator::None)) {
      failed = true;
      return;
    }
    finish_compound();
    pending = combinator;
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
        if (i + 1 < to && tokens[i + 1].kind == Token::Kind::Delim &&
            tokens[i + 1].value == "|") {
          // A named prefix. See `take_local_name`: unresolvable, so invalid.
          failed = true;
          break;
        }
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
        if (token.value == "*" && i + 1 < to && tokens[i + 1].kind == Token::Kind::Delim &&
            tokens[i + 1].value == "|") {
          std::size_t at = i + 2;
          take_local_name(at, SelectorPart::NamespaceMatch::Any);
          i = at;
        } else if (token.value == "|") {
          std::size_t at = i + 1;
          take_local_name(at, SelectorPart::NamespaceMatch::None);
          i = at;
        } else if (token.value == "*") {
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
          take_combinator(token.value == ">"   ? Combinator::Child
                          : token.value == "+" ? Combinator::NextSibling
                                               : Combinator::LaterSibling);
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
            part.arguments = ParseSelectors(tokens, i + 3, close,
                                            {depth + 1, false, mode.inside_has});
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
          if (function == "is" || function == "where") {
            if (depth >= kMaxSelectorNestingDepth) {
              failed = true;
              break;
            }
            part.kind = function == "is" ? SelectorPart::Kind::Is : SelectorPart::Kind::Where;
            // *Forgiving*, and that is the whole point of these two: an argument
            // this engine cannot parse is dropped and the rest of the list still
            // applies, so one selector from a future level does not cost an
            // author the four beside it. An empty result is a valid `:is()` that
            // matches nothing rather than a parse failure.
            for (const auto& [start, end] : SplitOnCommas(tokens, i + 2, close)) {
              std::vector<Selector> one =
                  ParseSelectors(tokens, start, end, {depth + 1, false, mode.inside_has});
              for (Selector& selector : one) {
                part.arguments.push_back(std::move(selector));
              }
            }
          } else if (function == "not") {
            if (depth >= kMaxSelectorNestingDepth) {
              failed = true;
              break;
            }
            part.kind = SelectorPart::Kind::Not;
            part.arguments =
                ParseSelectors(tokens, i + 2, close, {depth + 1, false, mode.inside_has});
            if (part.arguments.empty()) {
              failed = true;  // `:not()` is unforgiving, and `:not()` is invalid
              break;
            }
          } else if (function == "has") {
            // The relational pseudo-class. Unforgiving like `:not()`, and
            // forbidden inside another `:has()` at any depth -- which is why
            // `inside_has` is a flag rather than a second depth counter.
            if (depth >= kMaxSelectorNestingDepth || mode.inside_has) {
              failed = true;
              break;
            }
            part.kind = SelectorPart::Kind::Has;
            part.arguments = ParseSelectors(tokens, i + 2, close, {depth + 1, true, true});
            if (part.arguments.empty()) {
              failed = true;
              break;
            }
          } else if (function == "lang") {
            // `:lang(en, fr-*)`. Idents and strings both, joined with commas and
            // folded here so the matcher never has to: which language an element
            // is in is a question about text, and the answer must not depend on
            // how the author capitalised the tag.
            if (!ParseLangRanges(tokens, i + 2, close, part.value)) {
              failed = true;
              break;
            }
            part.kind = SelectorPart::Kind::Lang;
          } else if (function == "dir") {
            // Exactly one ident, and *any* ident: `:dir(lol)` is a valid
            // selector that matches nothing, where `:dir('ltr')` is not a
            // selector at all. The grammar and the match are different questions
            // and this is the one place the difference is visible.
            std::size_t at = i + 2;
            while (at < close && tokens[at].kind == Token::Kind::Whitespace) {
              ++at;
            }
            if (at >= close || tokens[at].kind != Token::Kind::Ident) {
              failed = true;
              break;
            }
            part.value = Lowered(tokens[at].value);
            ++at;
            while (at < close && tokens[at].kind == Token::Kind::Whitespace) {
              ++at;
            }
            if (at != close) {
              failed = true;
              break;
            }
            part.kind = SelectorPart::Kind::Dir;
          } else if (function == "nth-child" || function == "nth-last-child" ||
                     function == "nth-of-type" || function == "nth-last-of-type") {
            part.kind = SelectorPart::Kind::Nth;
            part.nth.from_end =
                function == "nth-last-child" || function == "nth-last-of-type";
            part.nth.of_type = function == "nth-of-type" || function == "nth-last-of-type";
            // `An+B of S` -- only on the two `-child` forms, because
            // `:nth-of-type(2n of .x)` would be two ways of saying which
            // sequence to count over and the grammar admits only one.
            const std::size_t of = FindNthOf(tokens, i + 2, close);
            std::size_t nth_end = close;
            if (of != close) {
              if (part.nth.of_type || depth >= kMaxSelectorNestingDepth) {
                failed = true;
                break;
              }
              part.arguments =
                  ParseSelectors(tokens, of + 1, close, {depth + 1, false, mode.inside_has});
              if (part.arguments.empty()) {
                failed = true;
                break;
              }
              nth_end = of;
            }
            if (!ParseAnPlusB(tokens, i + 2, nth_end, part.nth)) {
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
            part.arguments =
                ParseSelectors(tokens, i + 2, close, {depth + 1, false, mode.inside_has});
            if (part.arguments.empty()) {
              failed = true;
              break;
            }
          } else {
            // Every other functional pseudo-class this engine does not
            // implement. Dropped rather than guessed at -- ADR 0012 says why a
            // stub is worse than an absence -- and inside `:is()`/`:where()`
            // the drop now costs only this argument rather than the list.
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
        if (RequiresAnArgument(part.name)) {
          failed = true;
          break;
        }
        // Bare `:host`, which is the common spelling -- `:host(sel)` is the
        // qualified one and is parsed above. Its own kind rather than a
        // pseudo-class name the matcher special-cases, because *which root the
        // rule came from* is not a question the matcher may ask. ADR 0019 §3.
        //
        // Legacy single-colon `:before` / `:after` are still in the wild (and
        // on youtube's sheet): same generated boxes as the double-colon form.
        if (part.name == "host") {
          part.kind = SelectorPart::Kind::Host;
        } else if (part.name == "scope") {
          // Its own kind rather than a pseudo-class name, because it is the only
          // selector whose answer depends on *what it was asked about* rather
          // than only on the element -- and that is the same thing `:has()`
          // needs, so they share one field of the match context.
          part.kind = SelectorPart::Kind::Scope;
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
        // The optional namespace prefix. `[att]` and `[|att]` mean the same
        // thing -- an unprefixed attribute is in no namespace -- and `[*|att]`
        // is the only spelling that matches one that is. A `|` here is a prefix
        // and not the start of `|=`, which is what the lookahead separates.
        if (at + 1 < to && tokens[at].kind == Token::Kind::Delim && tokens[at].value == "*" &&
            tokens[at + 1].kind == Token::Kind::Delim && tokens[at + 1].value == "|") {
          part.name_space = SelectorPart::NamespaceMatch::Any;
          at += 2;
        } else if (at < to && tokens[at].kind == Token::Kind::Delim && tokens[at].value == "|") {
          part.name_space = SelectorPart::NamespaceMatch::None;
          ++at;
        }
        if (at >= to || tokens[at].kind != Token::Kind::Ident) {
          failed = true;
          break;
        }
        // A named prefix (`[svg|href]`) needs an `@namespace` rule to resolve.
        if (at + 1 < to && tokens[at + 1].kind == Token::Kind::Delim &&
            tokens[at + 1].value == "|" &&
            !(at + 2 < to && tokens[at + 2].kind == Token::Kind::Delim &&
              tokens[at + 2].value == "=")) {
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
          // The `i`/`s` flag. One, optional, and the last thing in the bracket:
          // `[a=b i i]` and `[a=b i,]` are not selectors. Whitespace either side
          // is optional too, so `[a='b'i]` is one -- comments already vanished
          // in the tokenizer, which is why `[a='b'/**/i]` needs nothing here.
          if (at < to && tokens[at].kind == Token::Kind::Ident) {
            const SelectorPart::AttributeCase flag = CaseFlag(tokens[at].value);
            if (flag == SelectorPart::AttributeCase::Default) {
              failed = true;
              break;
            }
            part.attribute_case = flag;
            ++at;
            while (at < to && tokens[at].kind == Token::Kind::Whitespace) {
              ++at;
            }
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

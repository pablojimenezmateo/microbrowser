#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "js/Ast.h"
#include "js/Lexer.h"

namespace microbrowser::js {

struct ParseError {
  std::string message;
  std::size_t offset = 0;
  std::size_t line = 1;
};

struct ParseResult {
  NodePtr program;
  // Empty on success. A parse either produces a tree or says why it could not;
  // there is no half-tree, because a consumer that had to check every node for
  // validity would check most of them and forget one.
  std::vector<ParseError> errors;

  bool Ok() const { return errors.empty() && program != nullptr; }
};

// Parses a script.
//
// Recursive descent with precedence climbing for the binary operators, which is
// the shape the grammar is written in and therefore the shape that can be
// checked against it line by line. A table-driven parser would be smaller and
// much harder to argue is correct.
//
// Recursion is bounded. Script is attacker-controlled, and `((((((...` nests as
// deeply as the input is long -- so a depth limit is a memory-safety
// requirement rather than a nicety, and exceeding it is a parse error rather
// than a stack overflow.
ParseResult Parse(std::string_view source);

// The nesting limit above, in `Depth` units rather than in levels of source:
// the expression grammar increments it at several points, so one level of
// nesting costs about three of these. Reading it as "1024 levels" overstates
// what it allows by 3x, which is the kind of misreading that gets a security
// bound raised carelessly.
//
// Measured rather than chosen, because the number is a stack budget and a
// round number is not one. On the 8MB stack this runs on, the parser overflows
// somewhere between 5,000 and 6,000 levels of nesting -- about 1.4KB each --
// so this is a 12x margin, and about 0.4MB. The previous value of 256 was 5%
// of the way to the failure it guards against, and real generated code sat
// just the wrong side of it. ADR 0009 has the measurements and what has to be
// re-measured if the engine's thread stack ever shrinks.
inline constexpr int kMaxParseDepth = 1024;

}  // namespace microbrowser::js

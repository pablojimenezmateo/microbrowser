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

// The nesting limit above. Deep enough that no human-written program reaches
// it and shallow enough that the stack does not.
inline constexpr int kMaxParseDepth = 256;

}  // namespace microbrowser::js

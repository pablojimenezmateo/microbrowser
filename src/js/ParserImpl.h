#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "js/Ast.h"
#include "js/Lexer.h"
#include "js/Parser.h"

namespace microbrowser::js {

// The recursive-descent parser's state, shared by the two halves of its
// implementation.
//
// Private to the module -- it is not in src/js/MODULE.deps' `public:` list --
// because the parser's entry point is the free function in Parser.h. This
// header exists only so that expressions and statements can live in separate
// translation units, which they do because one file holding both was over the
// module's line cap, and a file over its cap means a missing seam rather than
// a bigger file.
class ParserImpl {

 public:
  explicit ParserImpl(std::string_view source) : source_(source), lexer_(source) { Advance(); }

  ParseResult ParseProgram();
  // One expression and nothing else. What a template substitution is: `${...}`
  // holds an expression, and parsing it as a program makes a leading `{` a
  // block instead of an object literal -- so `${{a: 1}.a}` was a syntax error.
  ParseResult ParseExpressionSource();

 private:
  // --- Token plumbing ------------------------------------------------------
  void Advance();
  const Token& Current() const { return current_; }
  bool At(std::string_view punctuator) const { return current_.IsPunctuator(punctuator); }
  bool AtKeyword(std::string_view keyword) const { return current_.IsKeyword(keyword); }
  bool AtEnd() const { return current_.type == TokenType::EndOfFile; }
  bool Eat(std::string_view punctuator);
  bool EatKeyword(std::string_view keyword);
  bool Expect(std::string_view punctuator, std::string_view context);
  void Error(std::string message);

  // Automatic semicolon insertion. Returns whether a statement may end here.
  bool ConsumeSemicolon();

  NodePtr Make(NodeKind kind) const;
  // Gives an anonymous function the name of the binding it is being assigned
  // to, which is what makes `const f = () => {}` have `f.name === 'f'`.
  //
  // Here rather than at run time because it is a *syntactic* rule -- the spec
  // calls it named evaluation and applies it to a handful of positions -- and
  // because doing it in the parser means both engines get it from the tree
  // rather than each implementing it.
  static void InferName(Node* value, std::string_view name);

  // --- Statements ----------------------------------------------------------
  NodePtr ParseStatement();
  NodePtr ParseBlock();
  NodePtr ParseVariableDeclaration(bool eat_semicolon);
  NodePtr ParseIf();
  NodePtr ParseFor();
  NodePtr ParseWhile();
  NodePtr ParseDoWhile();
  NodePtr ParseReturn();
  NodePtr ParseThrow();
  NodePtr ParseTry();
  NodePtr ParseSwitch();
  NodePtr ParseFunction(bool declaration);
  NodePtr ParseClass(bool declaration);
  NodePtr ParseParameters();
  NodePtr ParseBindingTarget();

  // --- Expressions ---------------------------------------------------------
  NodePtr ParseExpression();           // comma operator
  NodePtr ParseAssignment();
  NodePtr ParseConditional();
  NodePtr ParseBinary(int min_precedence);
  NodePtr ParseUnary();
  NodePtr ParsePostfix();
  NodePtr ParseCallOrMember(NodePtr base, bool allow_call);
  NodePtr ParseNew();
  NodePtr ParsePrimary();
  NodePtr ParseArrayLiteral();
  NodePtr ParseObjectLiteral();
  NodePtr ParseTemplate();
  NodePtr ParseArrowFromParenthesised();
  // An `async` function, arrow or nothing. Null means the token was an
  // ordinary identifier after all, with the parser left exactly where it was.
  NodePtr ParseAsyncExpression();
  // Whether the parentheses starting here are followed by `=>`. A token scan
  // with a bracket counter, not a parse: `async(x)` is a call and `async(x) =>
  // x` is an arrow, and deciding by parsing and putting it back is exponential
  // when they nest. Leaves the parser exactly where it was.
  bool ArrowFollowsParentheses();
  bool ExpressionToParameters(NodePtr expression, Node& out);
  void ParseArguments(Node& call);

  // Depth guard. Script is attacker-controlled and `((((((` nests as deeply as
  // the input is long, so this is memory safety rather than tidiness.
  class Depth {
   public:
    explicit Depth(ParserImpl& parser) : parser_(parser) { ++parser_.depth_; }
    ~Depth() { --parser_.depth_; }
    Depth(const Depth&) = delete;
    Depth& operator=(const Depth&) = delete;
    bool Exceeded() const { return parser_.depth_ > kMaxParseDepth; }

   private:
    ParserImpl& parser_;
  };

  std::string_view source_;
  Lexer lexer_;
  Token current_;
  std::vector<ParseError> errors_;
  int depth_ = 0;
};

}  // namespace microbrowser::js

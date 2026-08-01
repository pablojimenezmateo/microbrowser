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

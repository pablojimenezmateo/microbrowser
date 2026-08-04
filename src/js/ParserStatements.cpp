#include "js/ParserImpl.h"

#include <algorithm>
#include <utility>

namespace microbrowser::js {

// --- Statements ------------------------------------------------------------

NodePtr ParserImpl::ParseBindingTarget() {
  if (At("[") ) {
    return ParseArrayLiteral();  // an array destructuring pattern
  }
  if (At("{")) {
    return ParseObjectLiteral();  // an object destructuring pattern
  }
  NodePtr node = Make(NodeKind::Identifier);
  if (current_.type == TokenType::Identifier ||
      (current_.type == TokenType::Keyword && current_.lexeme == "let")) {
    node->string = std::string(current_.lexeme);
    Advance();
  } else {
    Error("expected a binding name");
    Advance();
  }
  return node;
}

NodePtr ParserImpl::ParseParameters() {
  NodePtr node = Make(NodeKind::Parameters);
  Expect("(", "to open a parameter list");
  while (!At(")") && !AtEnd()) {
    if (At("...")) {
      NodePtr rest = Make(NodeKind::RestElement);
      Advance();
      rest->children.push_back(ParseBindingTarget());
      node->children.push_back(std::move(rest));
    } else {
      NodePtr target = ParseBindingTarget();
      if (Eat("=")) {
        NodePtr pattern = Make(NodeKind::AssignmentPattern);
        pattern->children.push_back(std::move(target));
        pattern->children.push_back(ParseAssignment());
        target = std::move(pattern);
      }
      node->children.push_back(std::move(target));
    }
    if (!At(")") && !Eat(",")) {
      Error("expected ',' between parameters");
      break;
    }
  }
  Expect(")", "to close a parameter list");
  return node;
}

NodePtr ParserImpl::ParseFunction(bool declaration) {
  NodePtr node = Make(declaration ? NodeKind::FunctionDeclaration : NodeKind::FunctionExpression);
  Advance();  // `function`
  if (Eat("*")) {
    node->number = static_cast<double>(kFunctionGenerator);
  }
  if (current_.type == TokenType::Identifier) {
    node->string = std::string(current_.lexeme);
    Advance();
  } else if (declaration) {
    Error("a function declaration needs a name");
  }
  node->children.push_back(ParseParameters());
  node->children.push_back(ParseBlock());
  return node;
}

NodePtr ParserImpl::ParseClass(bool declaration) {
  NodePtr node = Make(declaration ? NodeKind::ClassDeclaration : NodeKind::ClassExpression);
  Advance();  // `class`
  if (current_.type == TokenType::Identifier) {
    node->string = std::string(current_.lexeme);
    Advance();
  }
  node->children.push_back(EatKeyword("extends") ? ParseUnary() : nullptr);
  Expect("{", "to open a class body");

  while (!At("}") && !AtEnd()) {
    if (Eat(";")) {
      continue;  // a stray semicolon in a class body is legal and means nothing
    }
    NodePtr method = Make(NodeKind::MethodDefinition);
    std::uint8_t flags = kMethodPlain;
    if (current_.lexeme == "static" && current_.type == TokenType::Identifier) {
      const Token saved = current_;
      Advance();
      if (At("(") || At("=")) {
        // `static` used as a method or field name rather than a modifier.
        lexer_.SeekTo(saved.end, saved.line);
        current_ = saved;
      } else if (At("{")) {
        // `static { ... }`: a block that runs once with `this` bound to the
        // class. Carried as a method with no name, so a class body stays one
        // list -- and so its body is compiled through the same path every
        // other method body is.
        flags |= kMethodStatic | kMethodStaticBlock;
        method->number = static_cast<double>(flags);
        NodePtr function = Make(NodeKind::FunctionExpression);
        function->children.push_back(Make(NodeKind::Parameters));
        function->children.push_back(ParseBlock());
        method->children.push_back(std::move(function));
        node->children.push_back(std::move(method));
        continue;
      } else {
        flags |= kMethodStatic;
      }
    }
    if (current_.lexeme == "async" && current_.type == TokenType::Identifier) {
      const Token saved = current_;
      Advance();
      if (At("(") || At("=") || At(";") || At("}") || current_.newline_before) {
        // `async` used as a method or field name rather than a modifier.
        lexer_.SeekTo(saved.end, saved.line);
        current_ = saved;
      } else {
        flags |= kMethodAsync;
      }
    }
    // `*m(){}`, `async *m(){}`, `static *m(){}`. No lookahead needed: `*`
    // cannot start a member name, so seeing one here is unambiguous.
    if (Eat("*")) {
      flags |= kMethodGenerator;
    }
    if ((current_.lexeme == "get" || current_.lexeme == "set") &&
        current_.type == TokenType::Identifier) {
      const bool getter = current_.lexeme == "get";
      const Token saved = current_;
      Advance();
      if (At("(") || At("=") || At(";") || At("}")) {
        lexer_.SeekTo(saved.end, saved.line);
        current_ = saved;
      } else {
        flags |= getter ? kMethodGetter : kMethodSetter;
      }
    }

    if (At("[")) {
      flags |= kMethodComputed;
      Advance();
      method->children.push_back(ParseAssignment());
      Expect("]", "to close a computed method name");
    } else {
      method->string = std::string(current_.type == TokenType::StringLiteral ? current_.value
                                                                            : current_.lexeme);
      Advance();
    }

    method->number = static_cast<double>(flags);
    if (At("(")) {
      NodePtr function = Make(NodeKind::FunctionExpression);
      function->string = method->string;
      function->number = static_cast<double>(
          ((flags & kMethodAsync) != 0 ? kFunctionAsync : 0) |
          ((flags & kMethodGenerator) != 0 ? kFunctionGenerator : 0));
      function->children.push_back(ParseParameters());
      function->children.push_back(ParseBlock());
      method->children.push_back(std::move(function));
    } else {
      // A field. Its initializer is the method's only child, which keeps a
      // class body one list rather than two.
      method->children.push_back(Eat("=") ? ParseAssignment() : nullptr);
      ConsumeSemicolon();
    }
    node->children.push_back(std::move(method));
  }
  Expect("}", "to close a class body");
  return node;
}

NodePtr ParserImpl::ParseBlock() {
  NodePtr node = Make(NodeKind::Block);
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("blocks nest too deeply");
    // Consumes a token. Returning without one leaves the caller's statement
    // loop looking at the same `{`, which calls straight back in, hits the same
    // limit, and appends an empty block forever -- an out-of-memory from a few
    // hundred bytes of `{{{{`. Found by the fuzzer.
    Advance();
    return node;
  }
  Expect("{", "to open a block");
  std::size_t last_offset = std::string_view::npos;
  while (!At("}") && !AtEnd()) {
    // The same no-progress guard the program loop has. A recovery path that
    // consumes nothing is an infinite loop, and malformed input is the normal
    // case for something a page serves.
    if (current_.start == last_offset) {
      Advance();
      continue;
    }
    last_offset = current_.start;
    node->children.push_back(ParseStatement());
  }
  Expect("}", "to close a block");
  return node;
}

NodePtr ParserImpl::ParseVariableDeclaration(bool eat_semicolon) {
  NodePtr node = Make(NodeKind::VariableDeclaration);
  node->string = std::string(current_.lexeme);
  Advance();
  if (AtEnd()) {
    // `var` with nothing after it. Without this the loop below simply does not
    // run and a declaration with no declarators parses clean.
    Error("expected a binding name");
  }
  while (!AtEnd()) {
    NodePtr declarator = Make(NodeKind::Declarator);
    declarator->children.push_back(ParseBindingTarget());
    declarator->children.push_back(Eat("=") ? ParseAssignment() : nullptr);
    node->children.push_back(std::move(declarator));
    if (!Eat(",")) {
      break;
    }
  }
  if (eat_semicolon) {
    ConsumeSemicolon();
  }
  return node;
}

NodePtr ParserImpl::ParseIf() {
  NodePtr node = Make(NodeKind::If);
  Advance();
  Expect("(", "after 'if'");
  node->children.push_back(ParseExpression());
  Expect(")", "after an if condition");
  node->children.push_back(ParseStatement());
  node->children.push_back(EatKeyword("else") ? ParseStatement() : nullptr);
  return node;
}

NodePtr ParserImpl::ParseFor() {
  const std::size_t start = current_.start;
  const std::size_t line = current_.line;
  Advance();  // `for`
  // `for await (... of ...)`. EatKeyword rather than Eat: `await` is a reserved
  // word and so lexes as a keyword, and asking Eat for it compared against the
  // punctuators and never matched -- which made `for await` a syntax error
  // rather than the loop it is.
  const bool is_await = EatKeyword("await");
  Expect("(", "after 'for'");

  NodePtr init;
  if (At(";")) {
    // No initializer.
  } else if (AtKeyword("var") || AtKeyword("let") || AtKeyword("const")) {
    init = ParseVariableDeclaration(false);
  } else {
    init = ParseExpression();
  }

  if (AtKeyword("in") || (current_.type == TokenType::Identifier && current_.lexeme == "of")) {
    NodePtr node = std::make_unique<Node>();
    node->kind = NodeKind::ForIn;
    node->start = start;
    node->line = line;
    node->string = std::string(current_.lexeme);
    node->number = is_await ? 1.0 : 0.0;
    Advance();
    node->children.push_back(std::move(init));
    node->children.push_back(ParseAssignment());
    Expect(")", "after a for-in head");
    node->children.push_back(ParseStatement());
    return node;
  }

  NodePtr node = std::make_unique<Node>();
  node->kind = NodeKind::For;
  node->start = start;
  node->line = line;
  node->children.push_back(std::move(init));
  Expect(";", "after a for initializer");
  node->children.push_back(At(";") ? nullptr : ParseExpression());
  Expect(";", "after a for condition");
  node->children.push_back(At(")") ? nullptr : ParseExpression());
  Expect(")", "after a for head");
  node->children.push_back(ParseStatement());
  return node;
}

NodePtr ParserImpl::ParseWhile() {
  NodePtr node = Make(NodeKind::While);
  Advance();
  Expect("(", "after 'while'");
  node->children.push_back(ParseExpression());
  Expect(")", "after a while condition");
  node->children.push_back(ParseStatement());
  return node;
}

NodePtr ParserImpl::ParseDoWhile() {
  NodePtr node = Make(NodeKind::DoWhile);
  Advance();
  node->children.push_back(ParseStatement());
  if (!EatKeyword("while")) {
    Error("expected 'while' after a do body");
  }
  Expect("(", "after 'while'");
  node->children.push_back(ParseExpression());
  Expect(")", "after a while condition");
  Eat(";");  // optional, and inserted automatically when absent
  return node;
}

NodePtr ParserImpl::ParseReturn() {
  NodePtr node = Make(NodeKind::Return);
  Advance();
  // `return\nx` returns undefined. This is the ASI rule that surprises people,
  // and it is why the newline flag is on every token.
  if (At(";") || At("}") || AtEnd() || current_.newline_before) {
    node->children.push_back(nullptr);
  } else {
    node->children.push_back(ParseExpression());
  }
  ConsumeSemicolon();
  return node;
}

NodePtr ParserImpl::ParseThrow() {
  NodePtr node = Make(NodeKind::Throw);
  Advance();
  if (current_.newline_before) {
    // Unlike `return`, a newline after `throw` is a syntax error: there is no
    // "throw nothing".
    Error("no line terminator is allowed after 'throw'");
  }
  node->children.push_back(ParseExpression());
  ConsumeSemicolon();
  return node;
}

NodePtr ParserImpl::ParseTry() {
  NodePtr node = Make(NodeKind::Try);
  Advance();
  node->children.push_back(ParseBlock());

  if (EatKeyword("catch")) {
    if (Eat("(")) {
      node->children.push_back(ParseBindingTarget());
      Expect(")", "after a catch parameter");
    } else {
      node->children.push_back(nullptr);  // optional catch binding
    }
    node->children.push_back(ParseBlock());
  } else {
    node->children.push_back(nullptr);
    node->children.push_back(nullptr);
  }

  node->children.push_back(EatKeyword("finally") ? ParseBlock() : nullptr);
  if (node->Child(2) == nullptr && node->Child(3) == nullptr) {
    Error("a try needs a catch or a finally");
  }
  return node;
}

NodePtr ParserImpl::ParseSwitch() {
  NodePtr node = Make(NodeKind::Switch);
  Advance();
  Expect("(", "after 'switch'");
  node->children.push_back(ParseExpression());
  Expect(")", "after a switch discriminant");
  Expect("{", "to open a switch body");

  while (!At("}") && !AtEnd()) {
    NodePtr clause = Make(NodeKind::SwitchCase);
    if (EatKeyword("case")) {
      clause->children.push_back(ParseExpression());
    } else if (EatKeyword("default")) {
      clause->children.push_back(nullptr);
    } else {
      Error("expected 'case' or 'default'");
      Advance();
      continue;
    }
    Expect(":", "after a case label");
    std::size_t last_offset = std::string_view::npos;
    while (!At("}") && !AtKeyword("case") && !AtKeyword("default") && !AtEnd()) {
      if (current_.start == last_offset) {
        Advance();
        continue;
      }
      last_offset = current_.start;
      clause->children.push_back(ParseStatement());
    }
    node->children.push_back(std::move(clause));
  }
  Expect("}", "to close a switch body");
  return node;
}

NodePtr ParserImpl::ParseStatement() {
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("statements nest too deeply");
    Advance();
    return Make(NodeKind::Empty);
  }

  if (At("{")) {
    return ParseBlock();
  }
  if (At(";")) {
    NodePtr node = Make(NodeKind::Empty);
    Advance();
    return node;
  }
  if (AtKeyword("var") || AtKeyword("const")) {
    return ParseVariableDeclaration(true);
  }
  if (AtKeyword("let")) {
    // `let` is only a declaration when a binding follows. `let = 1` and
    // `let[0]` are expressions, and the difference is one token of lookahead.
    const Token saved = current_;
    Advance();
    const bool declares = current_.type == TokenType::Identifier || At("[") || At("{");
    lexer_.SeekTo(saved.end, saved.line);
    current_ = saved;
    if (declares) {
      return ParseVariableDeclaration(true);
    }
  }
  if (current_.type == TokenType::Identifier && current_.lexeme == "async") {
    // `async function f(){}` in statement position. Nothing else that starts
    // with the identifier `async` is a declaration, so a rewind that finds no
    // `function` leaves it to be parsed as the expression it is.
    const Token saved = current_;
    Advance();
    if (AtKeyword("function") && !current_.newline_before) {
      NodePtr node = ParseFunction(true);
      if (node != nullptr) {
        // Or-ed rather than assigned: ParseFunction has already recorded the
        // star, and `async function*` is both.
        node->number = static_cast<double>(static_cast<std::uint8_t>(node->number) | kFunctionAsync);
      }
      return node;
    }
    lexer_.SeekTo(saved.end, saved.line);
    current_ = saved;
  }
  if (AtKeyword("function")) {
    return ParseFunction(true);
  }
  if (AtKeyword("class")) {
    return ParseClass(true);
  }
  if (AtKeyword("if")) {
    return ParseIf();
  }
  if (AtKeyword("for")) {
    return ParseFor();
  }
  if (AtKeyword("while")) {
    return ParseWhile();
  }
  if (AtKeyword("do")) {
    return ParseDoWhile();
  }
  if (AtKeyword("return")) {
    return ParseReturn();
  }
  if (AtKeyword("throw")) {
    return ParseThrow();
  }
  if (AtKeyword("try")) {
    return ParseTry();
  }
  if (AtKeyword("switch")) {
    return ParseSwitch();
  }
  if (AtKeyword("break") || AtKeyword("continue")) {
    NodePtr node = Make(AtKeyword("break") ? NodeKind::Break : NodeKind::Continue);
    Advance();
    // A label on the same line only: `break\nfoo` breaks, then evaluates foo.
    if (current_.type == TokenType::Identifier && !current_.newline_before) {
      node->string = std::string(current_.lexeme);
      Advance();
    }
    ConsumeSemicolon();
    return node;
  }
  if (AtKeyword("debugger")) {
    NodePtr node = Make(NodeKind::Debugger);
    Advance();
    ConsumeSemicolon();
    return node;
  }

  // A labelled statement: an identifier followed by a colon.
  if (current_.type == TokenType::Identifier) {
    const Token saved = current_;
    Advance();
    if (At(":")) {
      Advance();
      NodePtr node = std::make_unique<Node>();
      node->kind = NodeKind::Labeled;
      node->string = std::string(saved.lexeme);
      node->start = saved.start;
      node->line = saved.line;
      node->children.push_back(ParseStatement());
      return node;
    }
    lexer_.SeekTo(saved.end, saved.line);
    current_ = saved;
  }

  NodePtr node = Make(NodeKind::ExpressionStatement);
  node->children.push_back(ParseExpression());
  ConsumeSemicolon();
  return node;
}

ParseResult ParserImpl::ParseProgram() {
  NodePtr program = std::make_unique<Node>();
  program->kind = NodeKind::Program;

  std::size_t last_offset = std::string_view::npos;
  while (!AtEnd()) {
    // No-progress guard. A recovery path that consumed nothing would loop
    // forever on malformed input, and malformed input is the normal case for
    // something a page serves.
    if (current_.start == last_offset) {
      Advance();
      continue;
    }
    last_offset = current_.start;
    program->children.push_back(ParseStatement());
    if (errors_.size() >= 32) {
      break;
    }
  }

  ParseResult result;
  result.program = std::move(program);
  result.errors = std::move(errors_);
  return result;
}

ParseResult ParserImpl::ParseExpressionSource() {
  NodePtr program = std::make_unique<Node>();
  program->kind = NodeKind::Program;
  if (!AtEnd()) {
    // The comma operator included: `${a, b}` is one expression, not two.
    program->children.push_back(ParseExpression());
  }
  if (!AtEnd()) {
    // Trailing tokens are an error rather than silently ignored -- `${a b}` is
    // not an expression, and dropping the `b` would run something the page did
    // not write.
    Error("unexpected token '" + std::string(current_.lexeme) + "'");
  }

  ParseResult result;
  result.program = std::move(program);
  result.errors = std::move(errors_);
  return result;
}

}  // namespace microbrowser::js

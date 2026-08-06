#include "js/ParserImpl.h"

#include "js/TemplateParts.h"

#include <algorithm>
#include <array>
#include <utility>

namespace microbrowser::js {

namespace {

// Binary operator precedence, per the grammar. `**` is right-associative and
// every other level is left-associative, which is the only irregularity here
// and the reason the table carries associativity rather than assuming it.
struct BinaryOp {
  std::string_view text;
  int precedence;
  bool right_associative;
};

constexpr std::array<BinaryOp, 24> kBinaryOps = {{
    {"??", 1, false},   {"||", 2, false},  {"&&", 3, false},  {"|", 4, false},
    {"^", 5, false},    {"&", 6, false},   {"==", 7, false},  {"!=", 7, false},
    {"===", 7, false},  {"!==", 7, false}, {"<", 8, false},   {">", 8, false},
    {"<=", 8, false},   {">=", 8, false},  {"<<", 9, false},  {">>", 9, false},
    {">>>", 9, false},  {"+", 10, false},  {"-", 10, false},  {"*", 11, false},
    {"/", 11, false},   {"%", 11, false},  {"**", 12, true},  {"__unused", 0, false},
}};

constexpr std::array<std::string_view, 13> kAssignmentOps = {
    "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "**=",
};

// Logical assignment is spelled with escapes because `??=` is a trigraph in a
// C++ source file.
constexpr std::string_view kLogicalAssignOps[] = {"&&=", "||=", "?\?="};

bool IsAssignmentOperator(std::string_view text) {
  return std::find(kAssignmentOps.begin(), kAssignmentOps.end(), text) != kAssignmentOps.end() ||
         std::find(std::begin(kLogicalAssignOps), std::end(kLogicalAssignOps), text) !=
             std::end(kLogicalAssignOps);
}

const BinaryOp* FindBinaryOp(std::string_view text) {
  for (const BinaryOp& op : kBinaryOps) {
    if (op.text == text) {
      return &op;
    }
  }
  return nullptr;
}

}  // namespace

void ParserImpl::Advance() {
  current_ = lexer_.Next();
  if (current_.type == TokenType::Invalid) {
    Error("invalid token");
  }
}

void ParserImpl::Error(std::string message) {
  // Bounded: a parser that recovers badly can otherwise produce an error per
  // token, and a megabyte of script becomes a megabyte of diagnostics.
  constexpr std::size_t kMaxErrors = 32;
  if (errors_.size() < kMaxErrors) {
    errors_.push_back(ParseError{std::move(message), current_.start, current_.line});
  }
}

bool ParserImpl::Eat(std::string_view punctuator) {
  if (!At(punctuator)) {
    return false;
  }
  Advance();
  return true;
}

bool ParserImpl::EatKeyword(std::string_view keyword) {
  if (!AtKeyword(keyword)) {
    return false;
  }
  Advance();
  return true;
}

bool ParserImpl::Expect(std::string_view punctuator, std::string_view context) {
  if (Eat(punctuator)) {
    return true;
  }
  Error("expected '" + std::string(punctuator) + "' " + std::string(context));
  return false;
}

bool ParserImpl::ConsumeSemicolon() {
  if (Eat(";")) {
    return true;
  }
  // The three places a semicolon is inserted: before a `}`, at end of input,
  // and before a token that began a new line. Written out rather than folded
  // together because they are three separate rules in the spec and each has
  // been got wrong by somebody.
  if (At("}") || AtEnd() || current_.newline_before) {
    return true;
  }
  Error("expected ';'");
  return false;
}

void ParserImpl::InferName(Node* value, std::string_view name) {
  if (value == nullptr || name.empty() || !value->string.empty()) {
    return;  // already named: `const f = function g(){}` is still `g`
  }
  switch (value->kind) {
    case NodeKind::FunctionExpression:
    case NodeKind::ArrowFunction:
    case NodeKind::ClassExpression:
      value->string = std::string(name);
      return;
    default:
      return;
  }
}

NodePtr ParserImpl::Make(NodeKind kind) const {
  auto node = std::make_unique<Node>();
  node->kind = kind;
  node->start = current_.start;
  node->line = current_.line;
  return node;
}

// --- Expressions -----------------------------------------------------------

NodePtr ParserImpl::ParsePrimary() {
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("expression nests too deeply");
    Advance();
    return Make(NodeKind::Empty);
  }

  if (current_.type == TokenType::NumericLiteral) {
    NodePtr node = Make(NodeKind::NumberLiteral);
    node->number = current_.number;
    Advance();
    return node;
  }
  if (current_.type == TokenType::BigIntLiteral) {
    NodePtr node = Make(NodeKind::BigIntLiteral);
    node->string = current_.value;
    Advance();
    return node;
  }
  if (current_.type == TokenType::StringLiteral) {
    NodePtr node = Make(NodeKind::StringLiteral);
    node->string = current_.value;
    Advance();
    return node;
  }
  if (current_.type == TokenType::TemplateString) {
    return ParseTemplate();
  }
  if (current_.type == TokenType::Identifier && current_.lexeme == "async") {
    if (NodePtr node = ParseAsyncExpression()) {
      return node;
    }
    // Not a modifier after all. `async` is a contextual keyword, so `async(1)`
    // is a call and `async = 1` is an assignment, and both have to keep
    // working -- which is why this asks rather than assumes.
  }
  if (current_.type == TokenType::PrivateIdentifier) {
    // A private name reaching here is the brand check -- `#x in o`. Every
    // other position for one is after a `.`, which the member path reads
    // straight off the token. Private state is stored under the written name,
    // so the check is a property test and the name is the string form of it.
    NodePtr node = Make(NodeKind::StringLiteral);
    node->string = std::string(current_.lexeme);
    Advance();
    return node;
  }
  if (current_.type == TokenType::Identifier) {
    NodePtr node = Make(NodeKind::Identifier);
    node->string = std::string(current_.lexeme);
    Advance();
    return node;
  }
  if (AtKeyword("true") || AtKeyword("false")) {
    NodePtr node = Make(NodeKind::BooleanLiteral);
    node->number = current_.lexeme == "true" ? 1.0 : 0.0;
    Advance();
    return node;
  }
  if (EatKeyword("null")) {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::NullLiteral;
    return node;
  }
  if (AtKeyword("import")) {
    Advance();
    if (Eat(".")) {
      if (current_.lexeme != "meta") {
        Error("expected 'meta' after 'import.'");
      } else {
        Advance();
      }
      return Make(NodeKind::ImportMeta);
    }
    // `import(spec)`. A call rather than a declaration: it resolves at run
    // time and answers a promise, which is what makes a lazily loaded chunk
    // possible.
    NodePtr node = Make(NodeKind::ImportCall);
    Expect("(", "after 'import'");
    node->children.push_back(ParseAssignment());
    Expect(")", "to close a dynamic import");
    return node;
  }
  if (AtKeyword("this")) {
    NodePtr node = Make(NodeKind::ThisExpression);
    Advance();
    return node;
  }
  if (AtKeyword("super")) {
    // Whether a `super` is in a position that allows it is a scope question,
    // not a syntax one, so the parser accepts it and the checker rejects it.
    NodePtr node = Make(NodeKind::Super);
    Advance();
    return node;
  }
  if (AtKeyword("let")) {
    // `let` is a reserved word only where a declaration could start. Everywhere
    // else it is an identifier, and `let = 1` is a program people have written.
    NodePtr node = Make(NodeKind::Identifier);
    node->string = std::string(current_.lexeme);
    Advance();
    return node;
  }
  if (AtKeyword("function")) {
    return ParseFunction(false);
  }
  if (AtKeyword("class")) {
    return ParseClass(false);
  }
  if (At("[")) {
    return ParseArrayLiteral();
  }
  if (At("{")) {
    return ParseObjectLiteral();
  }
  if (At("(")) {
    return ParseArrowFromParenthesised();
  }
  if (At("/") || At("/=")) {
    // A value is expected here, so the slash begins a regular expression. This
    // is the one place the lexer's ambiguity is resolved, and it is resolved by
    // the grammar rather than by guessing from the previous token.
    const Token regex = lexer_.RescanAsRegExp(current_);
    if (regex.type != TokenType::RegExpLiteral) {
      Error("unterminated regular expression");
      Advance();
      return Make(NodeKind::Empty);
    }
    NodePtr node = Make(NodeKind::RegExpLiteral);
    node->string = std::string(regex.lexeme);
    lexer_.SeekTo(regex.end, regex.line);
    Advance();
    return node;
  }

  Error("unexpected token '" + std::string(current_.lexeme) + "'");
  Advance();
  return Make(NodeKind::Empty);
}

NodePtr ParserImpl::ParseTemplate() {
  NodePtr node = Make(NodeKind::TemplateLiteral);
  node->string = std::string(current_.lexeme);
  // Substitutions are re-parsed from the raw text, which keeps the nesting
  // rules in one place: a template inside a substitution inside a template is
  // handled by recursion rather than by a second brace counter.
  for (const std::string_view substitution : SplitTemplate(current_.lexeme).substitutions) {
    ParserImpl inner_parser(substitution);
    ParseResult inner = inner_parser.ParseExpressionSource();
    // One child per substitution, position included -- a null for an empty
    // `${}` rather than a gap, because the interpreter pairs children with
    // literal chunks by index and a missing one would shift every later
    // substitution into the wrong slot.
    node->children.push_back(inner.program != nullptr && !inner.program->children.empty()
                                 ? std::move(inner.program->children.front())
                                 : nullptr);
    for (ParseError& error : inner.errors) {
      Error(std::move(error.message));
    }
  }
  Advance();
  return node;
}

NodePtr ParserImpl::ParseArrayLiteral() {
  NodePtr node = Make(NodeKind::ArrayLiteral);
  Expect("[", "to open an array literal");
  while (!At("]") && !AtEnd()) {
    if (At(",")) {
      // A hole. Kept as a null child rather than skipped, because `[1,,3]` has
      // three elements and dropping the middle one changes the length.
      node->children.push_back(nullptr);
      Advance();
      continue;
    }
    if (At("...")) {
      NodePtr spread = Make(NodeKind::Spread);
      Advance();
      spread->children.push_back(ParseAssignment());
      node->children.push_back(std::move(spread));
    } else {
      node->children.push_back(ParseAssignment());
    }
    if (!At("]") && !Eat(",")) {
      Error("expected ',' between array elements");
      break;
    }
  }
  Expect("]", "to close an array literal");
  return node;
}

NodePtr ParserImpl::ParseObjectLiteral() {
  NodePtr node = Make(NodeKind::ObjectLiteral);
  Expect("{", "to open an object literal");
  while (!At("}") && !AtEnd()) {
    NodePtr property = Make(NodeKind::Property);
    if (At("...")) {
      NodePtr spread = Make(NodeKind::Spread);
      Advance();
      spread->children.push_back(ParseAssignment());
      node->children.push_back(std::move(spread));
      if (!At("}") && !Eat(",")) {
        break;
      }
      continue;
    }

    // `get x(){}` and `set x(v){}`. Detected the same way the class body
    // detects them: `get` and `set` are ordinary identifiers, so a property
    // *named* `get` has to still work, and the only way to tell is to read the
    // next token and put it back.
    // `async m(){}`, detected the same way and put back the same way, so a
    // property *named* `async` keeps working.
    bool is_async = false;
    if (current_.lexeme == "async" && current_.type == TokenType::Identifier) {
      const Token saved = current_;
      Advance();
      if (At(":") || At("(") || At(",") || At("}") || At("=") || current_.newline_before) {
        lexer_.SeekTo(saved.end, saved.line);
        current_ = saved;
      } else {
        is_async = true;
      }
    }

    // `*m(){}` and `async *m(){}`. No lookahead needed: `*` cannot start a
    // property name, so seeing one here is unambiguous.
    const bool is_generator = Eat("*");

    bool getter = false;
    bool setter = false;
    if ((current_.lexeme == "get" || current_.lexeme == "set") &&
        current_.type == TokenType::Identifier) {
      const bool is_getter = current_.lexeme == "get";
      const Token saved = current_;
      Advance();
      if (At(":") || At("(") || At(",") || At("}") || At("=")) {
        lexer_.SeekTo(saved.end, saved.line);
        current_ = saved;
      } else {
        getter = is_getter;
        setter = !is_getter;
      }
    }

    bool computed = false;
    NodePtr computed_key;
    if (At("[")) {
      computed = true;
      Advance();
      computed_key = ParseAssignment();
      Expect("]", "to close a computed property name");
    } else if (current_.type == TokenType::StringLiteral) {
      property->string = current_.value;
      Advance();
    } else if (current_.type == TokenType::NumericLiteral) {
      property->string = std::string(current_.lexeme);
      Advance();
    } else if (current_.type == TokenType::Identifier || current_.type == TokenType::Keyword) {
      // A keyword is a legal property name: `{ if: 1 }` and `x.class` are both
      // fine, which is why keywords are not filtered out here.
      property->string = std::string(current_.lexeme);
      Advance();
    } else {
      Error("expected a property name");
      Advance();
      continue;
    }

    if (getter || setter) {
      // An accessor is a method whose value happens to be read or written
      // rather than called, so it parses as one and is marked.
      NodePtr function = Make(NodeKind::FunctionExpression);
      function->string = property->string;
      function->children.push_back(ParseParameters());
      function->children.push_back(ParseBlock());
      property->children.push_back(std::move(function));
    } else if (Eat(":")) {
      property->children.push_back(ParseAssignment());
      // `{ handler: () => {} }` names the arrow `handler`, which is what makes
      // a stack trace through an options object readable.
      if (!computed) {
        InferName(property->children.back().get(), property->string);
      }
    } else if (At("(")) {
      // A method. Represented as a plain property whose value is a function, so
      // that a consumer walking an object literal has one shape to handle.
      NodePtr function = Make(NodeKind::FunctionExpression);
      function->string = property->string;
      function->number = static_cast<double>((is_async ? kFunctionAsync : 0) |
                                             (is_generator ? kFunctionGenerator : 0));
      function->children.push_back(ParseParameters());
      function->children.push_back(ParseBlock());
      property->children.push_back(std::move(function));
    } else {
      // Shorthand: `{ x }`. Expanded here rather than in every consumer.
      NodePtr identifier = Make(NodeKind::Identifier);
      identifier->string = property->string;
      if (Eat("=")) {
        // `{ x = 1 }` is only legal as a destructuring pattern. Parsed so the
        // shape survives; whether the position allows it is a later check.
        NodePtr pattern = Make(NodeKind::AssignmentPattern);
        pattern->children.push_back(std::move(identifier));
        pattern->children.push_back(ParseAssignment());
        property->children.push_back(std::move(pattern));
      } else {
        property->children.push_back(std::move(identifier));
      }
    }
    // A bitfield, the way a class member's flags are: computed, getter and
    // setter are three independent facts about one property and `{ get [k]()
    // {} }` is all of them at once.
    property->number = (computed ? 1.0 : 0.0) + (getter ? 2.0 : 0.0) + (setter ? 4.0 : 0.0);
    if (computed) {
      property->children.push_back(std::move(computed_key));
    }
    node->children.push_back(std::move(property));

    if (!At("}") && !Eat(",")) {
      Error("expected ',' between object properties");
      break;
    }
  }
  Expect("}", "to close an object literal");
  return node;
}

// Reinterprets an already-parsed expression as an arrow function's parameter
// list.
//
// `(a, b) => x` and `(a, b)` are the same tokens until the arrow, and the
// obvious implementation -- parse as an expression, and if an arrow follows,
// rewind and parse again as parameters -- is exponential: every nesting level
// parses its contents twice, so `((((((x))))))` costs 2^n. A page serving a
// hundred bytes of nested parentheses would hang the parser. Found by the
// fuzzer as an out-of-memory.
//
// Converting the tree instead is linear. The conversion is total: anything that
// cannot be a binding target becomes an error rather than a silently wrong
// parameter.
bool ParserImpl::ExpressionToParameters(NodePtr expression, Node& out) {
  if (expression == nullptr) {
    return false;
  }
  if (expression->kind == NodeKind::Sequence) {
    for (NodePtr& element : expression->children) {
      if (!ExpressionToParameters(std::move(element), out)) {
        return false;
      }
    }
    return true;
  }

  switch (expression->kind) {
    case NodeKind::Identifier:
    case NodeKind::ArrayLiteral:   // an array destructuring pattern
    case NodeKind::ObjectLiteral:  // an object destructuring pattern
      out.children.push_back(std::move(expression));
      return true;
    case NodeKind::Assignment:
      if (expression->string != "=") {
        return false;  // `(a += 1) => x` is not a default value
      }
      expression->kind = NodeKind::AssignmentPattern;
      expression->string.clear();
      out.children.push_back(std::move(expression));
      return true;
    case NodeKind::Spread:
      expression->kind = NodeKind::RestElement;
      out.children.push_back(std::move(expression));
      return true;
    default:
      return false;
  }
}

NodePtr ParserImpl::ParseAsyncExpression() {
  // `async` is a contextual keyword: it modifies what follows it only when what
  // follows it is a function, and is an ordinary identifier otherwise. So this
  // reads ahead and puts the token back when it was not a modifier, which is
  // the same shape the getter/setter case in an object literal has.
  //
  // Null means "not a modifier here" and leaves the parser exactly where it
  // was, including its error list -- a speculative parse that failed must not
  // leave its complaints behind for the successful reading to carry.
  const Token saved = current_;
  const std::size_t errors_before = errors_.size();
  const auto rewind = [&] {
    lexer_.SeekTo(saved.end, saved.line);
    current_ = saved;
    errors_.resize(errors_before);
  };

  Advance();
  // `async` and what it modifies cannot be separated by a line terminator: ASI
  // would otherwise turn `async\nfunction f(){}` into two statements, and the
  // spec says it does.
  if (current_.newline_before) {
    rewind();
    return nullptr;
  }
  if (AtKeyword("function")) {
    NodePtr node = ParseFunction(false);
    if (node != nullptr) {
      // Or-ed rather than assigned: ParseFunction has already recorded the
      // star, and `async function*` is both.
      node->number = static_cast<double>(static_cast<std::uint8_t>(node->number) | kFunctionAsync);
    }
    return node;
  }
  if (current_.type == TokenType::Identifier) {
    // `async x => ...`, the one-parameter form with no parentheses.
    NodePtr parameter = Make(NodeKind::Identifier);
    parameter->string = std::string(current_.lexeme);
    Advance();
    if (!At("=>") || current_.newline_before) {
      rewind();
      return nullptr;
    }
    Advance();
    NodePtr arrow = Make(NodeKind::ArrowFunction);
    arrow->number = static_cast<double>(kFunctionAsync);
    NodePtr parameters = Make(NodeKind::Parameters);
    parameters->children.push_back(std::move(parameter));
    arrow->children.push_back(std::move(parameters));
    arrow->children.push_back(At("{") ? ParseBlock() : ParseAssignment());
    return arrow;
  }
  if (At("(") && ArrowFollowsParentheses()) {
    // `async (a, b) => ...`. Decided by scanning to the matching `)` rather
    // than by parsing what is inside it and putting it back: `async(x)` is a
    // call, so a speculative parse would parse the contents twice, and
    // `async(async(async(x)))` would parse them 2^n times. Measured before
    // this was a scan: 127 bytes of nested `async(` took two seconds, which is
    // a hang a page can serve. It is the same trap ExpressionToParameters
    // exists to avoid, one production over.
    NodePtr parsed = ParseArrowFromParenthesised();
    if (parsed != nullptr && parsed->kind == NodeKind::ArrowFunction) {
      parsed->number = static_cast<double>(kFunctionAsync);
      return parsed;
    }
    // The scan said arrow and the parse disagreed, which the bracket counter
    // can be talked into by a regular expression literal holding a bracket.
    // One wasted parse, and not one that nests.
    rewind();
    return nullptr;
  }
  rewind();
  return nullptr;
}

bool ParserImpl::ArrowFollowsParentheses() {
  // Bounded, because the scan is what keeps this linear and an unbounded one
  // inside a bounded nesting is still quadratic. A parameter list longer than
  // this is not something a page writes, and reading it as a call rather than
  // as an arrow is a syntax error rather than a wrong program.
  constexpr int kMaxScannedTokens = 2048;

  const Token saved = current_;
  const std::size_t errors_before = errors_.size();
  int depth = 0;
  bool arrow = false;
  for (int scanned = 0; scanned < kMaxScannedTokens && !AtEnd(); ++scanned) {
    if (At("(") || At("[") || At("{")) {
      ++depth;
    } else if (At(")") || At("]") || At("}")) {
      --depth;
      if (depth == 0) {
        Advance();
        arrow = At("=>") && !current_.newline_before;
        break;
      }
      if (depth < 0) {
        break;
      }
    }
    Advance();
  }
  lexer_.SeekTo(saved.end, saved.line);
  current_ = saved;
  errors_.resize(errors_before);
  return arrow;
}

NodePtr ParserImpl::ParseArrowFromParenthesised() {
  Expect("(", "to open a parenthesised expression");

  if (Eat(")")) {
    // `()` is only legal as an empty parameter list.
    if (At("=>") && !current_.newline_before) {
      Advance();
      NodePtr arrow = Make(NodeKind::ArrowFunction);
      arrow->children.push_back(Make(NodeKind::Parameters));
      arrow->children.push_back(At("{") ? ParseBlock() : ParseAssignment());
      return arrow;
    }
    Error("expected an expression");
    return Make(NodeKind::Empty);
  }

  // A trailing comma is legal in a parameter list and not in a parenthesised
  // expression, and a rest element only in the first -- so both are parsed here
  // and rejected below if no arrow follows.
  NodePtr inner;
  bool saw_rest = false;
  bool saw_trailing_comma = false;
  if (At("...")) {
    NodePtr spread = Make(NodeKind::Spread);
    Advance();
    spread->children.push_back(ParseBindingTarget());
    inner = std::move(spread);
    saw_rest = true;
  } else {
    // A comma-separated list parsed here rather than through ParseExpression,
    // because a trailing comma is legal in a parameter list and not in a
    // parenthesised expression -- and ParseExpression would consume it and then
    // demand an operand.
    NodePtr first = ParseAssignment();
    if (!At(",")) {
      inner = std::move(first);
    } else {
      NodePtr sequence = Make(NodeKind::Sequence);
      sequence->children.push_back(std::move(first));
      while (Eat(",")) {
        if (At(")")) {
          saw_trailing_comma = true;
          break;
        }
        if (At("...")) {
          NodePtr spread = Make(NodeKind::Spread);
          Advance();
          spread->children.push_back(ParseBindingTarget());
          sequence->children.push_back(std::move(spread));
          saw_rest = true;
          break;
        }
        sequence->children.push_back(ParseAssignment());
      }
      inner = std::move(sequence);
    }
  }
  Expect(")", "to close a parenthesised expression");

  // A line terminator before `=>` is a syntax error, and treating it as one
  // here means the expression stands rather than silently becoming a function.
  if (!At("=>") || current_.newline_before) {
    if (saw_rest || saw_trailing_comma) {
      Error("a rest element or trailing comma is only allowed in a parameter list");
    }
    return inner;
  }

  Advance();
  NodePtr arrow = Make(NodeKind::ArrowFunction);
  NodePtr parameters = Make(NodeKind::Parameters);
  if (!ExpressionToParameters(std::move(inner), *parameters)) {
    Error("this is not a valid parameter list");
  }
  arrow->children.push_back(std::move(parameters));
  arrow->children.push_back(At("{") ? ParseBlock() : ParseAssignment());
  return arrow;
}

void ParserImpl::ParseArguments(Node& call) {
  Expect("(", "to open an argument list");
  while (!At(")") && !AtEnd()) {
    if (At("...")) {
      NodePtr spread = Make(NodeKind::Spread);
      Advance();
      spread->children.push_back(ParseAssignment());
      call.children.push_back(std::move(spread));
    } else {
      call.children.push_back(ParseAssignment());
    }
    if (!At(")") && !Eat(",")) {
      Error("expected ',' between arguments");
      break;
    }
  }
  Expect(")", "to close an argument list");
}

NodePtr ParserImpl::ParseNew() {
  NodePtr node = Make(NodeKind::New);
  Advance();  // `new`
  if (At(".")) {
    // `new.target`. Not a construction at all -- it reads whether the running
    // call was reached through `new`, which nothing else can answer: the
    // receiver looks the same either way.
    Advance();
    if (current_.lexeme != "target") {
      Error("expected 'target' after 'new.'");
      return Make(NodeKind::Empty);
    }
    Advance();
    return Make(NodeKind::NewTarget);
  }
  // The callee of `new` excludes calls: `new a.b()` constructs a.b, and
  // `new a()()` calls the result of the construction.
  NodePtr callee = ParseCallOrMember(ParsePrimary(), false);
  node->children.push_back(std::move(callee));
  if (At("(")) {
    ParseArguments(*node);
  }
  return node;
}

NodePtr ParserImpl::ParseCallOrMember(NodePtr base, bool allow_call) {
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("expression nests too deeply");
    return base;
  }

  // Whether anything in this chain was written `?.`, which is what decides
  // where the short-circuit lands. `a?.b.c` gives up on the *whole* expression
  // when `a` is nullish -- so the outermost link is marked, and both engines
  // read the mark rather than tracking a chain of their own. Only here is the
  // surrounding syntax still available to say where the chain ends.
  bool chain_is_optional = false;

  while (!AtEnd()) {
    if (At(".") || At("?.")) {
      const bool optional = At("?.");
      chain_is_optional = chain_is_optional || optional;
      Advance();
      if (optional && At("(")) {
        if (!allow_call) {
          break;
        }
        NodePtr call = Make(NodeKind::Call);
        call->number = kCallOptional;
        call->children.push_back(std::move(base));
        ParseArguments(*call);
        base = std::move(call);
        continue;
      }
      if (optional && At("[")) {
        // `a?.[k]`. Computed *and* optional, which is why the flags are bits:
        // treating the two as alternatives is what made this a syntax error.
        Advance();
        NodePtr member = Make(NodeKind::Member);
        member->number = kMemberComputed | kMemberOptional;
        member->children.push_back(std::move(base));
        member->children.push_back(ParseExpression());
        Expect("]", "to close a computed member access");
        base = std::move(member);
        continue;
      }
      NodePtr member = Make(NodeKind::Member);
      member->number = optional ? kMemberOptional : kMemberPlain;
      member->children.push_back(std::move(base));
      NodePtr property = Make(NodeKind::Identifier);
      if (current_.type == TokenType::Identifier || current_.type == TokenType::Keyword ||
          current_.type == TokenType::PrivateIdentifier) {
        property->string = std::string(current_.lexeme);
        Advance();
      } else {
        Error("expected a property name after '.'");
      }
      member->children.push_back(std::move(property));
      base = std::move(member);
      continue;
    }
    if (At("[")) {
      Advance();
      NodePtr member = Make(NodeKind::Member);
      member->number = kMemberComputed;
      member->children.push_back(std::move(base));
      member->children.push_back(ParseExpression());
      Expect("]", "to close a computed member access");
      base = std::move(member);
      continue;
    }
    if (allow_call && At("(")) {
      NodePtr call = Make(NodeKind::Call);
      call->children.push_back(std::move(base));
      ParseArguments(*call);
      base = std::move(call);
      continue;
    }
    if (current_.type == TokenType::TemplateString) {
      NodePtr tagged = Make(NodeKind::TaggedTemplate);
      tagged->children.push_back(std::move(base));
      tagged->children.push_back(ParseTemplate());
      base = std::move(tagged);
      continue;
    }
    break;
  }
  if (chain_is_optional && base != nullptr) {
    if (base->kind == NodeKind::Member) {
      base->number = static_cast<double>(static_cast<std::uint8_t>(base->number) |
                                         kMemberChainRoot);
    } else if (base->kind == NodeKind::Call) {
      base->number =
          static_cast<double>(static_cast<std::uint8_t>(base->number) | kCallChainRoot);
    }
  }
  return base;
}

NodePtr ParserImpl::ParsePostfix() {
  // `new Foo(1).bar()` constructs, then accesses, then calls. ParseNew stops at
  // the construction because the callee of `new` excludes calls; the rest of
  // the chain is ordinary member and call syntax and has to continue here.
  NodePtr expression = AtKeyword("new") ? ParseCallOrMember(ParseNew(), true)
                                        : ParseCallOrMember(ParsePrimary(), true);
  if ((At("++") || At("--")) && !current_.newline_before) {
    // A line terminator before a postfix operator means it belongs to the next
    // statement -- ASI again, and the reason `a\n++b` is two statements.
    NodePtr update = Make(NodeKind::Update);
    update->string = std::string(current_.lexeme);
    update->number = 0.0;  // postfix
    update->children.push_back(std::move(expression));
    Advance();
    return update;
  }
  return expression;
}

NodePtr ParserImpl::ParseUnary() {
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("expression nests too deeply");
    Advance();
    return Make(NodeKind::Empty);
  }

  if (At("!") || At("~") || At("+") || At("-") || AtKeyword("typeof") || AtKeyword("void") ||
      AtKeyword("delete") || AtKeyword("await")) {
    NodePtr node = Make(NodeKind::Unary);
    node->string = std::string(current_.lexeme);
    Advance();
    node->children.push_back(ParseUnary());
    return node;
  }
  if (At("++") || At("--")) {
    NodePtr node = Make(NodeKind::Update);
    node->string = std::string(current_.lexeme);
    node->number = 1.0;  // prefix
    Advance();
    node->children.push_back(ParseUnary());
    return node;
  }
  return ParsePostfix();
}

NodePtr ParserImpl::ParseBinary(int min_precedence) {
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("expression nests too deeply");
    Advance();
    return Make(NodeKind::Empty);
  }

  // `[~In]` binds at this level and nowhere below it: `for (a in b)` must not
  // read `in` as an operator, but `for (k = ('x' in o);;)` must. Taking the
  // flag and clearing it before ParseUnary descends gives both, because
  // everything nested -- parentheses, arguments, array and object literals --
  // is parsed with it already false.
  const bool no_in = no_in_;
  no_in_ = false;

  NodePtr left = ParseUnary();
  while (!AtEnd()) {
    std::string_view text = current_.lexeme;
    // `in` and `instanceof` are keywords that behave as binary operators. They
    // sit at the relational level, with `<` and friends.
    const bool keyword_operator = AtKeyword("in") || AtKeyword("instanceof");
    if (no_in && AtKeyword("in")) {
      break;  // the head of a for-in, and the caller's to consume
    }
    if (!keyword_operator && current_.type != TokenType::Punctuator) {
      break;
    }
    int precedence = 0;
    bool right_associative = false;
    if (keyword_operator) {
      precedence = 8;
    } else if (const BinaryOp* op = FindBinaryOp(text)) {
      precedence = op->precedence;
      right_associative = op->right_associative;
    } else {
      break;
    }
    if (precedence < min_precedence) {
      break;
    }

    NodePtr node = Make(text == "&&" || text == "||" || text == "?\?" ? NodeKind::Logical
                                                                     : NodeKind::Binary);
    node->string = std::string(text);
    Advance();
    node->children.push_back(std::move(left));
    node->children.push_back(ParseBinary(right_associative ? precedence : precedence + 1));
    left = std::move(node);
  }
  return left;
}

NodePtr ParserImpl::ParseConditional() {
  NodePtr test = ParseBinary(1);
  if (!At("?")) {
    return test;
  }
  NodePtr node = Make(NodeKind::Conditional);
  Advance();
  node->children.push_back(std::move(test));
  // The consequent is an AssignmentExpression, so `a ? b = 1 : c` works and the
  // comma operator does not leak in.
  node->children.push_back(ParseAssignment());
  Expect(":", "in a conditional expression");
  node->children.push_back(ParseAssignment());
  return node;
}

NodePtr ParserImpl::ParseAssignment() {
  const Depth depth(*this);
  if (depth.Exceeded()) {
    Error("expression nests too deeply");
    Advance();
    return Make(NodeKind::Empty);
  }

  // `yield`, which is an AssignmentExpression rather than a unary operator --
  // `yield a + b` yields the sum, and `x = yield v` assigns what was sent back.
  // Whether the enclosing function is a generator is not a question the parser
  // can answer cheaply, so it parses one anywhere and the compiler rejects one
  // outside a generator. That is exactly what `await` does one production over.
  if (AtKeyword("yield")) {
    NodePtr node = Make(NodeKind::Yield);
    Advance();
    // No line terminator between `yield` and its star: `yield\n* x` is two
    // statements under ASI, and the spec says so explicitly.
    const bool delegate = !current_.newline_before && Eat("*");
    node->number = delegate ? 1.0 : 0.0;
    // `yield` alone is legal and yields undefined, so an operand is only read
    // when one can actually start here. A delegating yield always has one.
    const bool has_operand = delegate || (!current_.newline_before && !AtEnd() && !At(")") &&
                                          !At("]") && !At("}") && !At(",") && !At(";") && !At(":"));
    node->children.push_back(has_operand ? ParseAssignment() : nullptr);
    return node;
  }

  // A single-identifier arrow: `x => x + 1`. Checked before the general path
  // because the identifier would otherwise be consumed as an expression.
  //
  // Put back the way every other lookahead here puts one back -- seek to the
  // end of the saved token and restore it -- rather than to its *start* and
  // lexing it again. This runs on every identifier in the program, which in a
  // minified bundle is most of the tokens in it, so re-lexing here was a second
  // pass over the source paid for by the one arrow function in ten thousand
  // identifiers that needed it. The discarded `std::string name` was the same
  // trade: an allocation per identifier, to hold a copy of a view into a source
  // buffer that outlives the whole parse.
  if (current_.type == TokenType::Identifier) {
    const Token saved = current_;
    Advance();
    if (At("=>") && !current_.newline_before) {
      Advance();
      NodePtr arrow = Make(NodeKind::ArrowFunction);
      NodePtr parameters = Make(NodeKind::Parameters);
      NodePtr parameter = Make(NodeKind::Identifier);
      parameter->string = saved.lexeme;
      parameters->children.push_back(std::move(parameter));
      arrow->children.push_back(std::move(parameters));
      arrow->children.push_back(At("{") ? ParseBlock() : ParseAssignment());
      return arrow;
    }
    lexer_.SeekTo(saved.end, saved.line);
    current_ = saved;
  }

  NodePtr left = ParseConditional();
  if (current_.type == TokenType::Punctuator && IsAssignmentOperator(current_.lexeme)) {
    NodePtr node = Make(NodeKind::Assignment);
    node->string = std::string(current_.lexeme);
    const bool plain = node->string == "=";
    Advance();
    node->children.push_back(std::move(left));
    node->children.push_back(ParseAssignment());  // right-associative
    // `f = () => {}` names the arrow `f`, and only a plain `=` does: `f ||= ()
    // => {}` is a different production and the spec names that one too, but
    // the common case is this one.
    if (plain && node->Child(0) != nullptr && node->Child(0)->kind == NodeKind::Identifier) {
      InferName(node->children[1].get(), node->Child(0)->string);
    }
    return node;
  }
  return left;
}

NodePtr ParserImpl::ParseExpression() {
  NodePtr first = ParseAssignment();
  if (!At(",")) {
    return first;
  }
  NodePtr node = Make(NodeKind::Sequence);
  node->children.push_back(std::move(first));
  while (Eat(",")) {
    node->children.push_back(ParseAssignment());
  }
  return node;
}


ParseResult Parse(std::string_view source) {
  ParserImpl parser(source);
  return parser.ParseProgram();
}

}  // namespace microbrowser::js

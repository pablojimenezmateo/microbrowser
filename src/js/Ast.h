#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace microbrowser::js {

// The syntax tree, as one tagged node type rather than forty classes.
//
// That is a deliberate trade and worth stating. A class per production gives
// the compiler a say in which fields exist where; a tagged node gives a tree
// that is uniform to walk, cheap to copy, trivial to dump for a test, and
// impossible to grow a virtual method on by accident. For a language whose
// grammar is this irregular, the walk-uniformity wins: every consumer --
// the interpreter, a scope resolver, a source printer -- is a switch over
// Kind, and a switch that forgets a case is a compiler warning.
//
// The cost is that "which fields does this kind use" lives in comments and in
// the parser rather than in types. The mitigation is that the parser is the
// only thing that constructs nodes, and each kind's shape is documented here.
enum class NodeKind : std::uint8_t {
  // --- Expressions ---------------------------------------------------------
  NumberLiteral,      // number
  StringLiteral,      // string
  TemplateLiteral,    // string = raw source; children = substitution expressions
  RegExpLiteral,      // string = full literal including flags
  BooleanLiteral,     // number != 0
  NullLiteral,        //
  Identifier,         // string
  ThisExpression,     //
  Super,              // `super`, legal only inside a method; the check is the parser's caller's
  ArrayLiteral,       // children = elements; a hole is a null child
  ObjectLiteral,      // children = Property
  Property,           // string = key when not computed; [0] = value, [1] = key if computed
  FunctionExpression, // string = name (may be empty); [0] = Parameters; [1] = Block;
                      // number = FunctionFlags
  ArrowFunction,      // [0] = Parameters; [1] = Block or expression body; number = FunctionFlags
  ClassExpression,    // string = name; [0] = superclass or null; rest = MethodDefinition
  MethodDefinition,   // string = name; [0] = FunctionExpression; number = flags
  Parameters,         // children = Identifier / RestElement / AssignmentPattern
  RestElement,        // [0] = target
  AssignmentPattern,  // [0] = target; [1] = default
  Yield,              // [0] = argument or null; number = 1 when delegating (`yield*`)
  Unary,              // string = operator, including `await`; [0] = operand
  Update,             // string = operator; number = 1 when prefix; [0] = operand
  Binary,             // string = operator; [0] = left; [1] = right
  Logical,            // string = operator (&&, ||, ??); [0] = left; [1] = right
  Assignment,         // string = operator; [0] = target; [1] = value
  Conditional,        // [0] = test; [1] = consequent; [2] = alternate
  Call,               // [0] = callee; rest = arguments; number = CallFlags
  New,                // [0] = callee; rest = arguments
  Member,             // [0] = object; [1] = property; number = MemberFlags
  Sequence,           // children = expressions
  Spread,             // [0] = argument
  TaggedTemplate,     // [0] = tag; [1] = TemplateLiteral

  // --- Statements ----------------------------------------------------------
  Program,            // children = statements
  Block,              // children = statements
  VariableDeclaration,// string = kind (var/let/const); children = Declarator
  Declarator,         // [0] = target; [1] = initializer or null
  ExpressionStatement,// [0] = expression
  If,                 // [0] = test; [1] = consequent; [2] = alternate or null
  For,                // [0] = init or null; [1] = test or null; [2] = update or null; [3] = body
  ForIn,              // string = "in" or "of"; [0] = left; [1] = right; [2] = body;
                      // number = 1 for `for await`, which is "of" only
  While,              // [0] = test; [1] = body
  DoWhile,            // [0] = body; [1] = test
  Return,             // [0] = argument or null
  Break,              // string = label
  Continue,           // string = label
  Throw,              // [0] = argument
  Try,                // [0] = block; [1] = catch param or null; [2] = catch body or null;
                      // [3] = finally or null
  Switch,             // [0] = discriminant; rest = SwitchCase
  SwitchCase,         // [0] = test or null (default); rest = statements
  Labeled,            // string = label; [0] = body
  FunctionDeclaration,// same shape as FunctionExpression, number and all
  ClassDeclaration,   // same shape as ClassExpression
  Empty,              //
  Debugger,           //
};

// What a function's body can do, in `number` on a FunctionExpression, an
// ArrowFunction, a FunctionDeclaration or a method's function.
//
// Two flags rather than two fields because they compose: `async function*` is
// both, and the compiler reads the pair to decide what a call returns and what
// suspends the frame. An arrow can be async and can never be a generator --
// there is no syntax for one -- so the parser sets only the first there.
enum FunctionFlags : std::uint8_t {
  kFunctionPlain = 0,
  kFunctionAsync = 1 << 0,
  kFunctionGenerator = 1 << 1,
};

// What a Member access is, in `number`.
//
// Bits rather than three values, because they compose: `a?.[k]` is computed
// *and* optional, and treating them as alternatives is exactly why that form
// used to be a syntax error.
enum MemberFlags : std::uint8_t {
  kMemberPlain = 0,
  kMemberComputed = 1 << 0,
  // This link is written `?.`, so a nullish base gives up.
  kMemberOptional = 1 << 1,
  // The outermost link of a chain that contains an optional one.
  //
  // Short-circuiting is a property of the *chain*, not of the link: in
  // `a?.b.c.d`, a nullish `a` makes the whole expression undefined rather than
  // making `.b` undefined and then reading `.c` off it. Only the parser knows
  // where a chain ends -- by the time either engine has a node it has lost the
  // surrounding syntax -- so the parser marks it, and both engines read the
  // mark instead of keeping a chain-in-progress flag of their own.
  kMemberChainRoot = 1 << 2,
};

// The same, for a Call.
enum CallFlags : std::uint8_t {
  kCallPlain = 0,
  kCallOptional = 1 << 0,
  kCallChainRoot = 1 << 1,
};

// Flags on a MethodDefinition, in `number`.
enum MethodFlags : std::uint8_t {
  kMethodPlain = 0,
  kMethodStatic = 1 << 0,
  kMethodGetter = 1 << 1,
  kMethodSetter = 1 << 2,
  kMethodComputed = 1 << 3,
  // These two are also set as FunctionFlags on the method's FunctionExpression,
  // which is what the compiler reads -- a method body is compiled through the
  // same path an ordinary function is and should not have to know it came from
  // a class.
  kMethodAsync = 1 << 4,
  kMethodGenerator = 1 << 5,
};

struct Node {
  NodeKind kind = NodeKind::Empty;
  std::string string;
  double number = 0.0;
  // A null child is meaningful for several kinds -- an array hole, a missing
  // `else`, an omitted `for` clause -- so the vector is not compacted.
  std::vector<std::unique_ptr<Node>> children;
  std::size_t start = 0;
  std::size_t line = 1;

  const Node* Child(std::size_t index) const {
    return index < children.size() ? children[index].get() : nullptr;
  }
};

using NodePtr = std::unique_ptr<Node>;

// A tree dumped as parenthesised text, for tests. Structure only, so a test can
// state the parse it expects instead of walking children by index -- which is
// how a test ends up asserting something other than what it means.
std::string DumpAst(const Node& node);

}  // namespace microbrowser::js

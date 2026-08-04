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
  // `123n`. string = the digits, with the `n` and any separators gone; the
  // value is built when it is evaluated, because a Node carries no heap cell.
  BigIntLiteral,
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
  NewTarget,          // `new.target`; no children
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

  // --- Modules -------------------------------------------------------------
  // `import ... from "spec"`. string = the specifier; children = Import.
  // A bare `import "spec"` has no children and is loaded for its side effects.
  ImportDeclaration,
  // One name an import brings in. string = the local binding; number =
  // ImportKind; [0] = the exported name as an Identifier, for Named only.
  Import,
  // `export ...`. string = the specifier for a re-export and empty otherwise;
  // number = ExportFlags; [0] = the declaration for a declaration export, or
  // the expression for `export default expr`; children after that = Export.
  ExportDeclaration,
  // One name an export publishes. string = the local name; [0] = the exported
  // name as an Identifier when it differs.
  Export,
  // `import.meta`, which is the module's own record as an object.
  ImportMeta,
  // `import(spec)`, which is a *call* rather than a declaration: it resolves
  // at run time and answers a promise. [0] = the specifier expression.
  ImportCall,
};

// What one entry of an import declaration brings in.
enum ImportKind : std::uint8_t {
  // `import { a }` and `import { a as b }`.
  kImportNamed = 0,
  // `import a from`, which is sugar for `import { default as a }` and is kept
  // apart because the name `default` is a keyword and cannot be written.
  kImportDefault = 1,
  // `import * as ns from`, which binds the namespace object itself.
  kImportNamespace = 2,
};

enum ExportFlags : std::uint8_t {
  kExportPlain = 0,
  // `export default ...`. The thing exported is [0] and its name is "default".
  kExportDefault = 1 << 0,
  // `export * from "spec"`, which re-publishes every name the other module has
  // except `default`.
  kExportAll = 1 << 1,
  // `export const x = 1` and friends: [0] is the declaration, and the names it
  // binds are exported as well as declared.
  kExportDeclaration = 1 << 2,
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
  // `static { ... }`. Not a method and not a field: a block that runs once,
  // with `this` bound to the class, after the static fields written above it
  // and before those written below. Carried as a MethodDefinition anyway --
  // its body is a function body and gets compiled through the same path -- so
  // that a class body stays one list rather than two.
  kMethodStaticBlock = 1 << 6,
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

// The arity a function reports.
//
// Not the number of parameters: it stops at the first one with a default and
// at a rest element, which is what `Function.prototype.length` is defined as
// and what every library that dispatches on arity reads. `(a, b = 1) => {}`
// has length 1.
inline std::uint32_t DeclaredArity(const Node* parameters) {
  if (parameters == nullptr) {
    return 0;
  }
  std::uint32_t arity = 0;
  for (const NodePtr& parameter : parameters->children) {
    if (parameter == nullptr || parameter->kind == NodeKind::AssignmentPattern ||
        parameter->kind == NodeKind::RestElement || parameter->kind == NodeKind::Spread) {
      break;
    }
    ++arity;
  }
  return arity;
}


// Every name a `var` declares anywhere inside `body`, in source order, with
// duplicates removed.
//
// `var` is scoped to the *function*, not to the block it is written in, and
// its binding exists from the moment the function is entered rather than from
// the line that declares it. Both halves of that are what this collects: a
// `var` inside an `if`, a loop, a `try` or a bare block belongs to the
// function, and a read before its line is `undefined` rather than an error.
//
// It stops at a nested function -- that function's `var`s are its own -- but
// it does descend through every statement form that can contain one, which is
// the part that is easy to get wrong. Shared by both engines because both had
// the same gap and a second copy would drift.
void CollectVarNames(const Node& body, std::vector<std::string>& out);

// A tree dumped as parenthesised text, for tests. Structure only, so a test can
// state the parse it expects instead of walking children by index -- which is
// how a test ends up asserting something other than what it means.
std::string DumpAst(const Node& node);

}  // namespace microbrowser::js

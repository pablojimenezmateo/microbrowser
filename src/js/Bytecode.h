#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Value.h"

namespace microbrowser::js {

class Object;
class Environment;

// The instruction set, and the code a compiled function is.
//
// Why this exists at all is written in Heap.h and Interpreter.h, and it is not
// mainly speed. A tree-walker keeps every intermediate value in a C++ local,
// where the collector cannot see it -- so collection can only run between
// top-level statements, and a script that allocates while recursing runs out of
// heap with nothing able to shrink it. It also cannot *suspend*, because its
// state is C++ stack frames, which is why `await` has nowhere to go. Both fall
// out of the same change: make the operand stack and the call stack data.
//
// The encoding is one fixed-width instruction with one operand rather than a
// packed byte stream. A packed stream is smaller and is what a mature engine
// uses; it is also a decoder, and a decoder is a place to have a bug. This is
// the version whose behaviour can be read off the listing, and the width can be
// squeezed later against a benchmark rather than against a guess.

// The binary operators, as an enum rather than as the source text.
//
// One entry per operator the language has. Shared with the tree-walking
// interpreter deliberately: `+` is the most surprising operator in the language
// and two implementations of it would eventually disagree, so both paths go
// through Interpreter::ApplyBinary and this is what they name it with.
enum class BinaryOp : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  Exponent,
  LooseEqual,
  LooseNotEqual,
  StrictEqual,
  StrictNotEqual,
  Less,
  Greater,
  LessEqual,
  GreaterEqual,
  BitAnd,
  BitOr,
  BitXor,
  ShiftLeft,
  ShiftRight,
  ShiftRightUnsigned,
  In,
  InstanceOf,
};

// The operator the text spells, or false when it spells none of them. Used by
// the compiler and by the tree-walker's own dispatch, so a typo in one table
// cannot make the two paths differ.
bool ParseBinaryOp(std::string_view text, BinaryOp& op);

// What the machine can do.
//
// The stack effect of each is written beside it, innermost-last: `[base key] ->
// [value]` means the key is on top when the instruction runs and the value is
// on top after. That comment is the contract the compiler and the VM have with
// each other, and it is the first thing to check when a program produces a
// value from the wrong slot.
enum class Op : std::uint8_t {
  // --- Stack shuffling -----------------------------------------------------
  Pop,          // [a] -> []
  PopUnder,     // [a b] -> [b]
  Dup,          // [a] -> [a a]
  Dup2,         // [a b] -> [a b a b]
  Swap,         // [a b] -> [b a]
  // Lifts the top element out and re-inserts it `a` places down, which is how a
  // value that has to survive a store gets underneath the store's operands.
  // With a = 2: [x y z] -> [z x y].
  RotateDown,

  // --- Constants -----------------------------------------------------------
  PushConstant,   // a = constant index
  PushUndefined,
  PushNull,
  PushTrue,
  PushFalse,

  // --- Names ---------------------------------------------------------------
  // Bindings still live in Environment objects rather than in numbered slots.
  // That is the deliberate first cut: it keeps closures, `this`, `arguments`
  // and every builtin working exactly as they did, so this change is about the
  // stack becoming data and nothing else. Slot resolution is a later pass and a
  // separate argument.
  LoadName,     // a = name index -> [value]; ReferenceError when unbound
  StoreName,    // a = name index; [value] -> [value]
  DeclareLet,   // a = name index; [value] -> []
  DeclareConst, // a = name index; [value] -> []
  TypeofName,   // a = name index -> [string]; "undefined" when unbound
  LoadThis,

  // --- Properties ----------------------------------------------------------
  GetProperty,      // [base key] -> [value]
  GetPropertyName,  // a = name index; [base] -> [value]
  SetProperty,      // [base key value] -> [value]
  SetPropertyName,  // a = name index; [base value] -> [value]
  DeleteProperty,   // [base key] -> [boolean]
  // What makes reading through null a TypeError that names the property rather
  // than a crash. Two forms because the base is under the key when the key was
  // computed, and the message quotes whichever one says what was written.
  ThrowIfNullishName,  // a = name index; [base] -> [base]
  ThrowIfNullishKey,   // [base key] -> [base key]

  // --- Operators -----------------------------------------------------------
  Binary,       // a = BinaryOp; [left right] -> [value]
  Not,
  Negate,
  UnaryPlus,
  BitNot,
  TypeofValue,
  Discard,      // `void`: [a] -> [undefined]
  ToNumberOp,   // what `++` does to its operand before adding to it

  // --- Jumps ---------------------------------------------------------------
  // The peeking forms leave their operand where it is, which is what makes
  // `a || b` evaluate to `a` rather than to a boolean.
  Jump,                // a = target
  JumpIfFalse,         // a = target; pops
  JumpIfTrue,          // a = target; pops
  JumpIfFalsePeek,     // a = target
  JumpIfTruePeek,      // a = target
  JumpIfNotNullish,    // a = target
  JumpIfNullish,       // a = target
  JumpIfNotUndefined,  // a = target; how a default value is skipped

  // --- Calls ---------------------------------------------------------------
  // A JavaScript-to-JavaScript call pushes a frame and keeps going. It does
  // *not* recurse into C++, which is the property everything else here is for.
  Call,       // a = argument count; [callee self args...] -> [result]
  CallApply,  // [callee self argumentArray] -> [result]; the spread form
  New,        // a = argument count; [callee args...] -> [instance]
  NewApply,   // [callee argumentArray] -> [instance]
  Return,     // [value] -> unwinds the frame
  LoadArgument,   // a = index -> [value]; undefined past the end
  RestArguments,  // a = index -> [array] of the arguments from there on

  // --- Building values -----------------------------------------------------
  NewArray,     // -> [array]
  ArrayPush,    // [array value] -> [array]
  ArrayHole,    // [array] -> [array]
  ArraySpread,  // [array iterable] -> [array]
  NewObject,    // -> [object]
  ObjectSet,        // [object key value] -> [object]
  ObjectSetName,    // a = name index; [object value] -> [object]
  ObjectGetter,     // [object key function] -> [object]
  ObjectSetter,     // [object key function] -> [object]
  ObjectSpread,     // [object source] -> [object]
  Closure,          // a = function index -> [function]
  ClosureArrow,     // a = function index -> [function]; captures `this` now
  // Three forms the compiler hands back to the tree-walking evaluator, because
  // each is a whole feature and none of them is on a hot path. A class body's
  // methods are still walked; compiling them is the next step and is what
  // `super` waits on.
  ClassLiteral,     // a = node index -> [constructor]
  RegExpLiteral,    // a = node index -> [regexp]
  TemplateStrings,  // a = node index -> [array] with `raw`, for a tagged template

  // --- Scopes --------------------------------------------------------------
  PushScope,
  PopScope,   // a = how many

  // --- Iteration -----------------------------------------------------------
  // The cursor lives on the interpreter's iteration stack rather than on the
  // value stack: it is four fields, and a `for...of` that breaks early must be
  // able to stop asking, which means the state has to outlive one instruction.
  IterateOpen,   // [iterable] -> []
  IterateNext,   // a = target; -> [value], or jumps when the iterator is done
  // What a destructuring pattern wants instead: a short iterable fills the
  // remaining names with undefined rather than ending the pattern. The cursor
  // remembers that it finished, so a custom iterator is not asked twice.
  IterateStep,   // -> [value or undefined]
  IterateRest,   // -> [array] of everything the iterator has left
  IterateClose,  // a = how many
  ForInKeys,     // [object] -> [array of key strings]

  // --- The script's value --------------------------------------------------
  // Only the top-level chunk emits these. A script evaluates to its last
  // statement's value, which is what a console prints and what every test in
  // JsInterpreterTests asserts on. The slot is the frame's first, so it is an
  // ordinary root rather than a field to remember to trace.
  SetCompletion,    // [value] -> []
  ClearCompletion,
  LoadCompletion,   // -> [value]; the last thing a program does before returning

  // --- Abrupt --------------------------------------------------------------
  ThrowOp,          // [value] -> throws
  // For a construct the parser accepts and the language does not allow --
  // `super` outside a method, a bare spread. A thrown SyntaxError at the point
  // it is reached, which is what the tree-walker does.
  ThrowSyntaxError, // a = constant index holding the message

  Nop,
};

struct Instruction {
  Op op = Op::Nop;
  std::uint32_t a = 0;
};

// Where a throw goes, and what has to be unwound to get there.
//
// The three depths are the whole reason this is a table rather than a jump
// target. A throw can happen half way through an expression, inside three
// nested blocks, with two `for...of` cursors open -- and the catch clause has
// to start from the state the `try` started from. Recording the depths at
// compile time is what makes that a truncation rather than a search.
struct Handler {
  std::uint32_t begin = 0;  // first instruction covered
  std::uint32_t end = 0;    // one past the last
  std::uint32_t target = 0;
  std::uint32_t stack_depth = 0;
  std::uint32_t scope_depth = 0;
  std::uint32_t iteration_depth = 0;
};

// One function's code, and everything it names.
//
// Nested functions are owned here rather than interned globally, so a program's
// code is one tree that is freed as a unit -- the same shape the AST has, for
// the same reason: a function object points into it and can outlive the script
// that made it.
struct CompiledFunction {
  std::string name;
  std::vector<Instruction> code;
  // Primitives only. An object constant would be a root the collector has to
  // know about, and nothing needs one: an object literal is built by
  // instructions, and a regular expression has to be a fresh object per
  // evaluation anyway so that `lastIndex` starts over.
  std::vector<Value> constants;
  std::vector<std::string> names;
  // The same names, already built as property keys.
  //
  // A named access used to pass `names[a]` to GetProperty, which takes a
  // PropertyKey -- so every `o.x` copied the name into a fresh key, which for
  // anything past the small-string limit is a malloc per property read. The
  // name is known at compile time and so is the key; building it once is the
  // whole fix. Indices match `names` exactly, which is what lets one operand
  // serve both.
  std::vector<PropertyKey> keys;
  std::vector<Handler> handlers;
  std::vector<std::unique_ptr<CompiledFunction>> functions;
  // The AST nodes the three delegating opcodes point at. Borrowed from the
  // program tree, which outlives this for exactly the same reason.
  std::vector<const Node*> nodes;
  std::uint32_t parameter_count = 0;
  bool is_arrow = false;
  // Set when the body names `arguments`, so the array is built per call only
  // where something can observe it. The scan stops at a nested ordinary
  // function, which has an `arguments` of its own, and does not stop at an
  // arrow, which does not.
  bool needs_arguments = false;
};

// The value stack is reserved once and never grows.
//
// Not an optimization. A vector that reallocates invalidates every reference
// into it, and the machine takes references into it constantly; a fixed
// capacity makes that safe by construction rather than by remembering.
// Overflowing it is the same RangeError as running out of call depth, because
// from a page's side it is the same thing.
inline constexpr std::size_t kValueStackCapacity = 1u << 16;
inline constexpr std::size_t kFrameCapacity = 256;

// Compiles a parsed program, or returns null when some construct in it has no
// compiled form yet.
//
// Returning null is the mechanism that lets this arrive in pieces: a program
// the compiler does not fully understand is run by the tree-walking evaluator
// instead, exactly as before, so every step of the way the whole test suite
// passes on whichever engine took the program. Half a chunk is not runnable,
// so one unsupported construct rejects the program rather than the function --
// a function's code has to be complete for its *caller* to be compilable.
std::unique_ptr<CompiledFunction> Compile(const Node& program);

// One call in progress.
//
// Everything the machine needs to resume this function is here, and every
// pointer in it is one the collector walks -- which is the difference between
// this and the C++ frame it replaces.
struct Frame {
  const CompiledFunction* code = nullptr;
  Object* function = nullptr;
  // The call's own scope, holding `this` and the parameters. Block scopes go on
  // the interpreter's shared scope stack above it, so that a handler can
  // truncate to a depth instead of walking a chain.
  Environment* scope = nullptr;
  std::uint32_t ip = 0;
  // Where the callee was pushed. The result is written here and the stack is
  // truncated to just past it, so a return needs no arithmetic on the caller.
  std::size_t stack_base = 0;
  std::size_t argument_base = 0;
  std::uint32_t argument_count = 0;
  std::size_t scope_base = 0;
  std::size_t iteration_base = 0;
};

}  // namespace microbrowser::js

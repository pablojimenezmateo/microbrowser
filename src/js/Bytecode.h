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
  // The slow path, for a name the compiler could not place: anything in the
  // global scope, which other scripts and every builtin also write to, and
  // anything past the packing limits below.
  LoadName,     // a = name index -> [value]; ReferenceError when unbound
  StoreName,    // a = name index; [value] -> [value]
  DeclareLet,   // a = name index; [value] -> []
  DeclareConst, // a = name index; [value] -> []
  TypeofName,   // a = name index -> [string]; "undefined" when unbound

  // The fast path: a binding the compiler placed, reached by walking `hops`
  // scopes and indexing. No hashing, and no name at run time except in the
  // error message.
  //
  // The operand packs all three, so a load is two shifts and two masks rather
  // than a memory access to a side table:
  //
  //     hops  4 bits   scopes to walk out         (0-15)
  //     slot 12 bits   index within that scope    (0-4095)
  //     name 16 bits   for the error message only (0-65535)
  //
  // Every limit has the same fallback: the compiler emits the name form
  // instead. A function nested sixteen deep or a scope with four thousand
  // bindings is slower, not wrong.
  LoadSlot,     // a = packed -> [value]; ReferenceError before its declaration
  StoreSlot,    // a = packed; [value] -> [value]; TypeError on a const
  DeclareSlot,  // a = declaration index; [value] -> []

  LoadThis,
  // `new.target`. Undefined unless the running call was reached through `new`,
  // and it is the *most derived* constructor -- `new B()` gives B inside A's
  // constructor too, which is what the transpiled `class` guards read.
  LoadNewTarget,

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
  // What a template substitution does to its value before it is joined on.
  //
  // Not the same as letting `+` do it. `+` converts with the *default* hint,
  // which tries `valueOf` first; a substitution is ToString, which tries
  // `toString` first. An object with both is the only thing that can tell, and
  // a page that writes one has written it precisely so that it can.
  ToStringOp,   // [a] -> [string]

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
  Return,     // [value] -> unwinds the frame, or settles its promise when async
  // Suspends the running call. The frame comes off the machine and is filed
  // whole -- code pointer, ip, its slice of every stack -- and the promise the
  // call returns is left where its result would have gone, so the caller
  // continues with a promise the moment the body first waits. When the awaited
  // value settles, a microtask puts the frame back and pushes the value here.
  //
  // This is the instruction the machine was built for. Against C++ stack
  // frames there was nowhere to put one.
  Await,      // [value] -> [awaited value], eventually and in another turn
  // Suspends a generator's call and hands the value to whoever resumed it.
  // The same machinery Await uses -- the frame is filed whole and put back --
  // pointed at a different trigger: what puts this one back is a call to the
  // generator's `next`, `throw` or `return` rather than a settled promise.
  Yield,      // [value] -> [the value the next `next` sends in]
  // The suspend a generator body begins with, emitted once and immediately
  // after the parameters are bound. Hands the *caller* the generator object,
  // which is what makes calling a generator function return an iterator
  // without running a line of the body -- while still binding the parameters,
  // and still throwing out of the call when a default does.
  GeneratorEntry,  // -> [the value the first `next` sends in]
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
  // The other direction: what `const {a, ...rest} = o` binds to `rest`.
  //
  // `a` is how many keys the pattern already named, and those keys sit *under*
  // the source -- the pattern pushed each one as it went, because a computed
  // key is not known until it has run.
  // [key... source] -> [object] with every other own enumerable property.
  ObjectRest,
  Closure,          // a = function index -> [function]
  ClosureArrow,     // a = function index -> [function]; captures `this` now
  // Three forms the compiler hands back to the tree-walking evaluator, because
  // each is a whole feature and none of them is on a hot path. A class body's
  // methods are still walked; compiling them is the next step and is what
  // `super` waits on.
  ClassLiteral,     // a = node index -> [constructor]
  // `super.x` and `super(...)`, which only a class body can contain -- so these
  // exist because class methods are compiled, and could not before.
  LoadSuperBase,    // -> [the prototype of the object the method was defined on]
  SuperCall,        // a = argument count; [args...] -> []
  RegExpLiteral,    // a = node index -> [regexp]
  TemplateStrings,  // a = node index -> [array] with `raw`, for a tagged template

  // --- Scopes --------------------------------------------------------------
  PushScope,  // a = slots to reserve
  PopScope,   // a = how many
  // What a function whose scopes nothing can capture emits instead of the two
  // above: its bindings are already in the frame, so entering a block is
  // putting that block's slots back in their undeclared state rather than
  // allocating anything. Needed because a block can be entered twice -- the
  // second time round a loop, `x` before its own `let x` must still be a
  // ReferenceError.
  ClearLocals,  // a = packed base and count; see PackLocals
  // Replaces the innermost scope with a fresh copy of itself.
  //
  // What makes `for (let i = 0; ...)` one binding *per iteration* rather than
  // one for the whole loop -- which is the difference between `fs.map(f => f())`
  // giving `0,1,2` and giving `3,3,3`. A closure made in the body captured the
  // old Environment and keeps it; the next iteration reads and writes the new
  // one. Emitted only for a `let` or `const` loop head, and only in a function
  // whose scopes are on the heap: in a flattened one nothing can capture a
  // binding, so there is nothing to tell the difference and nothing to copy.
  CopyScope,

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
  // What `yield*` steps with. Unlike IterateNext it pushes on both paths: the
  // yielded value when there is one, and the delegate's *return* value when
  // there is not -- which is what the whole expression evaluates to.
  IterateDelegate,  // a = target; [sent] -> [value], or [return value] and jumps
  // What `yield*` does with a throw or a forced return that arrived at its
  // `yield`. Forwards it to the delegate's own `throw` or `return`, which is
  // the difference between a `yield*` being a loop and being a relationship:
  // an inner generator's `catch` sees `g.throw(e)`, and its `finally` runs
  // when `g.return()` closes the outer one.
  //
  // a = target; [thrown] -> [value], or [return value] and jumps. Rethrows
  // when the delegate has no method to forward to, which for a `return` is
  // the ordinary case and for a `throw` is the spec's TypeError.
  IterateForward,
  // The three `for await` uses. The step and the unpack are separate because
  // an Await sits between them: an async iterator's `next` hands back a
  // promise, and what the loop needs is the `{value, done}` inside it.
  // The three `for await` uses. Two of them can end the loop, which is the one
  // odd thing about the shape and is forced: a sync iterable answers `done`
  // when it is stepped, and an async one answers it inside the promise that
  // step returned -- so the branch is on either side of the Await between them.
  IterateOpenAsync,    // [iterable] -> []; resolves Symbol.asyncIterator first
  // a = target. Sync: the next value, or jumps when there is none. Async: the
  // promise `next` returned, and never jumps.
  IterateAsyncStep,
  // a = target. Async: [{value, done}] -> [value], or jumps when done. Sync: a
  // no-op -- what is on the stack is already the awaited value.
  IterateAsyncUnpack,
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

// The packing behind LoadSlot and StoreSlot, and the limits that make it fit.
inline constexpr std::uint32_t kMaxSlotHops = 15;
inline constexpr std::uint32_t kMaxSlotIndex = 4095;
inline constexpr std::uint32_t kMaxSlotName = 65535;

inline constexpr std::uint32_t PackSlot(std::uint32_t hops, std::uint32_t slot,
                                        std::uint32_t name) {
  return (hops << 28) | (slot << 16) | name;
}
inline constexpr std::uint32_t SlotHops(std::uint32_t packed) { return packed >> 28; }
inline constexpr std::uint32_t SlotIndex(std::uint32_t packed) { return (packed >> 16) & 0xFFFu; }
inline constexpr std::uint32_t SlotName(std::uint32_t packed) { return packed & 0xFFFFu; }

// The packing behind ClearLocals: a contiguous run of frame slots, which is
// what a block's bindings are because they are all reserved when it opens.
// Both halves are bounded by kMaxSlotIndex, so twelve bits each is exact
// rather than generous.
inline constexpr std::uint32_t PackLocals(std::uint32_t base, std::uint32_t count) {
  return (base << 12) | count;
}
inline constexpr std::uint32_t LocalsBase(std::uint32_t packed) { return packed >> 12; }
inline constexpr std::uint32_t LocalsCount(std::uint32_t packed) { return packed & 0xFFFu; }

// The first four slots of every compiled function's own scope.
//
// Fixed, and reserved whether or not the function uses them, because the
// compiler has to know a parameter's index without knowing what the *runtime*
// decided to declare. `__home__` and `arguments` are reserved either way and
// filled only when they exist -- a reserved-but-unset slot is not a binding, so
// a name lookup for `arguments` in a function that has none walks out to the
// enclosing one exactly as it did before.
//
// Interpreter::PushFrame fills these and Compiler::Function reserves them, in
// this order. The two agreeing is what the whole scheme rests on.
inline constexpr std::uint32_t kSlotThis = 0;
inline constexpr std::uint32_t kSlotHome = 1;
inline constexpr std::uint32_t kSlotFunction = 2;
inline constexpr std::uint32_t kSlotArguments = 3;
// `new.target`: the constructor a `new` names, or unset for an ordinary call.
//
// A slot rather than something derived, because "was this called with `new`"
// is not a question the callee can answer any other way -- the receiver looks
// the same either way, and a class that guards on it is guarding against
// exactly the call that looks ordinary.
inline constexpr std::uint32_t kSlotNewTarget = 4;
inline constexpr std::uint32_t kReservedSlots = 5;

// What a DeclareSlot fills in. A record rather than more packed bits because
// this one is not on a hot path -- a declaration runs once per scope, a read
// runs every time round the loop.
struct SlotDeclaration {
  std::uint32_t slot = 0;
  std::uint32_t name = 0;
  bool is_const = false;
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
  // Whether `target` is a finalizer rather than a `catch` clause.
  //
  // The two are the same shape to a throw and are not the same to a *return*.
  // Forcing a generator to return -- which is what `it.return()` and a
  // `for...of` that breaks both do -- has to run every `finally` between the
  // `yield` it was sitting at and the end of the body, and must run no `catch`
  // at all: the page did not throw anything, and a `catch (e)` that saw this
  // would be catching a completion rather than an error.
  bool is_finally = false;
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
  std::vector<SlotDeclaration> declarations;
  std::vector<std::unique_ptr<CompiledFunction>> functions;
  // The node this was compiled from, when something has to find it again by
  // node rather than by index. A class body is the case: its builder is still
  // the tree-walking EvaluateClass, which walks the members and asks for each
  // one's compiled body as it goes.
  const Node* source = nullptr;
  // The AST nodes the three delegating opcodes point at. Borrowed from the
  // program tree, which outlives this for exactly the same reason.
  std::vector<const Node*> nodes;
  std::uint32_t parameter_count = 0;
  // Whether calling this returns a promise and its body can suspend. Read by
  // PushFrame, which makes the promise, and by Return, which settles it.
  bool is_async = false;
  // Whether calling this returns a generator and runs none of the body. Read
  // by PushFrame, which makes the generator object the first GeneratorEntry
  // hands back. A sibling of the flag above rather than a mode beside it: the
  // two are independent bits of the same question -- what a call gives its
  // caller, and what suspends the frame.
  bool is_generator = false;
  // How many slots this function needs. The four reserved above plus one per
  // parameter binding -- and, when `frame_locals` is set, one per binding
  // every block in the body declares as well, because those live here too.
  // PushFrame reserves them before the prologue runs.
  std::uint32_t scope_slots = kReservedSlots;
  bool is_arrow = false;
  // Whether this function's scopes live in the frame instead of on the heap.
  //
  // A scope has to outlive its call only when something can still reach it
  // afterwards, and the only thing that can is a closure made inside it. A
  // function that creates none -- no nested function, no arrow, no class, so
  // no `Closure`, `ClosureArrow` or `ClassLiteral` op anywhere in its body --
  // cannot leak a scope, so its bindings go in a slice of the machine's locals
  // stack and the call allocates nothing. Every block in such a function is
  // flattened into that one slice, which is why the slot indices here are
  // function-wide rather than per-block.
  //
  // Decided from the syntax before anything is emitted, because the compiler
  // has to know it to resolve names: a name in a flattened function is one hop
  // count and a name in a scoped one is another. That it can be decided from
  // the syntax at all is the point -- a nested function is a node, and there
  // is no other way to make one. `eval` and `Function(source)` do not exist
  // here, and a test says so.
  bool frame_locals = false;
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
// How deep JavaScript recursion may go on the machine.
//
// Not the C++ stack: a JS-to-JS call is a push onto this vector, so the limit
// is a number chosen here rather than one the platform imposes -- which is the
// property the whole machine exists for. Two hundred and fifty-six was the
// figure from when calls still cost C++ frames, and it is far below what a
// page does: a recursive walk over a DOM tree or a parsed document reaches a
// thousand without being unusual.
//
// Reserved on the first call that needs it rather than in the constructor,
// like the locals stack and for the same reason: the capacity is fixed because
// an instruction holds a Frame* into it, but a page that runs no script should
// not pay for the reservation.
inline constexpr std::size_t kFrameCapacity = 4096;
// The locals stack, on the same terms and for the same reason: a frame holds a
// pointer into it while an instruction runs, so it is reserved once and
// overflowing it is a RangeError rather than a reallocation. Sized so that a
// full call stack of ordinary functions fits several times over; a function
// with thousands of bindings runs out of depth sooner, which is the same answer
// a page gets for recursing too far.
inline constexpr std::size_t kLocalsCapacity = 1u << 16;
// The `for...of` cursor stack, on exactly the same terms, and it earned them
// the hard way. A stepping instruction holds an `Iteration&` into this while it
// runs, and stepping runs the page's own `next` -- which can open another
// cursor. Growing the vector under that reference is a write into freed memory,
// reachable from any page whose custom iterator has a loop in it. Reserved once
// so the reference cannot be invalidated, and overflowing it is the RangeError
// that running out of any other bounded resource is. Sized well past any real
// nesting: cursors nest with loops inside iterators, not with data.
inline constexpr std::size_t kIterationCapacity = 1u << 12;

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
  //
  // When the code has `frame_locals`, no such scope was allocated and this is
  // the defining scope instead -- the one a name that is not the frame's own
  // resolves through. Both cases want the same thing from it, which is why it
  // is one field: it is where the scope chain continues.
  Environment* scope = nullptr;
  // Where this frame's bindings start on the locals stack, when it has any.
  std::size_t locals_base = 0;
  // The promise an async call returns, made when the frame is pushed and
  // settled when it returns or throws. Null for an ordinary call, and the
  // thing that makes a frame's identity outlive the machine's stacks: a filed
  // frame is found again through the reaction this is attached to.
  Object* promise = nullptr;
  // The generator this call is the body of, made when the frame is pushed and
  // handed to the caller by the GeneratorEntry that follows. Null for every
  // other call. It is here for the reason the promise above is: it is how a
  // filed frame is found again, only in the other direction -- the promise
  // finds its frame through a reaction, and a generator's frame finds its
  // generator through this, because a `yield` has to record where it filed
  // itself somewhere the next `next` can read.
  Object* generator = nullptr;
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

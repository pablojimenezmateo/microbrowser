#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "js/Ast.h"
#include "js/Bytecode.h"

namespace microbrowser::js {

// The compiler's shared state, split across two translation units the way the
// parser is: expressions in Compiler.cpp and statements in
// CompilerStatements.cpp. Private to those two.

// What every function being compiled shares.
//
// Failure is a flag rather than an exception or a return code on every method,
// because a compiler that cannot handle a construct has to abandon the *whole*
// program -- half a chunk is not runnable -- and threading that through fifty
// mutually recursive emitters would bury the code that matters. Nothing checks
// the flag except the top level and the recursion bounds.
struct CompileState {
  bool failed = false;
  // Every instruction emitted across every function in the program. Bounded
  // because a finalizer is emitted once per path that leaves its try, and
  // nested try/finally multiplies. A page can write that on purpose.
  std::size_t emitted = 0;
  int depth = 0;
};

// Where a `break` or a `continue` goes, and what it has to unwind on the way.
//
// The three depths are why this is a record and not a label. Leaving a loop
// from inside two blocks and an open `for...of` means popping two scopes and
// closing one cursor, and the counts are the difference between the depth here
// and the depth the loop was entered at.
struct LoopContext {
  std::string label;
  bool is_loop = true;  // a switch and a labelled block are breakable, not continuable

  std::uint32_t break_stack = 0;
  std::uint32_t break_scopes = 0;
  std::uint32_t break_iterations = 0;
  std::uint32_t continue_stack = 0;
  std::uint32_t continue_scopes = 0;
  std::uint32_t continue_iterations = 0;
  // How many enclosing finalizers were in scope when the loop was entered.
  // Everything above this has to run before the jump.
  std::size_t finally_depth = 0;

  std::vector<std::uint32_t> break_jumps;
  std::vector<std::uint32_t> continue_jumps;
};

// A `finally` block that has to run before control leaves the try it belongs
// to.
//
// It is emitted again at each exit path rather than jumped to as a subroutine.
// A subroutine needs the pending completion to be a value the machine carries
// -- a return in flight, a break with a destination -- and then needs to
// dispatch on it, which is three opcodes and a protocol to get wrong. Emitting
// the block again is duplication, and duplication of the one thing that makes
// `try { return 1 } finally { return 2 }` return 2 without any of that: the
// inner return is simply the next instruction. The cost is code size when
// finalizers nest, which the emitted-instruction bound above makes finite.
struct FinallyContext {
  const Node* body = nullptr;
  std::uint32_t scope_depth = 0;
  std::uint32_t iteration_depth = 0;
};

// One scope the compiler created, and therefore one whose layout it knows.
//
// Every scope in this list corresponds to exactly one Environment at run time:
// a function's own, or one a PushScope makes. The correspondence is what makes
// "walk two scopes out and take slot three" a thing that can be decided while
// compiling -- so the model has to be built and torn down in lockstep with the
// PushScope and PopScope instructions, and every place that emits one of those
// also pushes or pops here.
struct CompiledScope {
  // Slot index per name. Assigned before any of the block's code is emitted,
  // because control flow can skip a declaration and the index still has to be
  // where the compiler said.
  std::unordered_map<std::string, std::uint32_t> slots;
  std::uint32_t count = 0;
  // Where those indices start. Zero in a function whose scopes are real
  // Environments, because each one is indexed from its own base. In a
  // flattened function every scope shares the frame's one slice, so a block
  // has to say where in it its own run of slots begins -- which is what
  // ClearLocals needs to put them back in their undeclared state.
  std::uint32_t base = 0;
};

class Compiler {
 public:
  Compiler(CompileState& state, CompiledFunction& function, Compiler* parent = nullptr);

  // The top level of a script. Its own scope is the global one, so it pushes
  // none, and each statement clears the completion slot first -- which is what
  // makes `1; if (false) 2` evaluate to undefined.
  void Program(const Node& program);
  // A function, an arrow, or a method. Emits the parameter binding and then
  // the body.
  void Function(const Node& node, bool arrow);

 private:
  // --- Emitting ------------------------------------------------------------
  // The delta is the instruction's effect on the operand stack, passed at the
  // call site rather than looked up in a table. The compiler has to know the
  // depth at every point -- a handler records it, and a `break` pops down to
  // it -- so getting one wrong is a bug either way; saying it out loud is what
  // makes it reviewable next to the instruction it describes.
  std::uint32_t Emit(Op op, std::uint32_t a, int delta);
  std::uint32_t Emit(Op op) { return Emit(op, 0, 0); }
  std::uint32_t Here() const;
  void Patch(std::uint32_t at, std::uint32_t target);
  void PatchAll(const std::vector<std::uint32_t>& jumps, std::uint32_t target);
  std::uint32_t Constant(Value value);
  std::uint32_t Name(std::string_view text);

  // --- Placing names -------------------------------------------------------
  // Reserves a slot for `name` in the innermost scope, or does nothing when
  // there is no scope to put it in -- the top level of a program declares into
  // the global scope, which other scripts and every builtin also write to, and
  // which therefore has no layout to know.
  void Reserve(std::string_view name);
  // Opens, enters and leaves one scope, in that order and always all three.
  // The reservations happen between the first two, which is why entering is
  // not part of opening: a block's slots are all known before any of it runs.
  //
  // These are also the only place that knows a flattened function emits
  // ClearLocals where a scoped one emits PushScope and PopScope. Everything
  // else -- the depths a handler records, what a `break` unwinds -- is the
  // same either way, which is why `scope_depth_` counts scopes the compiler
  // modelled rather than instructions it emitted.
  void OpenScope();
  void EnterScope();
  void LeaveScope();
  // Reserves every name a statement list declares, before any of it is
  // emitted. The two passes are the point: `switch` can jump past a `let` and
  // the one after it must still land where the compiler said.
  void ReserveDeclarations(const Node& list);
  void ReservePattern(const Node& target);
  // Emits the load, store or declaration for a name -- resolved to a slot when
  // this compiler or one of its parents placed it, and by name when not.
  void EmitLoad(std::string_view name);
  void EmitStore(std::string_view name);
  void EmitDeclare(std::string_view name, bool is_const);
  // The packed operand for a name, or false when it is not placed anywhere or
  // does not fit the packing.
  bool ResolveSlot(std::string_view name, std::uint32_t& packed);
  std::uint32_t NodeIndex(const Node& node);
  void Fail();
  // Pops scopes, closes iterators and drops stack slots until the machine is
  // back at the depths a jump target expects.
  void UnwindTo(std::uint32_t stack, std::uint32_t scopes, std::uint32_t iterations);
  // Emits every finalizer above `depth`, innermost first, unwinding to each
  // one's own try before it runs.
  void RunFinalizers(std::size_t depth);

  // --- Expressions (Compiler.cpp) ------------------------------------------
  void Expression(const Node& node);
  void Unary(const Node& node);
  // `yield` and `yield*`. The second is a loop over the delegate's iterator
  // rather than an opcode, which is where its two deviations come from; both
  // are written out where it is emitted.
  void Yield(const Node& node);
  void Update(const Node& node);
  void Assignment(const Node& node);
  void CallExpression(const Node& node);
  void NewExpression(const Node& node);
  void MemberExpression(const Node& node);
  void ObjectLiteral(const Node& node);
  void ArrayLiteral(const Node& node);
  void TemplateLiteral(const Node& node);
  // `import(spec)`, as a call to the hook the linker installed. A dynamic
  // import resolves at run time and answers a promise, so it is a call and
  // not a declaration -- which is what lets it be compiled at all.
  void ImportCall(const Node& node);
  void TaggedTemplate(const Node& node);
  void FunctionValue(const Node& node, bool arrow);
  // Compiles a function into this chunk without emitting anything, and returns
  // its index. FunctionValue is this plus a Closure instruction; a class body
  // needs the first half alone, because its methods are attached by the builder
  // rather than pushed on the stack.
  std::uint32_t CompileNested(const Node& node, bool arrow);
  // Compiles every method body in a class, inside a scope standing for the one
  // EvaluateClass creates at run time. Modelling that scope is what keeps a
  // method's hop counts right: at run time a method's scope has the class scope
  // as its parent, so the compiler has to count it too.
  void ClassMethods(const Node& node);
  // Pushes the arguments of a call or a `new` starting at `first`. Returns true
  // when one of them was a spread, in which case a single array was pushed and
  // the caller wants the Apply form of the opcode.
  bool CallArguments(const Node& node, std::size_t first, std::uint32_t& count);
  // Reads a member's base and key onto the stack. `key_on_stack` comes back
  // false for `o.x`, whose name is an operand instead.
  void MemberOperands(const Node& node, bool& key_on_stack, std::uint32_t& name);
  // One place an optional chain can give up, and how much it is holding when
  // it does. Every link in `a?.b?.()?.c` abandons with a different amount on
  // the stack, so each records its own depth rather than sharing a cleanup
  // that would be right for whichever was written last.
  struct ChainExit {
    std::uint32_t jump = 0;
    std::uint32_t depth = 0;
  };
  // Compiles a Member or Call that the parser marked as the root of an
  // optional chain, and lands every short-circuit inside it here.
  //
  // The chain is what short-circuits, not the link: `a?.b.c` with a nullish
  // `a` is undefined, and does not read `.c` off undefined. So the exits are
  // collected while the whole spine compiles and merged once at the end.
  void OptionalChain(const Node& node);
  // Where the chain being compiled collects its exits, or null when nothing
  // is being compiled inside one. Suspended around a computed key and around
  // call arguments -- those are separate expressions, and an optional chain
  // written inside one is its own chain.
  std::vector<ChainExit>* chain_ = nullptr;
  void ThrowSyntax(std::string message);

  // --- Statements (CompilerStatements.cpp) ---------------------------------
  void Statement(const Node& node);
  void StatementList(const Node& list);
  void Block(const Node& node);
  void Hoist(const Node& list);
  void VariableDeclaration(const Node& node);
  void IfStatement(const Node& node);
  void WhileStatement(const Node& node, bool is_do);
  void ForStatement(const Node& node);
  void ForInStatement(const Node& node);
  void TryStatement(const Node& node);
  void SwitchStatement(const Node& node);
  void LabeledStatement(const Node& node);
  void BreakOrContinue(const Node& node, bool is_break);
  void ReturnStatement(const Node& node);
  // Whether a statement list declares anything, which is what decides if it
  // needs a scope of its own. The same test the tree-walker makes, and for the
  // same reason: a loop body that declares nothing must not allocate one per
  // iteration.
  static bool Declares(const Node& list);

  // --- Patterns ------------------------------------------------------------
  // Consumes the value on top of the stack. `declare` is a `let`/`const`/
  // parameter binding; otherwise it is an assignment to something that already
  // exists.
  void BindTarget(const Node& target, bool declare, bool is_const);
  void BindArrayPattern(const Node& target, bool declare, bool is_const);
  void BindObjectPattern(const Node& target, bool declare, bool is_const);
  void StoreToMember(const Node& target);

  CompileState& state_;
  CompiledFunction& function_;
  // The compiler of the function this one is written inside, or null at the
  // top. Resolution walks it, because the run-time scope chain does too: a
  // frame's scope has the defining scope as its parent, so counting scopes out
  // here counts the same scopes the machine will walk.
  Compiler* parent_ = nullptr;

  std::unordered_map<std::string, std::uint32_t> names_;
  std::vector<CompiledScope> scopes_;
  std::vector<LoopContext> loops_;
  std::vector<FinallyContext> finallys_;

  std::uint32_t stack_depth_ = 0;
  std::uint32_t scope_depth_ = 0;
  // How many entries in `scopes_` are not block scopes: one for a function's
  // own, none for a program, whose outermost scope is the global one. Every
  // point that compiles an expression holds
  // `scopes_.size() == scope_floor_ + scope_depth_`.
  std::size_t scope_floor_ = 0;
  std::uint32_t iteration_depth_ = 0;
  // Slots handed out so far in a flattened function, across every scope in it.
  // Unused otherwise, where each scope numbers its own from zero.
  std::uint32_t frame_slots_ = 0;
  // Carried from a Labeled statement to the loop it wraps. A labelled
  // `continue` names a loop rather than a label, and the loop is the only
  // thing that can act on it.
  std::string pending_label_;
  // Set for the top-level chunk, where an expression statement's value is the
  // script's result.
  bool is_program_ = false;
};

// Counts compile depth for as long as it is in scope, and fails the whole
// compile past the limit rather than running the C++ stack out on a tree a
// page chose the shape of.
class CompileDepth {
 public:
  CompileDepth(CompileState& state, int limit) : state_(state), exceeded_(++state.depth > limit) {}
  ~CompileDepth() { --state_.depth; }
  CompileDepth(const CompileDepth&) = delete;
  CompileDepth& operator=(const CompileDepth&) = delete;
  bool Exceeded() const { return exceeded_; }

 private:
  CompileState& state_;
  bool exceeded_;
};

inline constexpr int kMaxCompileDepth = 400;
inline constexpr std::size_t kMaxEmittedInstructions = 1u << 20;

}  // namespace microbrowser::js

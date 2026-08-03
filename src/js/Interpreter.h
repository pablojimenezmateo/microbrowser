#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Parser.h"
#include "js/Value.h"

namespace microbrowser::js {

// How a statement or expression finished.
//
// Control flow is a value rather than an exception, and that is the choice that
// makes the interpreter auditable: `break`, `continue`, `return` and `throw`
// all propagate the same way, every caller has to say what it does with each,
// and a C++ exception escaping into the host is impossible because there are
// none. `finally` is the case that proves the design -- it has to run whatever
// the block did and then decide whether to override it, which is unreadable
// when the four paths are three exception types and a return.
enum class Completion : std::uint8_t { Normal, Return, Break, Continue, Throw };

struct Result {
  Completion completion = Completion::Normal;
  Value value;
  // For a labelled break or continue.
  std::string label;

  static Result Normal(Value value = Value::Undefined()) {
    return Result{Completion::Normal, std::move(value), {}};
  }
  bool IsAbrupt() const { return completion != Completion::Normal; }
};

// Runs a program.
//
// A tree-walking interpreter, and deliberately so for now: it is the version
// whose behaviour can be read off the spec a line at a time, which is what
// makes it worth trusting before anything is compiled. A bytecode VM is the
// same semantics with an explicit value stack -- and that stack is exactly what
// a precise collector needs, which is why the two arrive together rather than
// the VM being a pure speed change.
class Interpreter {
 public:
  Interpreter();
  ~Interpreter();

  Interpreter(const Interpreter&) = delete;
  Interpreter& operator=(const Interpreter&) = delete;

  // Runs `source` and returns its completion value, or the thrown value with a
  // Throw completion. A syntax error is a Throw too, so a caller has one
  // failure path rather than two.
  Result Run(std::string_view source);
  Result RunProgram(const Node& program);

  Heap& GetHeap() { return heap_; }
  Object* Global() { return global_; }
  Environment* GlobalScope() { return global_scope_; }

  // Calls a callable value. Public because the host needs it -- an event
  // handler is a JS function the browser calls, not the other way round.
  Result CallFunction(const Value& callee, const Value& self,
                      const std::vector<Value>& arguments);

  // Builds an Error object with a message. Public so a native function can
  // throw the way JS code does.
  Value MakeError(std::string_view kind, std::string message);
  Result Throw(std::string_view kind, std::string message);

  // Anything written by `console.log`, in order. The host decides what to do
  // with it; collecting rather than printing is what keeps the engine
  // testable and keeps a page from writing to the terminal.
  const std::vector<std::string>& ConsoleOutput() const { return console_; }

  // Runs a collection if enough has been allocated since the last one. Called
  // only where every live value is reachable from the roots this tracks --
  // between top-level statements, with no evaluation in progress. See Heap.h.
  void MaybeCollect();

  // Allocates an array for a native function, as a value. Public because a
  // builtin has no other way to make one, and making the heap public would be
  // worse. Undefined when the heap is full, which a builtin propagates as an
  // ordinary value rather than as a second failure channel.
  Value NewArrayValue(std::vector<Value> elements);
  Value NewArrayValue(std::vector<Value> elements, std::vector<bool> present);

 private:
  friend struct NativeCall;

  Result EvaluateStatement(const Node& node, Environment& scope);
  Result EvaluateBlock(const Node& node, Environment& scope);
  Result Evaluate(const Node& node, Environment& scope);
  Result EvaluateCall(const Node& node, Environment& scope);
  Result EvaluateAssignment(const Node& node, Environment& scope);
  Result EvaluateBinary(const Node& node, Environment& scope);
  Result EvaluateMember(const Node& node, Environment& scope, Value& base_out);
  Result EvaluateForIn(const Node& node, Environment& scope);
  Result EvaluateClass(const Node& node, Environment& scope);
  // Runs a class's instance field initializers against a fresh instance.
  // Separate from the constructor because fields run *before* the constructor
  // body and after any super() call, and folding them in loses that ordering.
  Result InitializeFields(Object* instance, Object* constructor);

  // Declares the function declarations in a statement list before running it,
  // which is what makes a function callable above where it is written.
  void HoistDeclarations(const Node& list, Environment& scope);

  Result BindPattern(const Node& target, const Value& value, Environment& scope, bool declare,
                     bool is_const);
  Result BindParameters(const Node& parameters, const std::vector<Value>& arguments,
                        Environment& scope);

  Value GetProperty(const Value& base, std::string_view key);
  Result SetProperty(const Value& base, std::string_view key, const Value& value);

  Object* NewObject();
  Object* NewArray(std::vector<Value> elements);
  Object* NewArray(std::vector<Value> elements, std::vector<bool> present);
  Value NewFunction(const Node& node, Environment& scope, bool arrow);
  // Wraps a C++ callable as a JS function object, and puts one on an object
  // under a name. Every builtin is installed through these two.
  Object* NewNative(const char* name, NativeFunction function);
  void InstallNative(Object* target, const char* name, NativeFunction function);
  void InstallGlobals();
  // String.prototype, in its own translation unit: it is the largest single
  // group of builtins and Builtins.cpp is already near the module's TU limit.
  void InstallStringPrototype(Object* string_constructor);
  void InstallFunctionPrototype();

  Heap heap_;
  Object* global_ = nullptr;
  Environment* global_scope_ = nullptr;
  Object* array_prototype_ = nullptr;
  Object* object_prototype_ = nullptr;
  Object* function_prototype_ = nullptr;
  // Where a string's methods live, so that `"a".trim` and
  // `String.prototype.trim` are the same function object. A string is a
  // primitive here rather than a boxed object, so the lookup in GetProperty
  // consults this directly instead of walking a prototype chain from a wrapper.
  Object* string_prototype_ = nullptr;
  std::vector<std::string> console_;

  // Every scope currently on the C++ call stack, so the collector can find
  // them. A tree-walker cannot scan its own frames; this is the shadow stack
  // that replaces that.
  std::vector<Environment*> active_scopes_;
  // Same, for values held across a call.
  std::vector<Object*> active_objects_;
  int call_depth_ = 0;
  // Evaluation depth, which is not the same as call depth and has to be
  // bounded separately: sixty nested unary operators inside a function that
  // recurses two hundred deep is twelve thousand C++ frames, and neither limit
  // alone catches it. Found by the fuzzer as a stack overflow.
  int eval_depth_ = 0;
  static constexpr int kMaxEvalDepth = 512;
  // Bounded because JS recursion is bounded by the C++ stack here, and a page
  // that recurses forever must get a RangeError rather than a segfault.
  static constexpr int kMaxCallDepth = 200;
  // Bounded because `while (true) {}` is a hang otherwise, and a page can
  // write one.
  // The label attached to the statement about to be evaluated, if it is a
  // labelled one. A loop takes it as its own, which is what makes `continue
  // outer` continue the outer loop rather than leave it -- the label has to
  // reach the loop, and the loop is not the labelled statement.
  std::string pending_label_;
  // Set by EvaluateMember when the base was `super`, so the call that follows
  // knows to look the method up on the parent while keeping `this`. Cleared on
  // every other member access, so it cannot leak into an unrelated call.
  Value super_base_;
  std::size_t steps_ = 0;
  static constexpr std::size_t kMaxSteps = 20'000'000;

  // Keeps a scope alive for the collector while it is on the C++ stack.
  class ScopeGuard {
   public:
    ScopeGuard(Interpreter& interpreter, Environment* scope) : interpreter_(interpreter) {
      interpreter_.active_scopes_.push_back(scope);
    }
    ~ScopeGuard() { interpreter_.active_scopes_.pop_back(); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

   private:
    Interpreter& interpreter_;
  };
};

}  // namespace microbrowser::js

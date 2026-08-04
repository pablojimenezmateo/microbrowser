#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Parser.h"
#include "js/RegExp.h"
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
  // A plain object, for a builtin that has to return a record rather than a
  // list -- a match's named groups being the first of them.
  Value NewObjectValue();
  // A native function as a value. Public because a builtin that returns an
  // *object with methods* -- an iterator, whose `next` closes over that
  // iterator's own state -- cannot make one otherwise.
  Value NewNativeValue(const char* name, NativeFunction function);

  // Wraps a compiled pattern as the JavaScript object a regex literal
  // evaluates to. Public for the same reason NewArrayValue is: a builtin --
  // and the literal's evaluation -- has no other way to make one.
  Value NewRegExpValue(RegExp pattern);
  // A pending promise. Public so the host can hand one out: a fetch that
  // resolves later is the shape every browser API takes.
  Value NewPromiseValue();
  // One iteration in progress.
  //
  // A cursor rather than a collected vector, because the protocol is
  // observable: a `for...of` that breaks early must stop asking, and an
  // iterator whose `next` has side effects must not be run past the break.
  // Collecting first would be simpler and wrong in exactly the cases a page
  // writes a custom iterator for.
  struct Iteration {
    // The fast path: an array whose `Symbol.iterator` is still the built-in
    // one, walked by index. Allocating an iterator object and calling a
    // function per element is what `for (const x of xs)` would otherwise cost,
    // and it is the most common loop in any page.
    Object* array = nullptr;
    std::size_t index = 0;
    // The general path.
    Value iterator;
    Value next;
  };
  // Begins iterating `iterable`, or throws the TypeError the spec throws for
  // something that is not iterable.
  Result OpenIteration(const Value& iterable, Iteration& state);
  // The next value, with `done` set when there is none. Public alongside
  // OpenIteration because spread, destructuring and `for...of` are three
  // callers in three files.
  Result StepIteration(Iteration& state, Value& value_out, bool& done);
  // Everything an iterable yields. Spread and a rest element both consume the
  // whole thing by definition, so draining is what the spec says there --
  // unlike `for...of`, which must stop asking at a `break`.
  Result CollectIterable(const Value& iterable, std::vector<Value>& out);
  // One queued microtask.
  //
  // A promise reaction, or a plain job from `queueMicrotask`. The three values
  // are held rather than a captured closure because a capture is invisible to
  // the collector, and a queued job keeps its handler and its result alive by
  // definition -- they are the only reason it is still queued.
  struct Microtask {
    // The handler. Not callable when a `then` had none for this outcome, which
    // is how a rejection passes through a `.then(f)` untouched.
    Value callee;
    Value argument;
    // The promise to settle with the handler's result. Undefined for a plain
    // job, which has no promise behind it.
    Value derived;
    // Whether `argument` is a rejection reason rather than a value.
    bool rejected = false;
  };
  void EnqueueMicrotask(Microtask task);
  // Runs the queue to empty.
  //
  // Called at the end of a turn -- after a script, after a host-driven call --
  // and never on a timer. That is what keeps promises inside the zero-idle-CPU
  // invariant: a microtask exists only because something already ran, so
  // draining costs a wakeup that was already happening and never asks for one.
  void DrainMicrotasks();
  bool HasPendingMicrotasks() const { return !microtasks_.empty(); }

  // The `Symbol.iterator` cell, so a caller can ask for the protocol hook
  // without going through the global object -- which a page can reassign.
  Object* SymbolIterator() const { return well_known_.symbol_iterator; }

  // The compiled pattern behind a RegExp object, or null for anything else.
  // This is what "is a regular expression" means here, and it is a stronger
  // question than looking for a `source` property: an ordinary object cannot
  // answer yes to it.
  const RegExp* RegExpOf(const Value& value) const;

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

  Value GetProperty(const Value& base, const PropertyKey& key);

 public:
  // The same lookup a `.` does, for a builtin that has to read a method off an
  // object it was handed -- a Map constructor calling its own `set` is the
  // case, and doing it any other way would miss a subclass that overrode it.
  Value GetPropertyValue(const Value& base, const PropertyKey& key) {
    return GetProperty(base, key);
  }

 private:
  Result SetProperty(const Value& base, const PropertyKey& key, const Value& value);

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
  void InstallArrayPrototype();
  void InstallFunctionPrototype();
  // RegExp.prototype and the regex-aware String methods, in their own
  // translation unit: they are one feature, and splitting them across the two
  // files that already exist would put the pattern-matching logic in the file
  // whose whole point is that it has none.
  void InstallRegExpPrototype();
  // Symbol, the well-known symbols, and the iterators the built-in types
  // publish. One feature: symbols exist here so that the iteration protocol
  // has a key no page can write out, and installing them apart would separate
  // the mechanism from its only current use.
  void InstallIteration();
  // Map and Set. After InstallIteration, because both take an iterable in
  // their constructor and both publish `Symbol.iterator`.
  void InstallCollections();
  // Promise, and the microtask queue it settles through.
  void InstallPromises();
  // `JSON.parse` and the URI functions. In their own translation unit because
  // both parse text a page handed over -- one of them usually straight off the
  // network -- and a parser belongs with the bounds that make it safe.
  void InstallJsonAndUri(Object* json);
  // `Math`, `Number` and `Date`. One translation unit because all three are
  // arithmetic wearing different names, and because two of them touch the
  // clock -- which is a fingerprinting surface and is argued about once there
  // rather than in three places.
  void InstallNumbers(Object* math);

  // The values the language requires to exist, allocated once and handed out.
  //
  // One member rather than seven, and one list rather than two. Every one of
  // these is also a GC root, and the old shape had the fields here and the
  // roots enumerated in MaybeCollect -- so adding one and forgetting the other
  // was a use-after-free that nothing would catch until a page allocated
  // enough. `Roots()` is now the only list, and it cannot disagree with
  // itself.
  struct WellKnown {
    Object* object_prototype = nullptr;
    Object* array_prototype = nullptr;
    Object* function_prototype = nullptr;
    // Where a string's methods live, so that `"a".trim` and
    // `String.prototype.trim` are the same function object. A string is a
    // primitive here rather than a boxed object, so GetProperty consults this
    // directly instead of walking a chain from a wrapper.
    Object* string_prototype = nullptr;
    Object* regexp_prototype = nullptr;
    Object* promise_prototype = nullptr;
    // Where a number's methods live. A number is a primitive here, like a
    // string, so GetProperty consults this directly rather than boxing.
    Object* number_prototype = nullptr;
    // Not a prototype, but the same category: the cell every iteration goes
    // through. Held here rather than looked up through the global `Symbol`,
    // which a page can reassign -- the protocol has to keep working when it
    // does.
    Object* symbol_iterator = nullptr;

    std::vector<Object*> Roots() const {
      return {object_prototype, array_prototype,   function_prototype, string_prototype,
              regexp_prototype, promise_prototype, symbol_iterator,    number_prototype};
    }
  };

  Heap heap_;
  Object* global_ = nullptr;
  Environment* global_scope_ = nullptr;
  WellKnown well_known_;
  // Pending jobs, oldest first. A GC root while queued.
  std::vector<Microtask> microtasks_;
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

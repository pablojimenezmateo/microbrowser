#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "js/Ast.h"
#include "js/BigInt.h"
#include "js/Bytecode.h"
#include "js/Heap.h"
#include "js/Parser.h"
#include "js/RegExp.h"
#include "js/Value.h"
#include "util/PerformanceCounters.h"

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
// Every module `source` names with a *static* `import` or a re-`export`, in
// order, unresolved.
//
// The host needs this because the resolver it is asked through is synchronous: it
// has to have fetched a module's whole static graph before evaluation starts, and
// the only way to know what is in that graph is to parse. Declared beside the
// interpreter rather than on it because it needs no interpreter at all -- it is a
// question about text.
std::vector<std::string> ModuleImportSpecifiers(std::string_view source);

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
  // The same, on the machine. Public alongside RunProgram because they are the
  // two answers to the same question and a caller that has one should be able
  // to reach the other.
  // `scope` is where the chunk's top-level names go. The global one for a
  // script; a module's own for a module, which is the first thing modules
  // were added to the language for.
  Result RunCompiled(const CompiledFunction& program, Environment* scope = nullptr);

  // --- Modules (Modules.cpp) ------------------------------------------------
  //
  // How a specifier becomes source. Loading is the *host's* job: a specifier's
  // meaning is a URL question -- relative to what, over which protocol, past
  // which privacy verdict -- and none of that belongs in a language engine.
  // The engine takes this and does the linking.
  //
  // False when the specifier does not resolve, which the engine turns into the
  // TypeError the language throws at the import.
  using ModuleResolver = std::function<bool(std::string_view specifier,
                                            std::string_view referrer,
                                            std::string& resolved, std::string& source)>;
  void SetModuleResolver(ModuleResolver resolver);

  // A dynamic `import()` the host will answer *later*.
  //
  // The resolver above is synchronous, which is right for a static import graph
  // -- the host can have the whole thing before evaluation starts -- and wrong
  // for `import()`, which a page reaches at a moment nobody could predict.
  // Fetching from inside the resolver would block the one loop this browser has,
  // which ADR 0011 exists to prevent, so the promise is handed back pending and
  // the host settles it when the graph is closed.
  //
  // The starter is given the promise. False means the host cannot start the load
  // at all -- an unparseable specifier, a scheme it will not fetch -- and the
  // engine rejects immediately rather than leaving a promise nobody will settle.
  using DynamicImportStarter = std::function<bool(std::string_view specifier,
                                                  std::string_view referrer, Object* promise)>;
  void SetDynamicImportStarter(DynamicImportStarter starter);

  // The host has made every module in `specifier`'s graph resolvable and is
  // asking for it to be linked, evaluated and the promise settled. Settles with
  // the module's namespace object, or rejects with whatever went wrong.
  //
  // Called from a later turn of the loop, which is the whole point: by then the
  // synchronous resolver can answer from sources the host already has.
  void SettleDynamicImport(Object* promise, std::string_view specifier,
                           std::string_view referrer);
  // Runs `source` as a module named `specifier`, loading and evaluating what it
  // imports first. Answers the module's namespace object.
  Result RunModule(std::string_view source, std::string_view specifier);

  Heap& GetHeap() { return heap_; }
  Object* Global() { return global_; }
  Environment* GlobalScope() { return global_scope_; }

  // Calls a callable value. Public because the host needs it -- an event
  // handler is a JS function the browser calls, not the other way round.
  Result CallFunction(const Value& callee, const Value& self,
                      const std::vector<Value>& arguments);
  // `new callee(...)`, for a caller that has the pieces as values. Public for
  // the reason CallFunction is, and named apart from the private Construct so
  // that adding it did not change what that one means.
  Result ConstructValue(const Value& callee, const std::vector<Value>& arguments) {
    return Construct(callee, arguments);
  }

  // True when `self` is the instance Construct just allocated for this call --
  // i.e. this native is the body of `new F(...)`. Bound functions need it:
  // `new bound()` must Construct the target and ignore the bound `this`, and
  // ConstructionTarget cannot tell because a bound function has no `.prototype`.
  bool IsConstructCall(const Value& self) const;

  // The call stack as text, for an Error's `stack`.
  //
  // Built from the machine's frames, which is a thing only the machine can do
  // -- a tree-walker's frames are C++ frames and carry no name. Public so the
  // host can attach one to an error it made itself.
  std::string CaptureStack(std::string_view kind, std::string_view message) const;

  // Builds an Error object with a message. Public so a native function can
  // throw the way JS code does.
  Value MakeError(std::string_view kind, std::string message);
  Result Throw(std::string_view kind, std::string message);

  // Anything written by `console.log`, in order. The host decides what to do
  // with it; collecting rather than printing is what keeps the engine
  // testable and keeps a page from writing to the terminal.
  const std::vector<std::string>& ConsoleOutput() const { return console_; }

  // A line the *host* wants on this interpreter's console, which is not the same thing as one the page
  // wrote. There is one such caller and it is the reason this exists: a dedicated worker has its own
  // interpreter on its own thread, so anything it logs lands in a console nobody ever reads. The line
  // crosses as text and is prefixed by the caller, never derived from a page's own formatting.
  void LogConsoleLine(std::string line) { console_.push_back(std::move(line)); }

  // An exception nobody caught, from a place where throwing is *reported* rather than propagated: an
  // event listener, an `on…` handler, an observer callback. It goes to the console line the host
  // collects, which is the only channel a page's own error has.
  //
  // Public because the callers are in `src/bindings`, and it exists because discarding these was
  // silently losing whole scripts -- see EventDispatch.cpp. `where` is a fixed string chosen by the
  // caller, never derived from the page, so it cannot echo an attacker's text into a log.
  // Carries the stack when the thrown value has one, which every error this
  // engine makes does. A message names the fault and not the place, and on a
  // page with a megabyte of minified script the place is the entire question --
  // a stack reads `at HS (@1814415)` and the offset goes straight into the
  // source. `PageScript::RunTiming` had this for a top-level throw and nothing
  // reached from here did, so the errors that were hardest to find were the
  // ones reported with the least.
  void ReportUncaught(const Value& error, const char* where) {
    std::string line = std::string("Uncaught (in ") + where + ") " + ToString(error);
    if (error.IsObject()) {
      if (const Value* stack = error.object->Get("stack")) {
        if (stack->type == ValueType::String) {
          line += "\n    " + stack->AsString();
        }
      }
    } else if (error.type == ValueType::String) {
      // MakeError falls back to a bare string when the heap cannot hold an
      // Error object -- which is exactly the OOM case that most needs a place.
      // CaptureStack still works: frames are on the machine, not the heap.
      const std::string stack = CaptureStack("RangeError", error.AsString());
      if (!stack.empty()) {
        line += "\n    " + stack;
      }
    }
    console_.push_back(std::move(line));
  }

  // Keeps a value alive for the collector while it is a C++ local.
  //
  // A `js::Value` in a C++ variable is invisible to the collector -- the rule
  // this whole file is written around -- and the places that break it are the
  // ones where a *completion* is carried across a collection: `RunCompiled`
  // takes the value a script threw, drains the microtask queue, and returns
  // it, and draining is where MaybeCollect runs. Nothing rooted that value, so
  // a page whose script threw with enough allocation behind it read freed
  // memory. youtube.com is where it showed, as a segfault reading `e.stack`.
  //
  // Public because `PageScript::RunTiming` holds a thrown completion across
  // `error` event dispatch (another drain), and must root the same way.
  class ValueRoot {
   public:
    ValueRoot(Interpreter& interpreter, const Value& value)
        : interpreter_(interpreter), rooted_(value.IsObject() || value.IsSymbol()) {
      if (rooted_) {
        interpreter_.active_objects_.push_back(value.object);
      }
    }
    ~ValueRoot() {
      if (rooted_) {
        interpreter_.active_objects_.pop_back();
      }
    }
    ValueRoot(const ValueRoot&) = delete;
    ValueRoot& operator=(const ValueRoot&) = delete;

   private:
    Interpreter& interpreter_;
    bool rooted_;
  };

  // The wall clock, in milliseconds since the epoch.
  //
  // Public and in one place because it is a *privacy* surface rather than a
  // convenience: millisecond resolution is what the spec requires and is as
  // far as this goes, and a second caller reaching for a finer clock would
  // hand every page a timing side channel. `Date.now()` and `new Date()` both
  // come through here, and so will `performance.now` when it exists.
  double NowMilliseconds() const;

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
  // Settles one, with a value or with what it threw. Public for the same
  // reason, and the pair is what makes a host-owned promise usable at all: a
  // host that can create one and not settle one can only hand a page something
  // that never resolves. A *resolve* rather than a fulfil, so settling with a
  // promise flattens -- which is what `async function f(){ return g() }` needs
  // and what a host settling with something a page handed it needs equally.
  //
  // Also what an async call's own promise goes through, which is why there is
  // one of these rather than two: two settle paths is two answers to what
  // resolving with a thenable does.
  void SettleAsyncResult(Object* promise, const Value& value, bool rejected);
  // A bigint, as a value. Its digits go beside the heap under a fresh cell,
  // which is what makes `1n === 1n` a comparison of digits rather than of
  // identity -- see ValueType::BigInt.
  Value NewBigIntValue(BigInt digits);
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
    // Set once the iterator has reported done. A destructuring pattern longer
    // than its iterable keeps asking for values, and asking a custom iterator
    // again after it finished is observable -- so it is asked once and the
    // answer remembered.
    bool done = false;
    // Whether `next` hands back a promise of `{value, done}` rather than the
    // pair itself, which is what `Symbol.asyncIterator` means. Only `for await`
    // opens one of these, and it is a flag rather than a second cursor type
    // because everything else about walking one is identical.
    bool is_async = false;
  };
  // The same, resolving `Symbol.asyncIterator` first and falling back to the
  // sync protocol -- which is the spec's own rule for `for await` and the
  // reason a loop over an array of promises works.
  Result OpenAsyncIteration(const Value& iterable, Iteration& state);
  // One step of an async iteration, as something to await.
  //
  // For a real async iterator that is the promise `next` returned, and `done`
  // cannot be answered yet -- it is inside the promise, which is why the loop
  // has a second branch after the await. For a sync one it is the next value
  // itself, and awaiting *that* is what the spec's wrapping does.
  Result StepAsyncIteration(Iteration& state, Value& out, bool& done);
  // Begins iterating `iterable`, or throws the TypeError the spec throws for
  // something that is not iterable.
  Result OpenIteration(const Value& iterable, Iteration& state);
  // The next value, with `done` set when there is none. Public alongside
  // OpenIteration because spread, destructuring and `for...of` are three
  // callers in three files.
  Result StepIteration(Iteration& state, Value& value_out, bool& done);
  // The same step, with a value to send in.
  //
  // Only `yield*` has one: `it.next(v)` is how a resumed delegation passes on
  // what its own caller sent, and every other loop over an iterator sends
  // nothing. A separate entry point rather than a defaulted argument, because
  // "sends nothing" and "sends undefined" are the same to a built-in iterator
  // and different to one a page wrote.
  Result StepIterationWith(Iteration& state, const Value& sent, Value& value_out, bool& done);
  // Hands a throw or a forced return to the iterator itself, which is what
  // makes `yield*` a relationship. `thrown` is the value; `is_return` picks
  // between its `return` and its `throw`. False when the iterator has no such
  // method, in which case the caller rethrows.
  bool ForwardToIterator(Iteration& state, const Value& thrown, bool is_return, Result& out,
                         bool& done);
  // The machine's IterateForward, in Iteration.cpp beside the protocol it
  // speaks. `finished` says the delegation is over and the value is what the
  // `yield*` expression is worth.
  Result ForwardToDelegate(const Value& thrown, bool& finished);
  // Closes every open cursor above `down_to`, innermost first: an iterator
  // that has not finished and has a `return` gets it called, which is what the
  // protocol says a loop leaving early owes it. What makes a `break` out of a
  // `for...of` over a generator finish the generator rather than leave its
  // frame filed for ever.
  Result CloseIterations(std::size_t down_to);
  // The same, for a throw that is unwinding past them.
  //
  // Separate because the error handling is the opposite: a `break` propagates
  // what an iterator's `return` throws, and a throw *discards* it -- the
  // original error is what the page needs to see, and a generator's `finally`
  // raising a second one must not replace it.
  void CloseIterationsQuietly(std::size_t down_to);
  // One cursor's share of that, and what the tree-walker's `for...of` calls --
  // its cursor is a C++ local rather than a slot on a stack. The caller is
  // responsible for keeping the cursor rooted across this: it runs the page's
  // `return`, which can allocate and therefore collect.
  Result CloseIterationCursor(Iteration& cursor);
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
    // Non-zero when this job is a suspended `await` to put back rather than a
    // handler to call. Driven from the drain directly rather than through a
    // native, so that the resumed body runs with nothing of the drain's in a
    // C++ local -- which is what lets a collection happen inside it.
    std::uint64_t suspension = 0;
    // True when the job was queued while a CSP-trusted script context was
    // active, so promise continuations from a permitted script inherit trust.
    bool trust_scripts = false;
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

  // CSP transitive trust: bindings install these so promise reactions queued
  // during a permitted script run with trusted `<script>` insertion enabled.
  void SetTrustedScriptHooks(void* context, bool (*active)(void* context),
                             void (*apply)(void* context, bool push));

  // When non-null and returning true, `eval` / `Function` throw EvalError
  // (CSP without `'unsafe-eval'`). Null / false keeps the platform default.
  void SetEvalForbidden(void* context, bool (*forbidden)(void* context)) {
    eval_forbidden_context_ = context;
    eval_forbidden_ = forbidden;
  }

  // Fresh step budget for a host *task* (HTML event-loop task), when the
  // machine is idle. Used for timers/rAF-adjacent work that must not reset
  // under live frames. Fetch/XHR use `BeginNetworkTask` instead; trusted
  // pointer/key use `BeginInputTask`.
  void BeginTask() { BeginHostTurn(); }

  // A network completion is always a distinct host task — even when the engine
  // delivers it re-entrantly from `RunDueWork` under live frames (SABR `then`
  // after a soft-nav stamp). `BeginHostTurn` no-ops while frames are live, which
  // left youtube's googlevideo handler sharing a spent budget and never reaching
  // `appendBuffer` (TD-0042). Always zeros `steps_`; counts when frames were live.
  //
  // Soft-nav (TD-0049) still exhausted the *ordinary* ceiling inside the SABR
  // `then` (UMP → `appendBuffer`): cold `/watch` peaks ~10M, soft-nav hits
  // `steps_exhausted` at 20M with zero appends. Raise to `kMaxInputSteps` for
  // the delivery only — same ceiling as trusted input, restored afterward.
  std::size_t BeginNetworkTask();
  void EndNetworkTask(std::size_t previous_limit);

  // Trusted user input (click/key). Like `BeginNetworkTask` it always zeros
  // `steps_` under live frames, and it raises the hang-guard ceiling for that
  // dispatch: youtube's search-thumb `click` does more than `kMaxSteps` of
  // Polymer work before `preventDefault`, so the SPA never stamped and the
  // engine followed `a#thumbnail` as a document navigation (TD-0045).
  // Returns the previous `steps_limit_` for `InputTaskBudget` to restore.
  std::size_t BeginInputTask();
  // Restores the previous hang-guard ceiling after `BeginInputTask`. Zeros
  // `steps_` when they sit above that ceiling — otherwise a long click leaves
  // the soft-nav stamp born exhausted (TD-0049).
  void EndInputTask(std::size_t previous_limit);

  // Hang guard for `while (true) {}`, not a fairness scheduler (ADR 0036).
  // Public so tests and counters can name the ordinary vs input ceilings.
  static constexpr std::size_t kMaxSteps = 20'000'000;
  // Trusted click/key may run a page's SPA router before `preventDefault`
  // (TD-0045). Five ordinary budgets is enough for youtube search→watch; timers
  // and stamps keep `kMaxSteps` so a runaway rAF cannot open through this.
  static constexpr std::size_t kMaxInputSteps = kMaxSteps * 5;
  // Ceiling for one sync NestedHostBudget chain (MSE re-entry or CE-in-CE).
  // Five full budgets is enough for SABR; unbounded resets would re-open TD-0018.
  static constexpr std::size_t kMaxNestedHostChainSteps = kMaxSteps * 5;
  std::size_t StepsLimit() const { return steps_limit_; }
  // Test seam for TD-0049: EndInputTask must clear a count left above the
  // restored ceiling. Not for production call sites.
  std::size_t StepsForTesting() const { return steps_; }
  void SetStepsForTesting(std::size_t steps) { steps_ = steps; }

  // RAII for `BeginInputTask` / `EndInputTask` around one trusted dispatch.
  class InputTaskBudget {
   public:
    explicit InputTaskBudget(Interpreter& interpreter)
        : interpreter_(interpreter), previous_limit_(interpreter_.BeginInputTask()) {}
    ~InputTaskBudget() { interpreter_.EndInputTask(previous_limit_); }
    InputTaskBudget(const InputTaskBudget&) = delete;
    InputTaskBudget& operator=(const InputTaskBudget&) = delete;

   private:
    Interpreter& interpreter_;
    std::size_t previous_limit_;
  };

  // RAII for fetch/XHR delivery (TD-0049). Same raised ceiling as input; timers
  // and stamps stay on `kMaxSteps`.
  class NetworkTaskBudget {
   public:
    explicit NetworkTaskBudget(Interpreter& interpreter)
        : interpreter_(interpreter), previous_limit_(interpreter_.BeginNetworkTask()) {}
    ~NetworkTaskBudget() { interpreter_.EndNetworkTask(previous_limit_); }
    NetworkTaskBudget(const NetworkTaskBudget&) = delete;
    NetworkTaskBudget& operator=(const NetworkTaskBudget&) = delete;

   private:
    Interpreter& interpreter_;
    std::size_t previous_limit_;
  };

  // Fresh hang-guard allotment for host work that must run while script frames
  // are still live. `BeginHostTurn` refuses to reset in that case (nested
  // import stamp hang), but two call sites need a refresh anyway:
  //
  // 1. MSE updateend → appendBuffer → updateend before appendBuffer returns
  //    (TD-0020 / youtube SABR → `fmt.unplayable`).
  // 2. Custom-element upgrades from inside an rAF/script stamp (TD-0018 /
  //    youtube search thumbs): `BeginTask` in UpgradeElement was a no-op under
  //    live frames, so the first lazy-list chunk shared one spent budget and
  //    never reached `u5m`/`IntersectionObserver.observe`.
  //
  // Cap across the reentrant chain so a storm cannot open through nesting.
  class NestedHostBudget {
   public:
    explicit NestedHostBudget(Interpreter& interpreter,
                              util::PerfCounterId reset_counter)
        : interpreter_(interpreter) {
      interpreter_.EnterNestedHostBudget(reset_counter);
    }
    ~NestedHostBudget() { interpreter_.LeaveNestedHostBudget(); }
    NestedHostBudget(const NestedHostBudget&) = delete;
    NestedHostBudget& operator=(const NestedHostBudget&) = delete;

   private:
    Interpreter& interpreter_;
  };

  // MSE / media-element events — same NestedHostBudget with a media counter.
  class MediaEventBudget {
   public:
    explicit MediaEventBudget(Interpreter& interpreter)
        : budget_(interpreter, util::PerfCounterId::JsMediaEventBudgetResets) {}
    MediaEventBudget(const MediaEventBudget&) = delete;
    MediaEventBudget& operator=(const MediaEventBudget&) = delete;

   private:
    NestedHostBudget budget_;
  };

  // Custom-element upgrade while a stamp frame is live.
  class ElementUpgradeBudget {
   public:
    explicit ElementUpgradeBudget(Interpreter& interpreter)
        : budget_(interpreter, util::PerfCounterId::JsElementUpgradeBudgetResets) {}
    ElementUpgradeBudget(const ElementUpgradeBudget&) = delete;
    ElementUpgradeBudget& operator=(const ElementUpgradeBudget&) = delete;

   private:
    NestedHostBudget budget_;
  };

  // The `Symbol.iterator` cell, so a caller can ask for the protocol hook
  // without going through the global object -- which a page can reassign.
  Object* SymbolIterator() const { return well_known_.symbol_iterator; }
  // The same, for the two hooks a builtin rather than an operator consults:
  // `Object.prototype.toString` reads the tag, and `Array.prototype.concat`
  // will read the spread flag.
  Object* SymbolToStringTag() const { return well_known_.symbol_to_string_tag; }
  // The method a page's own object may supply to stand in for a pattern.
  //
  // `'abc'.replace(x, y)` asks `x` for `Symbol.replace` before doing anything
  // else, and so do `match`, `matchAll`, `split` and `search`. That is how a
  // library object -- a template, a tokenizer, an internationalised matcher --
  // is used where a RegExp would be, and refusing to look is what makes those
  // libraries silently stringify instead.
  //
  // Null when the value has no such method, which is every string and every
  // ordinary object. Looked up through the interpreter so a getter runs.
  Object* PatternProtocol(const Value& value, const char* which);
  // Where a buffer a typed array allocated for itself gets its methods.
  Object* ArrayBufferPrototype() const { return well_known_.array_buffer_prototype; }

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
  // Whether a value is the marker a short-circuited optional chain travels
  // as, and the marker itself. Every link in a chain checks the first before
  // doing anything with what it was handed, and the link the parser marked as
  // the chain's root turns it into undefined.
  bool IsChainSignal(const Value& value) const {
    return value.IsObject() && value.object == well_known_.chain_signal;
  }
  Value ChainSignal() const {
    return well_known_.chain_signal == nullptr ? Value::Undefined()
                                               : Value::Obj(well_known_.chain_signal);
  }
  Result EvaluateForIn(const Node& node, Environment& scope);
  // Where an update's new value goes. Lifted out because a bigint increments
  // as a bigint and a number as a number, and only the arithmetic differs.
  Result StoreUpdate(const Node& node, const Node& operand, const Value& value,
                     Environment& scope);
  // `enclosing` is the chunk the class literal was compiled into, or null when
  // the tree-walker is building it. When it is there, each method's body is
  // taken from it already compiled; the *building* is the same either way,
  // because the ordering a class needs is the subtle part and there should be
  // one of it.
  Result EvaluateClass(const Node& node, Environment& scope,
                       const CompiledFunction* enclosing = nullptr);
  // The compiled chunk inside `enclosing` that came from `source`, or null.
  static const CompiledFunction* FindCompiled(const CompiledFunction* enclosing,
                                              const Node* source);
  // A function object over a compiled body. The counterpart of NewFunction,
  // which makes one over an AST body.
  Value NewCompiledFunction(const CompiledFunction& code, Environment& scope, bool arrow);
  // Two cases lifted out of Evaluate's switch, and not for tidiness.
  //
  // Evaluate recurses, so every local in it is paid for at every level of
  // expression nesting -- a case body holding a string and two vectors makes
  // the frame bigger even in the deep recursion that never reaches that case.
  // Adding these two inline pushed the depth-bound test past the stack under
  // ASan, which is what a frame that grew by a few hundred bytes looks like
  // from the outside.
  Result EvaluateRegExpLiteral(const Node& node);
  Result EvaluateTaggedTemplate(const Node& node, Environment& scope);
  // `new C(...)`: a fresh instance with C's prototype, the field ordering a
  // class needs, and the rule that a constructor returning an object replaces
  // the instance. Shared by the tree-walker's New and the machine's, so the
  // ordering that lets a derived field read a base one is written once.
  Result Construct(const Value& callee, const std::vector<Value>& arguments);
  // What to call the thing that turned out not to be a function: its value,
  // and the name the compiler recorded for the call when it knew one.
  std::string DescribeCallee(const CompiledFunction& code, std::uint32_t at,
                             const Value& callee) const;
  // Runs a class's instance field initializers against a fresh instance.
  // Separate from the constructor because fields run *before* the constructor
  // body and after any super() call, and folding them in loses that ordering.
  Result InitializeFields(Object* instance, Object* constructor);
  Value BoundThisAfterSuper(const Value& instance, const Result& super_result,
                            Value* rebind_self = nullptr);  // after super()

  // Declares the function declarations in a statement list before running it,
  // which is what makes a function callable above where it is written.
  void HoistDeclarations(const Node& list, Environment& scope);
  // Every `var` a function body declares, bound to undefined before the body
  // runs. Separate from HoistDeclarations because it runs at a different
  // boundary: function declarations hoist to their block, `var` to the
  // function, and calling both from one place would give `var` block scope.
  void HoistVars(const Node& body, Environment& scope);

  Result BindPattern(const Node& target, const Value& value, Environment& scope, bool declare,
                     bool is_const);
  Result BindParameters(const Node& parameters, const std::vector<Value>& arguments,
                        Environment& scope);

  // One binary operator applied to two already-evaluated operands. Shared with
  // the compiler's `Binary` opcode, so `+` has one answer rather than two that
  // can drift. Lives in Operators.cpp.
  Result ApplyBinary(BinaryOp op, const Value& a, const Value& b);
  // The half of an operator that runs when either side is a bigint. `handled`
  // says whether it answered; when it did not, the ordinary numeric path runs.
  // Separate because the rule is *not* a conversion -- mixing a bigint and a
  // number is a TypeError, so the numeric path must not get the chance.
  Result ApplyBigIntBinary(BinaryOp op, const Value& a, const Value& b, bool& handled);
  // The machine's numeric unaries -- Negate, UnaryPlus, BitNot, ToNumberOp and
  // StepValue -- beside the binary ones, because both halves have the same
  // rule about bigints and one of them would drift.
  Result ApplyNumericUnary(Op op, std::uint32_t operand, const Value& value);

 public:
  // The unary operators on a bigint: `-x`, `~x`, and the `+1` an update does.
  // `handled` says whether it answered, for the reason the binary one has it.
  // Public because both engines' unary paths reach it.
  Result ApplyBigIntUnary(const char* op, const Value& operand, bool& handled);

 private:

 public:
  // --- The conversions that can run script (Operators.cpp) -----------------
  //
  // `ToNumber` and `ToString` in Value.h are pure functions and answer NaN and
  // "[object Object]" for every object, because they have no interpreter to
  // call `valueOf` with. That is the wrong answer for `[] + {}`, for `+[]`,
  // for `date - date` and for every page that defines one -- and it is wrong
  // *quietly*, which is worse than throwing.
  //
  // These four are the spec's versions: they can call into script, so they can
  // throw, so they return a Result. The pure ones stay for the places that
  // genuinely have a primitive already -- a number's own `toString`, a
  // console line -- and calling one of those on a value that might be an
  // object is now the bug to look for.
  enum class Hint : std::uint8_t { Default, Number, String };
  // OrdinaryToPrimitive with the `Symbol.toPrimitive` override in front of it.
  // The hint decides the order `valueOf` and `toString` are tried in, and
  // Default means Number for everything except a Date.
  Result ToPrimitive(const Value& value, Hint hint, Value& out);
  Result ToNumberOf(const Value& value, double& out);
  Result ToStringOf(const Value& value, std::string& out);
  // ToPropertyKey: a computed access converts its key through ToPrimitive with
  // a string hint, so `o[{toString(){return 'a'}}]` reads `o.a`.
  Result ToKeyOf(const Value& value, PropertyKey& out);
  // `==` where either side may be an object. The pure LooseEquals in Value.h
  // answers false for every object it is not identical to; this one coerces,
  // which is what makes `[1] == 1` true.
  Result LooseEqualsOf(const Value& a, const Value& b, bool& out);

 private:

  // --- The virtual machine -------------------------------------------------
  //
  // In Vm.cpp. The loop runs frames until the one it was entered for pops,
  // which is what makes a JavaScript-to-JavaScript call an ordinary push
  // rather than C++ recursion.
  Result RunFrames(std::size_t entry_depth);
  // Pushes a frame for a compiled function, with the callee, the receiver and
  // the arguments already on the value stack at `callee_slot`.
  Result PushFrame(Object* function, std::size_t callee_slot, std::uint32_t argument_count);
  // Runs a compiled function to completion from a C++ caller -- a native that
  // called back into script, the host dispatching an event. Re-entrant: it
  // pushes a frame on the same stacks and runs until that one returns.
  Result CallCompiled(Object* function, const Value& self,
                      const std::vector<Value>& arguments);
  // Unwinds to the innermost handler that covers the throw, or out of the VM
  // when there is none. False when nothing caught it.
  //
  // A forced return goes through here too, carrying a value marked by
  // `return_signal`. It differs in exactly two ways, both below: no `catch`
  // may see it, and reaching the end of a generator's frame completes the
  // generator instead of letting the value out.
  bool UnwindToHandler(const Value& thrown, std::size_t entry_depth);
  // Drop every frame above `entry_depth` without running catch/finally. Used
  // when the step-budget hang guard aborts a turn that already absorbed one
  // RangeError — leaving those frames made the next `Run` share a spent
  // budget (`BeginHostTurn` sees non-empty frames and refuses to reset).
  void AbandonFrames(std::size_t entry_depth);
  // The value a forced return travels as, carrying what the generator should
  // return. Null when the heap is full, which the caller turns into dropping
  // the frame -- the answer this was trying to improve on.
  Value NewReturnSignal(const Value& value);
  // Whether a thrown value is one of those. The one place the difference
  // between a throw and a forced return is observable inside the machine.
  bool IsReturnSignal(const Value& thrown) const;
  // Everything the VM is holding, as roots. Appended to what MaybeCollect
  // already gathers -- and the reason it can now run mid-evaluation at all.
  void GatherVmRoots(std::vector<Object*>& objects, std::vector<Environment*>& scopes) const;
  // The same, for the calls that are waiting. A suspended frame's values are
  // reachable from nothing else -- they were taken off the stacks GatherVmRoots
  // walks -- so this is the only thing keeping them alive.
  void GatherSuspensionRoots(std::vector<Object*>& objects,
                             std::vector<Environment*>& scopes) const;
  // The scope instructions read and write. The frame's own scope when it has
  // pushed none of its own.
  Environment* CurrentScope();

  // --- Suspending a call (Async.cpp) ---------------------------------------
  //
  // What `await` does, and the one thing a tree-walker cannot be made to do:
  // its state is C++ stack frames and there is nowhere to put one. Here a
  // frame is a record and every stack it points into is a vector, so
  // suspending is copying five slices out and putting them back later.
  //
  // Takes the running frame off the machine, files it, arranges for the
  // awaited value to put it back, and leaves the call's promise where its
  // result would have gone -- so the caller carries on with a promise from the
  // moment the body first waits.
  Result SuspendForAwait(const Value& awaited);
  // The half of a suspend that `await` and `yield` share: lift the running
  // frame and its slice of every stack off the machine, file it, and leave
  // `result` where the call's result would have gone. Returns the id it was
  // filed under, or zero when the cap was reached and there was nowhere to
  // put it.
  std::uint64_t FileRunningFrame(Value result);
  // The other half of a suspend is PutFrameBack, declared below beside the
  // Suspension record it moves, because that is where the type it takes is.
  // Puts a filed frame back and runs it until it returns or suspends again.
  // `rejected` throws the value at the `await` rather than handing it over,
  // which is how `try { await p } catch` sees a rejection.
  Result ResumeSuspended(std::uint64_t suspension, const Value& value, bool rejected);
  // The same suspend, with a generator's trigger instead of a promise's.
  //
  // Files the running frame, records where it went on the generator, and
  // leaves `value` where the call's result would have gone -- which is what
  // the resumer reads back as the yielded value. `status` is what the
  // generator is left in: Suspended for a `yield`, and Start for the
  // GeneratorEntry that runs before the body does, so that a `throw` before
  // the first `next` does not start a body it should not.
  Result SuspendForYield(const Value& value, GeneratorState::Status status);
  // Puts a generator's filed frame back and runs it until it yields, returns
  // or throws. `thrown` throws the value at the `yield` rather than handing it
  // over, which is what `generator.throw(e)` is.
  //
  // What came back -- yield or return -- is not in the Result: both are a
  // value. The generator's own status is what says which, because `yield` is
  // the thing that sets it and a return is the thing that does not.
  Result ResumeGenerator(Object* generator, const Value& sent, bool thrown);
  // A generator object, with nothing filed against it yet. Made by PushFrame
  // before the body runs, for the reason an async call's promise is: the
  // caller has to be handed one whatever the body does next. The flag picks
  // which of the two prototypes it gets, which is the only thing that differs
  // between the two kinds from the object's side.
  Object* NewGenerator(bool is_async);
  // Installs %GeneratorPrototype% and %AsyncGeneratorPrototype%. In Async.cpp,
  // beside what they drive.
  void InstallGeneratorPrototype();
  // --- Async generators ------------------------------------------------------
  //
  // One `next`, `throw` or `return` an async generator has been asked for and
  // has not answered yet.
  //
  // An async generator's methods hand back a promise immediately and settle it
  // when the body gets there, so a second `next` before the first has settled
  // cannot resume the frame -- it is already on the machine, or awaiting. It
  // queues instead, which is what makes `Promise.all([it.next(), it.next()])`
  // give two values in order rather than one value and a TypeError.
  struct AsyncRequest {
    enum class Kind : std::uint8_t { Next, Throw, Return };
    Object* promise = nullptr;
    Value value;
    Kind kind = Kind::Next;
  };

  //
  // Both suspends at once, which is the whole of what makes them their own
  // thing rather than a flag: the body stops at a `yield` and at an `await`,
  // and only the first of those has anything to hand back. So a call's answer
  // is not one promise made once, it is one promise per request -- and the
  // requests queue, because a second `next` cannot resume a frame that is
  // already on the machine.
  //
  // Queues a request and pumps. What every method on the prototype does, with
  // the kind being the only difference between them.
  Value EnqueueAsyncRequest(Object* generator, AsyncRequest::Kind kind, const Value& value);
  // Answers as many queued requests as it can without waiting: resumes the
  // frame when one is suspended at a `yield`, and settles straight away when
  // the generator is finished. Does nothing while a resume is in flight, which
  // is what keeps the frame from being put back twice -- whoever finishes that
  // resume pumps again.
  void PumpAsyncGenerator(Object* generator);
  // Settles the request at the front of the queue with `{value, done}`, or
  // rejects it. What a `yield`, a `return` and a throw out of an async
  // generator body each do, and the reason each of them has to find the
  // generator through its frame.
  void SettleAsyncRequest(Object* generator, const Value& value, bool done, bool rejected);
  // Settles every request still queued on a finished generator, which is what
  // a `return` or a throw owes the ones behind it.
  void DrainAsyncRequests(Object* generator, bool rejected, const Value& reason);
  // Whether this frame belongs to an async generator, which is the pair of
  // flags rather than either one: an async function has a promise, a generator
  // has a generator, and this has both.
  static bool IsAsyncGeneratorFrame(const Frame& frame);
  // Makes a suspended generator return, running every `finally` between the
  // `yield` it stopped at and the end of its body. What `it.return(v)` is, and
  // what a `for...of` that breaks does to a generator through IterateClose.
  // The Result is what the body finally returned, which a `finally` of its own
  // can change.
  Result ReturnFromGenerator(Object* generator, const Value& value);
  // Completes a generator without resuming it, and drops the frame it had
  // filed. The answer for one that never started or has already finished, and
  // the fallback when there is no room to put its frame back -- the filed frame
  // is a root, so a generator nobody finishes has to be finished by somebody.
  void CloseGenerator(Object* generator);
  // Arranges for `value` to resume suspension `id` once it settles, treating a
  // non-promise as an already-resolved one -- so `await 1` still yields a turn,
  // which is what the language says and what a page's ordering depends on.
  // In Promises.cpp, where the reaction machinery lives.
  void AwaitOn(const Value& value, std::uint64_t suspension);
  // The binding a resolved slot names, in whichever of the two places the
  // running frame keeps its bindings. Null when the slot was never reserved,
  // which is a compiler bug rather than a program one; an unset slot comes
  // back with `live` false, which is the language's temporal dead zone.
  Binding* SlotBinding(const Frame& frame, std::uint32_t packed);
  // One of the four names a frame's own scope always has room for -- `this`,
  // `__home__`, `__function__` -- looked up where it lives and then outwards.
  // The walk out is what makes `super` work inside an arrow inside a method:
  // the arrow's own slot is reserved and unset, so the method's is found.
  Value* FrameName(std::string_view name, std::uint32_t slot);

  // A proxy's handler, and the target behind it. Null for anything else, which
  // is what every property operation checks before doing its ordinary work.
  Object* ProxyTrap(const Value& base, const char* trap, Value& target) const;

  // The read a `.` or a `[]` does.
  //
  // `abrupt`, when given, is where a **throw from inside the read** goes: a `get`
  // accessor and a `Proxy` get trap are both calls, and a call can throw. This
  // returns a `Value` because 73 call sites want one, so the exception cannot be the
  // return -- and before this parameter existed it was simply dropped, which turned
  // `get x() { throw ... }` into a property that quietly reads `undefined`.
  //
  // Every caller that is *inside* an evaluation -- the tree-walker's member read, the
  // machine's GetProperty opcodes, destructuring, iteration -- passes it and
  // propagates. A builtin that reads a property of its own argument may pass null,
  // and then a throwing getter is swallowed exactly as it was before; those are the
  // sites left to convert, and they are listed in docs/js-conformance-roadmap.md.
  Value GetProperty(const Value& base, const PropertyKey& key, Result* abrupt = nullptr);

 public:
  // `delete base[key]`, and the own keys an enumeration sees.
  //
  // Both exist because of Proxy: a delete goes to the `deleteProperty` trap
  // and an enumeration to `ownKeys`, and every caller that reached for
  // `object->Delete` or `object->Keys` directly was a place a proxy became
  // visible. Public for the reason GetPropertyValue is -- the host enumerates
  // and deletes too.
  bool DeleteProperty(const Value& base, const PropertyKey& key);
  std::vector<std::string> OwnKeys(const Value& base, bool enumerable_only);

 private:

 public:
  // The same lookup a `.` does, for a builtin that has to read a method off an
  // object it was handed -- a Map constructor calling its own `set` is the
  // case, and doing it any other way would miss a subclass that overrode it.
  Value GetPropertyValue(const Value& base, const PropertyKey& key) {
    return GetProperty(base, key);
  }
  // The same read, with the throw. A host that reads a property of an object a
  // page handed it -- a `NodeFilter`'s `acceptNode`, an init dictionary's
  // member -- has to see a getter that throws rather than swallow it into
  // `undefined`, which is a wrong answer wearing a plausible one. The
  // three-argument form above is private because the *inside* of an evaluation
  // must always propagate; this is the door for callers outside one.
  Value GetPropertyOrThrow(const Value& base, const PropertyKey& key, Result& abrupt) {
    return GetProperty(base, key, &abrupt);
  }
  // And the write, for the same reason and a sharper one: `Object.assign` used
  // `object->Set` directly, which stores a slot and skips a setter on the
  // prototype chain and a proxy's `set` trap. The specification says assign
  // *invokes* the setter, and a host that installs accessors on a prototype --
  // which is what every reflected DOM attribute is -- had every
  // `Object.assign(element, {...})` silently write a plain property onto the
  // wrapper instead of the element.
  Result SetProperty(const Value& base, const PropertyKey& key, const Value& value);

 private:

  Object* NewObject();
  Object* NewArray(std::vector<Value> elements);
  Object* NewArray(std::vector<Value> elements, std::vector<bool> present);
  Value NewFunction(const Node& node, Environment& scope, bool arrow);
  // Wraps a C++ callable as a JS function object, and puts one on an object
  // under a name. Every builtin is installed through these two.
  Object* NewNative(const char* name, NativeFunction function);
  void InstallNative(Object* target, const char* name, NativeFunction function);
  void InstallGlobals();
  // `console`, in ConsoleBuiltins.cpp. Apart from the rest because it is the
  // host's diagnostic surface rather than part of the language.
  void InstallConsole();
  // String.prototype, in its own translation unit: it is the largest single
  // group of builtins and Builtins.cpp is already near the module's TU limit.
  void InstallStringPrototype(Object* string_constructor);
  void InstallArrayPrototype();
  // `Error` and the seven NativeError kinds, in their own translation unit:
  // they are one feature and the only builtins whose *constructors* form a
  // prototype chain, which is the part that is invisible until something walks
  // it.
  void InstallErrors();
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
  // `Date`, in its own translation unit. Forty-five methods, a calendar, a
  // parser and a formatter -- and the calendar is computed here rather than
  // asked of the platform, because a page can name the year 275760 and
  // `time_t` cannot hold it.
  void InstallDate();
  // `ArrayBuffer`, the nine typed arrays and `DataView`. One translation unit
  // because they are one feature: a buffer is bytes, a typed array is a window
  // onto them with an element type, and a DataView is a window told its type
  // per access.
  void InstallTypedArrays();

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
    // And a boolean's, on exactly the same terms. Two methods live on it and
    // both matter more than their size suggests: `true.toString()` is what
    // ToPrimitive reaches for, so without this a boolean in a string context
    // is a TypeError rather than "true".
    Object* boolean_prototype = nullptr;
    // Where a bigint's methods live. A bigint is a primitive here, like a
    // number, so GetProperty consults this directly rather than boxing.
    Object* bigint_prototype = nullptr;
    // The two the typed arrays need to find again: a typed array made without
    // a buffer allocates one and has to give it the right prototype, and the
    // nine constructors share one prototype between them.
    Object* array_buffer_prototype = nullptr;
    Object* typed_array_prototype = nullptr;
    // Where `next`, `throw` and `return` live, and the `Symbol.iterator` that
    // returns the generator itself. One object shared by every generator
    // rather than one per generator function -- so `Object.getPrototypeOf(g())`
    // is this rather than `gen.prototype`, which is the one place a page could
    // tell and is not a place any page looks.
    Object* generator_prototype = nullptr;
    // The prototype of the value a forced return travels as.
    //
    // Making a generator return means running every `finally` between the
    // `yield` it stopped at and the end of its body, and the only run-time path
    // that does that is the one a throw takes. So a forced return *is* a throw,
    // of a value nothing else can produce -- this is the marker that says so,
    // and the unwinder reads it to know that no `catch` may see it.
    //
    // A page cannot reach this object: it is never a property of anything it
    // can name, and the only values carrying it are handed to finalizers,
    // which do not receive them.
    Object* return_signal = nullptr;
    // The same for an async generator, and a separate object rather than the
    // one above because every method on it differs: `next` hands back a
    // promise of `{value, done}` rather than the pair itself, and the hook it
    // answers to is `Symbol.asyncIterator`.
    Object* async_generator_prototype = nullptr;
    // Not a prototype, but the same category: the cell every iteration goes
    // through. Held here rather than looked up through the global `Symbol`,
    // which a page can reassign -- the protocol has to keep working when it
    // does.
    Object* symbol_iterator = nullptr;
    // What `for await` resolves against, held for the reason above.
    Object* symbol_async_iterator = nullptr;
    // What a short-circuited optional chain travels as, in the tree-walker.
    //
    // `a?.b.c` with a nullish `a` is undefined for the whole expression, so
    // the innermost link has to tell the ones outside it to give up too --
    // and a tree-walker's links are C++ frames, which can only say so with a
    // value. One shared object rather than one per short-circuit, compared by
    // identity; the parser marks where the chain ends, and that mark is where
    // it turns back into undefined.
    //
    // A page cannot reach it: it is never a property of anything nameable,
    // and every path that could return it converts it first.
    Object* chain_signal = nullptr;
    // The three hooks an *operator* consults, held for the reason the two
    // above are: `+`, `instanceof` and `Object.prototype.toString` have to
    // find them whatever a page did to the global `Symbol`.
    Object* symbol_to_primitive = nullptr;
    Object* symbol_has_instance = nullptr;
    Object* symbol_to_string_tag = nullptr;

    std::vector<Object*> Roots() const {
      return {object_prototype,    array_prototype,   function_prototype,  string_prototype,
              regexp_prototype,    promise_prototype, symbol_iterator,     number_prototype,
              generator_prototype, symbol_async_iterator, async_generator_prototype,
              return_signal,       symbol_to_primitive, symbol_has_instance,
              symbol_to_string_tag, boolean_prototype,  chain_signal,
              array_buffer_prototype, typed_array_prototype, bigint_prototype};
    }
  };

  // Everything the machine holds while it runs, grouped.
  //
  // One member rather than five, for the reason WellKnown is one member rather
  // than seven: every field here is also a GC root, and a root list that lives
  // apart from the fields is a use-after-free waiting for a page to allocate
  // enough. GatherVmRoots is next to these and walks all of them.
  //
  // The value stack is reserved once and never grows, so that a Value* taken
  // into it stays valid across a push. Overflowing it is the same RangeError
  // as running out of call depth, because from a page's side it is.
  struct VmState {
    std::vector<Value> stack;
    std::vector<Frame> frames;
    // The bindings of every frame whose scopes nothing can capture, one
    // contiguous slice per frame. Reserved once like the value stack, and for
    // the same reason: an instruction holds a Binding* into it while it runs.
    std::vector<Binding> locals;
    // Block scopes pushed by the running frames, innermost last. One vector
    // shared by every frame, with each frame recording where its own start --
    // a vector per frame would be a heap allocation per call.
    std::vector<Environment*> scopes;
    // Open `for...of` cursors, the same way.
    std::vector<Iteration> iterations;
    // Every program compiled so far. Held for the life of the interpreter for
    // the reason the ASTs are: a function object points at its code, and every
    // callback outlives the script that made it.
    std::vector<std::unique_ptr<CompiledFunction>> programs;
  };

  // One call that is waiting on an `await`.
  //
  // A frame and its slice of each of the machine's stacks, lifted out whole.
  // The frame alone is not enough: an `await` can happen half way through an
  // expression, inside three blocks, with two `for...of` cursors open, and all
  // of that has to be there when it resumes. Which is the same list the
  // handler table records depths of, for the same reason.
  struct Suspension {
    Frame frame;
    std::vector<Value> stack;
    std::vector<Environment*> scopes;
    std::vector<Iteration> iterations;
    std::vector<Binding> locals;
  };

  // Rebases a filed frame onto whatever the machine looks like now and pushes
  // it. False when there is no room, which the caller has to turn into the
  // right kind of failure -- a rejected promise for an awaiting call, a
  // completed generator for an iterator. The other half of FileRunningFrame,
  // and here rather than beside it because this is where its parameter's type
  // is defined.
  bool PutFrameBack(Suspension& held);

  // Every call waiting, by id.
  //
  // Keyed rather than pointed at, because what holds a suspension alive is a
  // promise reaction, and a reaction is an object a page can reach. An id is a
  // number it can do nothing with; a pointer would be a pointer.
  //
  // A suspension whose promise never settles is never resumed and never freed,
  // which is a leak a page can ask for -- so the count is capped and `await`
  // past the cap throws, the same answer as running out of call depth.
  // Every call waiting, by id, and every request waiting on one.
  //
  // Keyed rather than pointed at, because what holds a suspension alive is a
  // promise reaction, and a reaction is an object a page can reach. An id is a
  // number it can do nothing with; a pointer would be a pointer.
  //
  // A suspension whose promise never settles is never resumed and never freed,
  // which is a leak a page can ask for -- so the count is capped and `await`
  // past the cap throws, the same answer as running out of call depth.
  //
  // The request queues are here rather than beside the generator states on the
  // heap for the reason the suspensions are here: every promise and value in
  // one is a GC root, and GatherSuspensionRoots is what walks them. A queue is
  // erased when it empties, so nothing accumulates.
  struct Suspensions {
    std::unordered_map<std::uint64_t, Suspension> live;
    std::uint64_t next = 1;
    std::unordered_map<Object*, std::vector<AsyncRequest>> requests;
  };

  Heap heap_;
  VmState vm_;
  Suspensions suspensions_;
  Object* global_ = nullptr;
  Environment* global_scope_ = nullptr;
  WellKnown well_known_;
  // Pending jobs, oldest first. A GC root while queued.
  std::vector<Microtask> microtasks_;
  // Every program this interpreter has run. Kept because a function object
  // points at its own parameters and body in the tree, so a callback that
  // outlives its script would otherwise outlive its code. See Run.
  std::vector<NodePtr> programs_;
  std::vector<std::string> console_;

  // Every scope currently on the C++ call stack, so the collector can find
  // them. A tree-walker cannot scan its own frames; this is the shadow stack
  // that replaces that.
  std::vector<Environment*> active_scopes_;
  // Same, for values held across a call.
  std::vector<Object*> active_objects_;
  // The object each in-flight `new` is building, innermost last.
  //
  // A stack rather than a field because constructors nest, and separate from
  // `active_objects_` because that one is a GC root list shared with iteration
  // cursors -- its back() is not reliably the thing being constructed.
  //
  // It exists because `super()` may *replace* what is being constructed: a
  // base constructor that returns an object makes that object the derived
  // class's `this`, which is the rule custom elements are built on -- an
  // element is upgraded by having HTMLElement hand back the element the
  // document already has. Construct reads this back after the body runs.
  std::vector<Object*> constructing_;
  int call_depth_ = 0;
  // Evaluation depth, which is not the same as call depth and has to be
  // bounded separately: sixty nested unary operators inside a function that
  // recurses two hundred deep is twelve thousand C++ frames, and neither limit
  // alone catches it. Found by the fuzzer as a stack overflow.
  int eval_depth_ = 0;
  static constexpr int kMaxEvalDepth = 512;
  // How deep the *C++* stack may go, which is a different question from how
  // deep JavaScript may recurse.
  //
  // A JS-to-JS call on the machine is a push onto a vector and costs no C++
  // frame at all; what costs one is re-entering the interpreter from a native
  // -- `arr.map(f)`, a promise reaction, the host dispatching an event -- and
  // the tree-walker, whose calls are C++ frames throughout. So this bounds
  // those, and kFrameCapacity bounds the other. Folding the two together is
  // what limited every page to two hundred frames of ordinary recursion.
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

  // --- Modules --------------------------------------------------------------
  //
  // One record per resolved specifier. Keyed by the *resolved* name, so two
  // specifiers naming one file are one module -- which is what makes a shared
  // dependency shared rather than run twice, and is the property a cycle is
  // broken by.
  struct Module {
    std::string specifier;
    const Node* program = nullptr;
    // How many bytes the tree was parsed from. Kept because a module's body is
    // compiled at *evaluation* time and the source is gone by then, and the
    // compiler's instruction bound is a ratio against it -- see
    // kInstructionsPerSourceByte. It is the eighth field, and the seven above
    // ask for an argument: this is not evaluation-order state, it is the one
    // thing about the source that the tree does not carry.
    std::size_t source_length = 0;
    // The module's own top-level scope, whose parent is the global one. A
    // module's declarations are not globals, which is the first thing modules
    // were added to the language for.
    Environment* scope = nullptr;
    // What it publishes, as an object with no prototype: `ns.toString` must be
    // the module's export of that name or undefined, never Object.prototype's.
    Object* exports = nullptr;
    // Each `import`/`from` specifier and what it resolved to, in source order.
    std::vector<std::pair<std::string, Module*>> requests;
    enum class Status : std::uint8_t { New, Evaluating, Evaluated, Failed };
    Status status = Status::New;
    Value error;
  };
  ModuleResolver module_resolver_;
  DynamicImportStarter dynamic_import_starter_;
  std::unordered_map<std::string, std::unique_ptr<Module>> modules_;

  Module* FindModule(const std::string& specifier);
  Result LoadModule(const std::string& specifier, const std::string& referrer, int depth,
                    Module*& out);
  Result EvaluateModule(Module& module);
  // What one `export` declaration publishes, read out of the module's scope
  // after the body has run. Separate from the body because the compiler
  // compiles an export's *declaration* and nothing else -- only this knows
  // what a module is.
  Result PublishExports(const Node& statement, Module& module);
  // `import(spec)`. Answers a promise, which is what the language says even
  // though loading here is synchronous -- a page cannot tell the difference
  // except in ordering, and the ordering is the one every `then` sees.
  Value ImportDynamically(const std::string& specifier, const std::string& referrer);
  // The promises handed out for dynamic imports the host has not answered yet,
  // as a JavaScript array on the global -- which the collector already walks.
  // The host holds a raw `Object*` meanwhile, and a raw pointer is worse than
  // invisible to a collector: it survives the sweep that freed its target.
  Object* PendingImports();
  void DropPendingImport(Object* promise);
  // Every name a declaration binds, so `export const {a, b} = o` publishes
  // both. The same walk a binding pattern does.
  static void CollectDeclaredNames(const Node& node, std::vector<std::string>& out);
  // What `new.target` is for the call about to be made.
  //
  // Set by Construct just before it calls the constructor, and taken by the
  // call that follows -- so it is in effect for exactly one call and cannot
  // leak into the next. A `super()` puts the running frame's back, which is
  // what makes `new B()` give B inside A's constructor as well as inside B's.
  //
  // A member rather than an argument because both engines and four call paths
  // reach the same place, and threading it through all of them would mean four
  // chances to forget.
  Value pending_new_target_;
  std::size_t steps_ = 0;
  // Hang-guard ceiling for the current host turn. Raised by InputTaskBudget
  // and NetworkTaskBudget (TD-0045 / TD-0049).
  std::size_t steps_limit_ = kMaxSteps;
  // After a step-budget RangeError is caught inside RunFrames, further
  // exhaustion in the same turn aborts rather than looping (TD-0018).
  bool step_budget_absorbed_ = false;
  // Depth of NestedHostBudget (media events, CE upgrades) and steps charged
  // across the current sync nesting. See EnterNestedHostBudget.
  std::size_t nested_host_budget_depth_ = 0;
  std::size_t nested_host_chain_steps_ = 0;
  // Re-entrancy depth for `DrainMicrotasks`. Nested drains must not refresh
  // the hang-guard budget (that is the microtask-storm hang TD-0018 forbids);
  // the outermost entry may, when the parent turn already spent most of it.
  int microtask_drain_depth_ = 0;
  struct TrustedScriptHooks {
    void* context = nullptr;
    bool (*active)(void* context) = nullptr;
    void (*apply)(void* context, bool push) = nullptr;
  };
  TrustedScriptHooks trusted_script_hooks_;
  // CSP `'unsafe-eval'` gate for `eval` / `Function` (ADR 0039). Null means
  // allowed — pages with no policy keep the platform default.
  void* eval_forbidden_context_ = nullptr;
  bool (*eval_forbidden_)(void* context) = nullptr;

  // Fresh step budget for a top-level script turn (RunCompiled / RunProgram).
  // Not for CallCompiled — microtasks and nested reactions share the caller.
  void BeginHostTurn();
  void EnterNestedHostBudget(util::PerfCounterId reset_counter);
  void LeaveNestedHostBudget();
  Result ExhaustedSteps();

  // Keeps a scope alive for the collector while it is on the C++ stack.
  class ScopeGuard {
   public:
    ScopeGuard(Interpreter& interpreter, Environment* scope) : interpreter_(interpreter) {
      interpreter_.active_scopes_.push_back(scope);
    }
    ~ScopeGuard() { interpreter_.active_scopes_.pop_back(); }
    // Points the guard at a different scope without giving up the slot.
    //
    // One caller: the per-iteration environment a `for (let i = ...)` makes.
    // The old scope is still reachable -- through whatever closure the body
    // made, which is why it exists at all -- and the new one has to be rooted
    // from the moment it replaces it, which is what this does.
    void Retarget(Environment* scope) { interpreter_.active_scopes_.back() = scope; }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

   private:
    Interpreter& interpreter_;
  };
};

}  // namespace microbrowser::js

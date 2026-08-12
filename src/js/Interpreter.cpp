#include "js/Interpreter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

#include "js/BuiltinSupport.h"
#include "js/StringUnits.h"
#include "util/Env.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::js {

namespace {

// The floor on how many allocations buy a collection. Below this, tracing costs
// more than the memory it would reclaim.
constexpr std::size_t kMinCollectionThreshold = 4096;

// And the whole of the rule: a collection is worth running once the program has
// allocated a *fraction of what is already live*, not once it has allocated a
// fixed number of cells.
//
// This was a flat 4096 and it made the collector quadratic in the size of the
// live set, which is the shape that matters -- a mark-sweep pass costs what is
// live, so a program that grows to N live cells and keeps allocating pays
// O(N) every 4096 allocations, which is O(N²/4096) over the run. It is exactly
// the failure a growing program cannot see coming: nothing is leaking, every
// individual collection is correct, and each one reclaims almost nothing
// because almost everything is still reachable.
//
// Measured on `encoding/legacy-mb-japanese/euc-jp/eucjp-encode-href-errors-han.html`,
// which builds 21,269 testharness subtests and holds every one of them live.
// Batches of 2,000 took 1.8s, 3.2s, 4.1s, 4.6s -- a straight line in the size
// of the heap -- and the page never finished. With the ratio it is flat.
//
// A half is the conventional number: the collector runs when the heap has grown
// by 50%, so total tracing over a run is proportional to total allocation and
// the amortised cost per allocation is constant.
std::size_t CollectionThreshold(std::size_t live_cells) {
  return std::max(kMinCollectionThreshold, live_cells / 2);
}

}  // namespace

void Interpreter::BeginHostTurn() {
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  // Nested RunCompiled — a module body evaluated from dynamic `import()` while
  // another script's frames are still on the machine — must share the outer
  // budget. Resetting here let a stamp storm run forever: each import got a
  // fresh 20M and TD-0018's hang guard never fired.
  if (!vm_.frames.empty()) {
    return;
  }
  steps_ = 0;
  step_budget_absorbed_ = false;
}

std::size_t Interpreter::BeginNetworkTask() {
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  if (!vm_.frames.empty()) {
    util::AddPerformanceCounter(util::PerfCounterId::JsFetchDeliveryBudgetResets);
  }
  steps_ = 0;
  step_budget_absorbed_ = false;
  const std::size_t previous = steps_limit_;
  steps_limit_ = kMaxInputSteps;
  return previous;
}

void Interpreter::EndNetworkTask(std::size_t previous_limit) {
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  // Restoring the ordinary ceiling while leaving steps_ above it makes every
  // later turn instantly exhaust (soft-nav stamp after a long click, TD-0049).
  const bool clamped = steps_ > previous_limit;
  steps_limit_ = previous_limit;
  steps_ = 0;
  step_budget_absorbed_ = false;
  if (clamped) {
    util::AddPerformanceCounter(util::PerfCounterId::JsPostTaskStepClamps);
  }
}

std::size_t Interpreter::BeginInputTask() {
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  if (!vm_.frames.empty()) {
    util::AddPerformanceCounter(util::PerfCounterId::JsInputTaskBudgetResets);
  }
  const std::size_t previous = steps_limit_;
  steps_ = 0;
  step_budget_absorbed_ = false;
  steps_limit_ = kMaxInputSteps;
  return previous;
}

void Interpreter::EndInputTask(std::size_t previous_limit) {
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  // Same contract as EndNetworkTask: the raised ceiling was only for this
  // dispatch. A search-thumb click often burns past kMaxSteps; leaving that
  // count under the restored 20M ceiling aborted the Polymer stamp that runs
  // on the next rAF/timer (TD-0049).
  const bool clamped = steps_ > previous_limit;
  steps_limit_ = previous_limit;
  steps_ = 0;
  step_budget_absorbed_ = false;
  if (clamped) {
    util::AddPerformanceCounter(util::PerfCounterId::JsPostTaskStepClamps);
  }
}

void Interpreter::EnterNestedHostBudget(util::PerfCounterId reset_counter) {
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  if (nested_host_budget_depth_ == 0) {
    nested_host_chain_steps_ = 0;
  } else {
    nested_host_chain_steps_ += steps_;
  }
  ++nested_host_budget_depth_;
  if (nested_host_chain_steps_ >= kMaxNestedHostChainSteps) {
    // Leave steps past the hang guard so the nested CallFunction aborts rather
    // than opening an unbounded pump. Peak already recorded above.
    steps_ = StepsLimit() + 1;
    return;
  }
  // Refresh only when BeginHostTurn would (empty machine), when nesting past
  // the first NestedHostBudget (MSE updateend → append → updateend), or when
  // the shared allotment is already half spent. Unconditional reset on every
  // custom-element upgrade under a live rAF would re-open TD-0018's stamp hang:
  // each cheap upgrade would wipe the outer turn's progress toward kMaxSteps.
  const bool under_live_frames = !vm_.frames.empty();
  const bool refresh = !under_live_frames || nested_host_budget_depth_ > 1 ||
                       steps_ >= StepsLimit() / 2;
  if (!refresh) {
    return;
  }
  steps_ = 0;
  step_budget_absorbed_ = false;
  // Count only the interesting case: a refresh that BeginHostTurn refused
  // (live frames) or a nested NestedHostBudget. Empty-machine zeros are the
  // ordinary BeginTask path and would swamp `js.element_upgrade_budget_resets`.
  if (under_live_frames || nested_host_budget_depth_ > 1) {
    util::AddPerformanceCounter(reset_counter);
  }
}

void Interpreter::LeaveNestedHostBudget() {
  nested_host_chain_steps_ += steps_;
  if (nested_host_budget_depth_ > 0) {
    --nested_host_budget_depth_;
  }
  if (nested_host_budget_depth_ == 0) {
    nested_host_chain_steps_ = 0;
  }
}

void Interpreter::SetTrustedScriptHooks(void* context, bool (*active)(void* context),
                                        void (*apply)(void* context, bool push)) {
  trusted_script_hooks_.context = context;
  trusted_script_hooks_.active = active;
  trusted_script_hooks_.apply = apply;
}

Result Interpreter::ExhaustedSteps() {
  util::AddPerformanceCounter(util::PerfCounterId::JsStepsExhausted);
  util::MaxPerformanceCounter(util::PerfCounterId::JsStepsPeak, steps_);
  return Throw("RangeError", "script ran too long");
}

Interpreter::Interpreter() {
  // Reserved once, and never allowed to grow past it. The machine takes
  // references into the value stack and a reallocation would invalidate every
  // one of them, so a fixed capacity makes that safe by construction; see the
  // note at the top of Vm.cpp.
  vm_.stack.reserve(kValueStackCapacity);
  global_ = heap_.AllocateObject(Object::Kind::Plain);
  global_scope_ = heap_.AllocateEnvironment(nullptr);
  well_known_.object_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.array_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.function_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.string_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.regexp_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.promise_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.generator_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.async_generator_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.return_signal = heap_.AllocateObject(Object::Kind::Plain);
  // One object, compared by identity. What a short-circuited optional chain
  // travels as in the tree-walker, where a link is a C++ frame and can only
  // tell the ones outside it to give up with a value.
  well_known_.chain_signal = heap_.AllocateObject(Object::Kind::Plain);
  InstallGlobals();
}

Interpreter::~Interpreter() = default;

double Interpreter::NowMilliseconds() const {
  // Millisecond resolution, deliberately. A finer clock is what every timing
  // side channel is built on, and the spec asks for no more than this.
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

Object* Interpreter::NewObject() {
  Object* object = heap_.AllocateObject(Object::Kind::Plain);
  if (object == nullptr) {
    return nullptr;  // the heap is full; the caller turns this into a RangeError
  }
  object->SetPrototype(well_known_.object_prototype);
  return object;
}

Value Interpreter::NewArrayValue(std::vector<Value> elements) {
  Object* array = NewArray(std::move(elements));
  return array == nullptr ? Value::Undefined() : Value::Obj(array);
}

Value Interpreter::NewArrayValue(std::vector<Value> elements, std::vector<bool> present) {
  Object* array = NewArray(std::move(elements), std::move(present));
  return array == nullptr ? Value::Undefined() : Value::Obj(array);
}

Object* Interpreter::NewArray(std::vector<Value> elements) {
  return NewArray(std::move(elements), {});
}

Object* Interpreter::NewArray(std::vector<Value> elements, std::vector<bool> present) {
  Object* array = heap_.AllocateObject(Object::Kind::Array);
  if (array == nullptr) {
    return nullptr;
  }
  array->SetPrototype(well_known_.array_prototype);
  array->SetElements(std::move(elements), std::move(present));
  return array;
}

Value Interpreter::NewObjectValue() {
  Object* object = NewObject();
  return object == nullptr ? Value::Undefined() : Value::Obj(object);
}

Value Interpreter::NewNativeValue(const char* name, NativeFunction function) {
  Object* native = NewNative(name, std::move(function));
  return native == nullptr ? Value::Undefined() : Value::Obj(native);
}

Value Interpreter::NewBigIntValue(BigInt digits) {
  // A Symbol-kind cell, because that is what a cell whose identity is not the
  // point looks like here: nothing reads properties off it, and the sweep that
  // frees it drops the digits.
  Object* cell = heap_.AllocateObject(Object::Kind::Symbol);
  if (cell == nullptr) {
    return Value::Undefined();
  }
  heap_.AttachBigInt(cell, std::make_shared<const BigInt>(std::move(digits)));
  return Value::Big(cell);
}

Value Interpreter::NewRegExpValue(RegExp pattern) {
  Object* object = heap_.AllocateObject(Object::Kind::RegExp);
  if (object == nullptr) {
    return Value::Undefined();
  }
  object->SetPrototype(well_known_.regexp_prototype);
  // Writable, and read back before every global match: a page advances a
  // stateful regex by assigning to it.
  object->Set("lastIndex", Value::Number(0.0));
  // Own data properties rather than accessors, for the same reason an Error
  // carries `name` and `message`: ToString has no interpreter to call a method
  // with, and printing a pattern as "[object Object]" tells nobody anything. A
  // page that assigns to these changes what printing says and not what
  // matches, since the compiled pattern beside the object is the authority.
  object->Set("source", Value::String(pattern.Source().empty() ? std::string("(?:)")
                                                               : pattern.Source()));
  object->Set("flags", Value::String(pattern.Flags().Text()));
  heap_.AttachRegExp(object, std::make_shared<const RegExp>(std::move(pattern)));
  return Value::Obj(object);
}

const RegExp* Interpreter::RegExpOf(const Value& value) const {
  return value.IsObject() ? heap_.FindRegExp(value.object) : nullptr;
}

Value Interpreter::MakeError(std::string_view kind, std::string message) {
  util::AddPerformanceCounter(util::PerfCounterId::JsThrows);
  // Caught throws never reach ReportUncaught. youtube's Gal/setmediasrc path is
  // the motivating case: it catches, maps to fmt.unplayable, and leaves the
  // original TypeError/DOMException name only in the UV details object. Opt in
  // with MICROBROWSER_JS_THROWS=1; off by default so a page that throws in a
  // hot loop does not own stderr.
  static const bool kLogThrows = util::EnvFlagEnabled("MICROBROWSER_JS_THROWS");
  if (kLogThrows) {
    const std::string stack = CaptureStack(kind, message);
    // One line for easy grepping; stack frames stay on following lines so a
    // caught throw still names a place the way ReportUncaught would.
    std::fprintf(stderr, "[js.throw] %s\n", stack.c_str());
    std::fflush(stderr);
  }
  Object* error = heap_.AllocateObject(Object::Kind::Error);
  if (error == nullptr) {
    // Out of memory while reporting being out of memory. A string is the one
    // thing that can still be made, and losing the error type is better than
    // recursing here.
    return Value::String(std::string(kind) + ": " + message);
  }
  // The prototype of the matching constructor, when there is one, so that a
  // page's `catch (e) { if (e instanceof TypeError) }` is true of an error the
  // *engine* threw and not only of one the page made. Falls back to the plain
  // object prototype during startup, before the constructors exist.
  Object* prototype = well_known_.object_prototype;
  if (Value* constructor = global_scope_->Lookup(std::string(kind))) {
    if (constructor->IsObject()) {
      if (const Value* declared = constructor->object->GetOwn("prototype")) {
        if (declared->IsObject()) {
          prototype = declared->object;
        }
      }
    }
  }
  error->SetPrototype(prototype);
  // Non-enumerable, like every own property of an error the language makes:
  // `Object.keys(e)` on a caught engine throw must be empty, and a page that
  // spreads or JSON-stringifies one must not find these in it.
  //
  // `name` is own rather than inherited because the prototype above is a
  // *fallback* -- during startup, and for a kind with no constructor, there is
  // no prototype carrying the right name and `e.name` would answer "Error".
  error->SetHidden("name", Value::String(std::string(kind)));
  const std::string text(message);
  error->SetHidden("message", Value::String(std::move(message)));
  // An error the *engine* threw gets the same stack an error a page
  // constructed does. Without this the two kinds are told apart by whether
  // `e.stack` is undefined, and the engine's -- the TypeError from calling
  // something that is not a function -- is exactly the one worth locating: it
  // says what went wrong and, until now, nothing at all about where.
  error->SetHidden("stack", Value::String(CaptureStack(kind, text)));
  return Value::Obj(error);
}

std::string Interpreter::CaptureStack(std::string_view kind,
                                      std::string_view message) const {
  std::string out(kind);
  if (!message.empty()) {
    out += ": ";
    out += message;
  }
  // Innermost first, which is the order every engine prints and every stack
  // parser expects. Bounded, because a page can recurse as deep as the frame
  // limit allows and a megabyte of stack text helps nobody.
  constexpr std::size_t kMaxFrames = 32;
  std::size_t written = 0;
  for (std::size_t i = vm_.frames.size(); i > 0 && written < kMaxFrames; --i) {
    const Frame& frame = vm_.frames[i - 1];
    const std::string& name = frame.code == nullptr ? std::string() : frame.code->name;
    out += "\n    at ";
    out += name.empty() ? "<anonymous>" : name;
    if (frame.code != nullptr && !frame.code->positions.empty()) {
      // `ip` has already been advanced past the instruction being executed, so
      // the one that threw -- or, in an outer frame, the call that led here --
      // is the one before it.
      const std::uint32_t at = frame.ip == 0 ? 0 : frame.ip - 1;
      out += " (@";
      out += std::to_string(frame.code->PositionOf(at));
      out += ")";
    }
    ++written;
  }
  return out;
}

Result Interpreter::Throw(std::string_view kind, std::string message) {
  return Result{Completion::Throw, MakeError(kind, std::move(message)), {}};
}

Value NativeCall::Throw(std::string_view kind, std::string message) {
  return ThrowValue(interpreter.MakeError(kind, std::move(message)));
}

Value NativeCall::ThrowValue(Value value) {
  thrown = std::move(value);
  threw = true;
  return Value::Undefined();
}

void Interpreter::MaybeCollect() {
  // The live count is read *before* the collection rather than remembered from
  // after the last one, which overstates it by whatever garbage has piled up
  // since -- and that is the safe direction: it can only make the next
  // collection later, never sooner, so it cannot reintroduce the quadratic.
  const std::size_t cells = heap_.ObjectCount() + heap_.EnvironmentCount();
  if (heap_.AllocationsSinceCollection() < CollectionThreshold(cells) || call_depth_ != 0) {
    return;
  }
  std::vector<Object*> object_roots = well_known_.Roots();
  object_roots.push_back(global_);
  object_roots.insert(object_roots.end(), active_objects_.begin(), active_objects_.end());
  // A queued job is the only thing keeping its handler and its result alive.
  for (const Microtask& task : microtasks_) {
    for (const Value* held : {&task.callee, &task.argument, &task.derived}) {
      if (held->IsObject() || held->IsSymbol()) {
        object_roots.push_back(held->object);
      }
    }
  }
  // Every module that has been loaded. Its namespace object and its own scope
  // are reachable from the table and from nowhere else the collector walks --
  // a module whose importer has finished is still live, because `import` of
  // the same specifier again must answer with the same namespace rather than
  // re-evaluate it. Without this the second import reads freed memory.
  for (const auto& [specifier, module] : modules_) {
    if (module == nullptr) {
      continue;
    }
    if (module->exports != nullptr) {
      object_roots.push_back(module->exports);
    }
    if (module->error.IsObject() || module->error.IsSymbol()) {
      object_roots.push_back(module->error.object);
    }
  }
  std::vector<Environment*> environment_roots{global_scope_};
  environment_roots.insert(environment_roots.end(), active_scopes_.begin(), active_scopes_.end());
  for (const auto& [specifier, module] : modules_) {
    if (module != nullptr && module->scope != nullptr) {
      environment_roots.push_back(module->scope);
    }
  }
  // The machine's stacks. This is the addition that makes collection possible
  // while script is running rather than only between top-level statements: a
  // frame's scope and every value in flight are here, where a tree-walker's
  // were in C++ locals nothing could scan.
  GatherVmRoots(object_roots, environment_roots);
  // And the calls that are waiting. A suspended frame's values came off those
  // stacks, so nothing else can see them.
  GatherSuspensionRoots(object_roots, environment_roots);
  heap_.Collect(object_roots, environment_roots);
}

Value Interpreter::NewFunction(const Node& node, Environment& scope, bool arrow) {
  // The two flags that need somewhere to put a frame down, by name rather than
  // by `number != 0`. It was the latter, which read every *future* flag as
  // "async or generator" -- and kFunctionNamedExpression, which is neither,
  // made the tree-walker refuse `var f = function me(){}` the day it was added.
  if ((static_cast<std::uint8_t>(node.number) & (kFunctionAsync | kFunctionGenerator)) != 0) {
    // An async function or a generator, and this is the tree-walker's function
    // -- the machine makes its own in the Closure opcode. A tree-walker can run
    // neither: its state is C++ stack frames, and both `await` and `yield` need
    // somewhere to put one down. So calling it says so, rather than returning
    // something that is not a promise or not an iterator and letting the
    // difference surface three lines later.
    Object* refuser = NewNative(node.string.c_str(), [](NativeCall& call) {
      return call.Throw("TypeError",
                        "an async function or generator needs the bytecode machine; this "
                        "program fell back to the tree-walking interpreter");
    });
    return refuser == nullptr ? Value::Undefined() : Value::Obj(refuser);
  }
  Object* function = heap_.AllocateObject(Object::Kind::Function);
  if (function == nullptr) {
    return Value::Undefined();
  }
  function->SetPrototype(well_known_.function_prototype);
  function->MakeFunction(node.Child(0), node.Child(1), &scope, arrow);
  function->SetHidden("name", Value::String(node.string));
  function->Set("length", Value::Number(DeclaredArity(node.Child(0))));
  if (!arrow) {
    // Every ordinary function gets a fresh `prototype` object, because any of
    // them can be called with `new`. An arrow function cannot, which is why it
    // does not get one.
    if (Object* prototype = NewObject()) {
      prototype->SetHidden("constructor", Value::Obj(function));
      function->Set("prototype", Value::Obj(prototype));
    }
  }
  return Value::Obj(function);
}

Value Interpreter::NewCompiledFunction(const CompiledFunction& code, Environment& scope,
                                       bool arrow) {
  Object* function = heap_.AllocateObject(Object::Kind::Function);
  if (function == nullptr) {
    return Value::Undefined();
  }
  function->SetPrototype(well_known_.function_prototype);
  function->MakeCompiled(&code, &scope, arrow);
  function->SetHidden("name", Value::String(code.name));
  function->Set("length", Value::Number(static_cast<double>(code.parameter_count)));
  if (!arrow) {
    if (Object* prototype = NewObject()) {
      prototype->SetHidden("constructor", Value::Obj(function));
      function->Set("prototype", Value::Obj(prototype));
    }
  }
  return Value::Obj(function);
}

// --- Property access -------------------------------------------------------

Object* Interpreter::PatternProtocol(const Value& value, const char* which) {
  if (!value.IsObject() && !value.IsSymbol()) {
    return nullptr;  // a string is the common case and has no methods to ask
  }
  // A RegExp answers to these too in the spec, and here the RegExp path is
  // taken directly by the caller -- so an actual RegExp is excluded, and only
  // an object standing in for one reaches the protocol.
  if (value.IsObject() && value.object->GetKind() == Object::Kind::RegExp) {
    return nullptr;
  }
  Value* symbol_object = global_scope_->Lookup("Symbol");
  if (symbol_object == nullptr || !symbol_object->IsObject()) {
    return nullptr;
  }
  const Value* cell = symbol_object->object->GetOwn(which);
  if (cell == nullptr || !cell->IsSymbol()) {
    return nullptr;
  }
  const Value method = GetProperty(value, PropertyKey::Symbol(cell->object));
  return method.IsObject() && method.object->IsCallable() ? method.object : nullptr;
}

Object* Interpreter::ProxyTrap(const Value& base, const char* trap, Value& target) const {
  if (!base.IsObject() || base.object->GetKind() != Object::Kind::Proxy) {
    return nullptr;
  }
  const Value* behind = base.object->GetOwn("#target");
  target = behind == nullptr ? Value::Undefined() : *behind;
  const Value* handler = base.object->GetOwn("#handler");
  if (handler == nullptr || !handler->IsObject()) {
    return nullptr;
  }
  // A handler without the trap is not an error: the operation falls through to
  // the target, which is what makes `new Proxy(o, {})` behave exactly like `o`.
  const Value* hook = handler->object->Get(trap);
  return hook != nullptr && hook->IsObject() && hook->object->IsCallable() ? hook->object
                                                                          : nullptr;
}



bool Interpreter::DeleteProperty(const Value& base, const PropertyKey& key) {
  if (base.IsObject() && base.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(base, "deleteProperty", target)) {
      const Result asked =
          CallFunction(Value::Obj(trap), Value::Undefined(), {target, KeyValue(key)});
      return !asked.IsAbrupt() && ToBoolean(asked.value);
    }
    return !target.IsObject() || DeleteProperty(target, key);
  }
  return !base.IsObject() || base.object->Delete(key);
}

std::vector<std::string> Interpreter::OwnKeys(const Value& base, bool enumerable_only) {
  if (base.IsObject() && base.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(base, "ownKeys", target)) {
      std::vector<Value> listed;
      const Result asked = CallFunction(Value::Obj(trap), Value::Undefined(), {target});
      if (asked.IsAbrupt() || CollectIterable(asked.value, listed).IsAbrupt()) {
        return {};
      }
      std::vector<std::string> out;
      for (const Value& entry : listed) {
        if (!entry.IsString()) {
          continue;  // a symbol key is not something an enumeration reports
        }
        if (enumerable_only) {
          // The spec asks the descriptor trap whether each key is enumerable,
          // which is what makes a proxy able to hide one from `Object.keys`
          // while still listing it from `getOwnPropertyNames`.
          Value ignored;
          if (Object* describe = ProxyTrap(base, "getOwnPropertyDescriptor", ignored)) {
            const Result descriptor = CallFunction(Value::Obj(describe), Value::Undefined(),
                                                   {target, entry});
            if (descriptor.IsAbrupt() || !descriptor.value.IsObject() ||
                !ToBoolean(GetProperty(descriptor.value, "enumerable"))) {
              continue;
            }
          }
        }
        out.push_back(entry.AsString());
      }
      return out;
    }
    return target.IsObject() ? OwnKeys(target, enumerable_only) : std::vector<std::string>{};
  }
  if (!base.IsObject()) {
    return {};
  }
  return enumerable_only ? base.object->EnumerableKeys() : base.object->Keys();
}

Value Interpreter::GetProperty(const Value& base, const PropertyKey& key, Result* abrupt) {
  // One place to record a throw from a getter or a trap, so the three call sites below
  // cannot disagree about whether they reported it.
  const auto answered = [abrupt](const Result& got) {
    if (got.IsAbrupt() && abrupt != nullptr) {
      *abrupt = got;
    }
    return got.IsAbrupt() ? Value::Undefined() : got.value;
  };
  if (base.IsObject() && base.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(base, "get", target)) {
      // (target, key, receiver), and the receiver is the proxy -- so a getter
      // reached through it sees the proxy as `this`, which is what makes a
      // reactive object notice a read of a computed property.
      const Result got = CallFunction(Value::Obj(trap), Value::Undefined(),
                                      {target, KeyValue(key), base});
      return answered(got);
    }
    return target.IsUndefined() ? Value::Undefined() : GetProperty(target, key, abrupt);
  }
  // A symbol key names none of the built-in structure below -- there is no
  // symbol spelled "length" and no symbol that is an array index -- so those
  // tests are guarded rather than repeated inside each one.
  const bool named = !key.IsSymbol();
  if (base.IsString()) {
    const std::string& text = base.AsString();
    if (named && key.Text() == "length") {
      // In UTF-16 code units, which is what the language counts. For an ASCII
      // string that is the byte count and the conversion is a scan that stops
      // at the first high bit; see StringUnits.h.
      return Value::Number(static_cast<double>(Utf16Length(text)));
    }
    if (const std::optional<std::size_t> index =
            named ? ParseArrayIndex(key.Text()) : std::nullopt) {
      return *index < Utf16Length(text)
                 ? Value::String(SubstringUnits(text, *index, *index + 1))
                 : Value::Undefined();
    }
    // Anything else is a method, read from the shared prototype rather than
    // from a wrapper object: a string is a primitive here, so there is nothing
    // to box. Reading a plain value is the whole lookup. An accessor a page
    // added to String.prototype reads as undefined rather than running with
    // some invented receiver -- boxing is what would make it callable, and it
    // is not worth building for a case no real page has.
    const Object::Property* method =
        well_known_.string_prototype == nullptr ? nullptr : well_known_.string_prototype->GetProperty(key);
    return method == nullptr || method->IsAccessor() ? Value::Undefined() : method->value;
  }
  if (base.IsNumber()) {
    // Read straight off the shared prototype, the same way a string's methods
    // are: a number is a primitive here, so there is nothing to box.
    const Object::Property* method =
        well_known_.number_prototype == nullptr
            ? nullptr
            : well_known_.number_prototype->GetProperty(key);
    return method == nullptr || method->IsAccessor() ? Value::Undefined() : method->value;
  }
  if (base.IsBigInt()) {
    // A primitive, like a number: its two methods come off the shared
    // prototype rather than off a wrapper.
    const Object::Property* method =
        well_known_.bigint_prototype == nullptr
            ? nullptr
            : well_known_.bigint_prototype->GetProperty(key);
    return method == nullptr || method->IsAccessor() ? Value::Undefined() : method->value;
  }
  if (base.type == ValueType::Boolean) {
    // A boolean is a primitive here too, so its two methods come off the
    // shared prototype rather than off a wrapper. Small, and load-bearing:
    // ToPrimitive calls `toString` on whatever it is handed.
    const Object::Property* method =
        well_known_.boolean_prototype == nullptr
            ? nullptr
            : well_known_.boolean_prototype->GetProperty(key);
    return method == nullptr || method->IsAccessor() ? Value::Undefined() : method->value;
  }
  if (base.IsSymbol()) {
    // A symbol is a primitive, but its cell is an object, so `sym.description`
    // and `sym.toString` are an ordinary prototype walk from the cell. No
    // wrapper is needed and none is made -- the same reasoning as a string,
    // which reads its methods straight off String.prototype.
    const Object::Property* property = base.object->GetProperty(key);
    if (property == nullptr) {
      return Value::Undefined();
    }
    if (property->getter != nullptr) {
      return answered(CallFunction(Value::Obj(property->getter), base, {}));
    }
    return property->IsAccessor() ? Value::Undefined() : property->value;
  }
  if (!base.IsObject()) {
    return Value::Undefined();
  }

  Object* object = base.object;
  if (object->View() != nullptr && named &&
      object->GetKind() == Object::Kind::TypedArray) {
    // A typed array's indices are bytes in a buffer rather than properties, so
    // they are answered here and never reach the property map -- which is also
    // what makes `ta[99] = 1` on a four-element view a no-op rather than a new
    // property nobody can see through `length`.
    if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
      return object->GetElement(*index);
    }
  }
  if (object == global_ && named && object->GetOwnProperty(key) == nullptr) {
    // `globalThis.Math`. The builtins are bindings in the global *scope*
    // rather than properties of the global *object*, because that is where a
    // name lookup finds them -- so reading one off `globalThis` has to reach
    // the same place. One namespace with two spellings, not two that overlap.
    if (Value* binding = global_scope_->Lookup(key.Text())) {
      return *binding;
    }
  }
  if (object->GetKind() == Object::Kind::Array && named) {
    if (key.Text() == "length") {
      return Value::Number(static_cast<double>(object->ElementCount()));
    }
    if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
      return object->GetElement(*index);
    }
  }
  const Object::Property* property = object->GetProperty(key);
  if (property == nullptr) {
    return Value::Undefined();
  }
  if (property->getter != nullptr) {
    // The getter runs with `this` bound to the object it was read *from*, not
    // the one that owns it -- which is what makes an accessor inherited from a
    // class body see the instance.
    return answered(CallFunction(Value::Obj(property->getter), base, {}));
  }
  if (property->setter != nullptr) {
    return Value::Undefined();  // set-only: reading gives undefined
  }
  return property->value;
}

Result Interpreter::SetProperty(const Value& base, const PropertyKey& key, const Value& value) {
  if (base.IsObject() && base.object == global_ && !key.IsSymbol() &&
      base.object->GetOwnProperty(key) == nullptr) {
    // The other direction of the same rule: `globalThis.Math = x` has to be
    // the assignment `Math = x`, or the two spellings would disagree from the
    // next read on.
    if (global_scope_->HasOwn(key.Text())) {
      if (global_scope_->Assign(key.Text(), value) == Environment::AssignResult::Constant) {
        return Throw("TypeError", "assignment to constant variable '" + key.Text() + "'");
      }
      return Result::Normal(value);
    }
  }
  if (base.IsObject() && base.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(base, "set", target)) {
      const Result set = CallFunction(Value::Obj(trap), Value::Undefined(),
                                      {target, KeyValue(key), value, base});
      return set.IsAbrupt() ? set : Result::Normal(value);
    }
    return target.IsUndefined() ? Result::Normal(value) : SetProperty(target, key, value);
  }
  const bool named = !key.IsSymbol();
  if (base.IsObject() && named && base.object->GetKind() == Object::Kind::TypedArray &&
      base.object->View() != nullptr) {
    if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
      base.object->SetElement(*index, value);
      return Result::Normal(value);
    }
  }
  if (!base.IsObject()) {
    // Assigning to a property of a primitive is a silent no-op outside strict
    // mode and a TypeError inside it. Null and undefined are always an error,
    // which is the one people actually hit.
    if (base.IsNullish()) {
      return Throw("TypeError", "cannot set property '" +
                                    (named ? key.Text() : std::string("[symbol]")) + "' of " +
                                    ToString(base));
    }
    return Result::Normal(value);
  }

  Object* object = base.object;
  if (object->GetKind() == Object::Kind::Array && named) {
    if (key.Text() == "length") {
      const double numeric_length = ToNumber(value);
      const std::uint32_t length = ToUint32(numeric_length);
      if (numeric_length != static_cast<double>(length) || length > kMaxAllocationLength) {
        // A page can write `a.length = 4294967295`, and honouring it would be a
        // 34-gigabyte allocation. The bound is far past anything real. Fractional,
        // negative, and NaN lengths are invalid rather than truncated.
        return Throw("RangeError", "array length is too large");
      }
      object->ResizeElements(length);
      return Result::Normal(value);
    }
    if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
      if (*index >= kMaxAllocationLength) {
        return Throw("RangeError", "array index is too large");
      }
      object->SetElement(*index, value);
      return Result::Normal(value);
    }
  }
  if (const Object::Property* property = object->GetProperty(key)) {
    if (property->setter != nullptr) {
      return CallFunction(Value::Obj(property->setter), base, {value});
    }
    if (property->getter != nullptr) {
      // Getter-only. Assigning is a silent no-op outside strict mode, which is
      // the mode this engine is in.
      return Result::Normal(value);
    }
  }
  object->Set(key, value);
  return Result::Normal(value);
}

// --- Calling ---------------------------------------------------------------

Result Interpreter::BindParameters(const Node& parameters, const std::vector<Value>& arguments,
                                   Environment& scope) {
  for (std::size_t i = 0; i < parameters.children.size(); ++i) {
    const Node* parameter = parameters.Child(i);
    if (parameter == nullptr) {
      continue;
    }
    if (parameter->kind == NodeKind::RestElement) {
      std::vector<Value> rest;
      for (std::size_t j = i; j < arguments.size(); ++j) {
        rest.push_back(arguments[j]);
      }
      const Node* target = parameter->Child(0);
      if (target != nullptr) {
        Object* rest_array = NewArray(std::move(rest));
        if (rest_array == nullptr) {
          return Throw("RangeError", "out of memory");
        }
        const Result bound = BindPattern(*target, Value::Obj(rest_array), scope, true, false);
        if (bound.IsAbrupt()) {
          return bound;
        }
      }
      break;
    }
    Value argument = i < arguments.size() ? arguments[i] : Value::Undefined();
    const Node* target = parameter;
    if (parameter->kind == NodeKind::AssignmentPattern) {
      target = parameter->Child(0);
      if (argument.IsUndefined() && parameter->Child(1) != nullptr) {
        // A default applies for a missing argument *and* for an explicit
        // undefined, which is the rule and is not the same as "no argument".
        const Result value = Evaluate(*parameter->Child(1), scope);
        if (value.IsAbrupt()) {
          return value;
        }
        argument = value.value;
      }
    }
    if (target != nullptr) {
      const Result bound = BindPattern(*target, argument, scope, true, false);
      if (bound.IsAbrupt()) {
        return bound;
      }
    }
  }
  return Result::Normal();
}

Result Interpreter::Construct(const Value& callee, const std::vector<Value>& arguments) {
  if (!callee.IsObject() || !callee.object->IsCallable()) {
    return Throw("TypeError", ToString(callee) + " is not a constructor");
  }
  if (callee.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(callee, "construct", target)) {
      // (target, argumentsList, newTarget). The trap has to hand back an
      // object -- a construction that produced a primitive would leave `new`
      // with nothing to be.
      const Value list = NewArrayValue(arguments);
      const Result made =
          CallFunction(Value::Obj(trap), Value::Undefined(), {target, list, callee});
      if (made.IsAbrupt()) {
        return made;
      }
      if (!made.value.IsObject()) {
        return Throw("TypeError", "a construct trap must return an object");
      }
      return made;
    }
    return Construct(target, arguments);
  }
  const CompiledFunction* code = callee.object->Code();
  if (code != nullptr && code->is_async) {
    // An async function returns a promise, so `new` on one has nothing to hand
    // back that is an instance. The spec says TypeError and this is the only
    // place that can tell.
    return Throw("TypeError", "an async function is not a constructor");
  }
  // What kind of object to allocate, which is the root constructor's business
  // rather than this one's: `class HttpError extends Error` has to allocate an
  // Error, because the kind is fixed at allocation and is what makes
  // `String(e)` say "HttpError: not found" instead of "[object Object]".
  Object::Kind kind = Object::Kind::Plain;
  const Object* root = callee.object;
  for (int depth = 0; root != nullptr && depth < 1000; ++depth) {
    if (const Value* marked = root->GetOwn(kConstructsKey)) {
      kind = static_cast<Object::Kind>(static_cast<int>(ToNumber(*marked)));
      break;
    }
    root = root->SuperConstructor();
  }
  Object* instance =
      kind == Object::Kind::Array ? NewArray({}) : heap_.AllocateObject(kind);
  if (instance == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  instance->SetPrototype(well_known_.object_prototype);
  const Value* prototype = callee.object->Get("prototype");
  if (prototype != nullptr && prototype->IsObject()) {
    instance->SetPrototype(prototype->object);
  }
  const Value self = Value::Obj(instance);
  active_objects_.push_back(instance);
  constructing_.push_back(instance);
  // A base class initializes its fields before the constructor body runs. A
  // derived one does it after its super() call instead, which is the ordering
  // that lets a derived field read a base one.
  Object* parent = callee.object->SuperConstructor();
  if (parent == nullptr) {
    const Result fields = InitializeFields(instance, callee.object);
    if (fields.IsAbrupt()) {
      active_objects_.pop_back();
      constructing_.pop_back();
      return fields;
    }
  } else if (callee.object->Body() == nullptr && callee.object->Code() == nullptr) {
    // A derived class with no explicit constructor gets an implicit
    // `constructor(...args){ super(...args) }`. Without it, `class B extends A
    // { n = 5 }` runs no constructor at all and leaves both the base's state
    // and its own fields unset.
    pending_new_target_ = callee;
    const Result base = CallFunction(Value::Obj(parent), self, arguments);
    pending_new_target_ = Value::Undefined();
    if (base.IsAbrupt()) {
      active_objects_.pop_back();
      constructing_.pop_back();
      return base;
    }
    // An implicit `constructor(...args){ super(...args) }` is still a super
    // call, so it too may have been handed a different object to be.
    const Result fields = InitializeFields(constructing_.back(), callee.object);
    if (fields.IsAbrupt()) {
      active_objects_.pop_back();
      constructing_.pop_back();
      return fields;
    }
  }
  // `new.target` is in effect for exactly this call. Taken by whichever call
  // path binds the frame, and cleared there, so an ordinary call made later
  // does not inherit it.
  pending_new_target_ = callee;
  const Result constructed = CallFunction(callee, self, arguments);
  pending_new_target_ = Value::Undefined();
  // What `super()` left, which is `instance` unless a base constructor
  // returned an object of its own and became what this class is.
  Object* built = constructing_.empty() ? instance : constructing_.back();
  active_objects_.pop_back();
  if (!constructing_.empty()) {
    constructing_.pop_back();
  }
  if (constructed.IsAbrupt()) {
    return constructed;
  }
  // A constructor returning an object replaces the instance; returning a
  // primitive does not. The rule exists so a factory can be a constructor.
  return Result::Normal(constructed.value.IsObject() ? constructed.value
                                                     : Value::Obj(built));
}

bool Interpreter::IsConstructCall(const Value& self) const {
  return !constructing_.empty() && self.IsObject() && constructing_.back() == self.object;
}

Result Interpreter::CallFunction(const Value& callee, const Value& self,
                                 const std::vector<Value>& arguments) {
  if (!callee.IsObject() || !callee.object->IsCallable()) {
    return Throw("TypeError", ToString(callee) + " is not a function");
  }
  if (callee.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(callee, "apply", target)) {
      // (target, thisArg, argumentsList). The arguments arrive as an array
      // rather than spread, which is what lets a wrapper read them without
      // knowing the arity.
      const Value list = NewArrayValue(arguments);
      return CallFunction(Value::Obj(trap), Value::Undefined(), {target, self, list});
    }
    // No trap: the call goes straight through, which is what makes
    // `new Proxy(fn, {})` behave exactly like `fn`.
    return CallFunction(target, self, arguments);
  }
  if (call_depth_ >= kMaxCallDepth) {
    // Re-entering the interpreter from C++ -- a native calling back into
    // script -- is what runs the C++ stack out. Ordinary JS recursion is
    // bounded by kFrameCapacity instead, which is a much larger number
    // because a frame there costs a vector slot rather than a C++ frame.
    return Throw("RangeError", "maximum call stack size exceeded");
  }

  Object* function = callee.object;
  if (function->Code() != nullptr) {
    // A compiled body, entered from C++ -- a native calling a callback, the
    // host dispatching an event, or a custom-element reaction. Do *not* raise
    // call_depth_: that flag exists to block safepoints under C++ locals the
    // collector cannot see (natives, the tree-walker, ClassLiteral). The
    // machine's stacks are data and GatherVmRoots sees them, which is the
    // whole reason the bytecode engine exists. Raising the depth here made
    // every VM safepoint a no-op for the duration of a reaction, so youtube's
    // Polymer upgrades allocated straight into the 500k heap limit and threw
    // `RangeError: out of memory` with no stack (MakeError itself was OOM).
    return CallCompiled(function, self, arguments);
  }
  if (function->GetKind() == Object::Kind::Native) {
    ++call_depth_;
    NativeCall call{*this, function, self, arguments};
    Value value = function->Native()(call);
    --call_depth_;
    if (call.HasThrown()) {
      return Result{Completion::Throw, call.ThrownValue(), {}};
    }
    return Result::Normal(std::move(value));
  }

  Environment* scope = heap_.AllocateEnvironment(function->Closure());
  if (scope == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  const ScopeGuard guard(*this, scope);

  // An arrow function has no `this` of its own: it uses the one captured where
  // it was written. That is the whole difference between the two forms, and it
  // is why the binding is decided here rather than by the caller.
  Value this_value = self;
  if (!function->IsArrow()) {
    // Compiled functions carry `is_strict`. Tree-walker bodies are treated as
    // non-strict here: the differential path is not where youtube's player
    // runs, and OrdinaryCallBindThis for a strict AST function under
    // MICROBROWSER_JS_TREEWALK is a known approximation.
    const bool strict = function->Code() != nullptr && function->Code()->is_strict;
    if (!strict && (this_value.IsUndefined() || this_value.IsNull())) {
      this_value = Value::Obj(global_);
    }
  }
  scope->Declare("this", function->IsArrow() ? function->BoundThis() : this_value, true);
  // What `super` needs: the object the method was defined on, and the function
  // itself (for its superclass). Bound here rather than looked up later,
  // because by the time super runs the C++ frame that knew them is gone.
  if (function->HomeObject() != nullptr) {
    scope->Declare("__home__", Value::Obj(function->HomeObject()), true);
  }
  scope->Declare("__function__", callee, true);
  // Taken rather than read: it belongs to this call and to no other.
  if (!function->IsArrow()) {
    // An arrow has no `new.target` of its own, the way it has no `this` --
    // it reads the one from where it was written, which the walk out finds.
    scope->Declare("__newtarget__", pending_new_target_, true);
    pending_new_target_ = Value::Undefined();
  }
  if (Object* argument_list = NewArray(arguments)) {
    scope->Declare("arguments", Value::Obj(argument_list), false);
  }

  if (function->Parameters() != nullptr) {
    const Result bound = BindParameters(*function->Parameters(), arguments, *scope);
    if (bound.IsAbrupt()) {
      return bound;
    }
  }

  const Node* body = function->Body();
  if (body == nullptr) {
    return Result::Normal();
  }
  // Every `var` in the body exists from here, holding undefined, wherever in
  // the body it is written. `for (var i = 0; ...) {} return i` depends on it,
  // and so does the closure that reads a `var` declared further down -- which
  // is how a bundle's global-object probe is written.
  HoistVars(*body, *scope);

  ++call_depth_;
  Result result = body->kind == NodeKind::Block ? EvaluateBlock(*body, *scope)
                                                : Evaluate(*body, *scope);
  --call_depth_;

  if (result.completion == Completion::Return) {
    return Result::Normal(std::move(result.value));
  }
  if (result.completion == Completion::Throw) {
    return result;
  }
  // An expression-bodied arrow returns its expression; a block body that falls
  // off the end returns undefined.
  if (body->kind != NodeKind::Block) {
    return Result::Normal(std::move(result.value));
  }
  return Result::Normal();
}

Result Interpreter::Run(std::string_view source) {
  // Three jobs with nothing in common but their order, and a page whose bundle
  // takes eleven seconds needs to know which of them it is spending them in.
  // Nested inside the host's js::RunScript scope, so the self-time column
  // splits that row three ways.
  std::optional<util::PerformanceTrace::Scope> phase;
  phase.emplace("js::Parse");
  ParseResult parsed = Parse(source);
  if (!parsed.errors.empty()) {
    // A syntax error is a thrown SyntaxError, so a caller has one failure path
    // rather than two.
    return Throw("SyntaxError", parsed.errors.front().message + " (line " +
                                    std::to_string(parsed.errors.front().line) + ")");
  }
  // The tree is kept for as long as the interpreter lives, and it has to be.
  //
  // A function object holds raw `Node*` at its parameters and its body -- the
  // tree is the code. So a function that outlives the call that created it
  // outlives its own source unless the source is kept, and *every* callback is
  // one of those: an event listener, a promise reaction, a `setTimeout`. Until
  // something invoked one of them after its script had finished, this was a
  // use-after-free nobody had reached.
  //
  // The cost is one AST per script for the life of the page, which is what
  // every engine pays for the same reason. A bytecode VM changes the shape of
  // what is retained, not whether something is.
  programs_.push_back(std::move(parsed.program));
  const Node& program = *programs_.back();

  // The compiler is the default and the tree-walker is the fallback, in that
  // order and for two reasons. Compilation can fail -- a construct with no
  // opcode yet rejects the whole program, since half a chunk is not runnable --
  // and something has to run it. And the two engines answering the same suite
  // is the only way to know they agree: MICROBROWSER_JS_TREEWALK=1 runs
  // everything through the old one, which is how a difference gets found on
  // purpose rather than by a page.
  static const bool tree_walk = util::EnvFlagEnabled("MICROBROWSER_JS_TREEWALK");
  if (!tree_walk) {
    phase.emplace("js::Compile");
    std::unique_ptr<CompiledFunction> compiled = Compile(program, source.size());
    if (compiled != nullptr) {
      vm_.programs.push_back(std::move(compiled));
      phase.emplace("js::Execute");
      return RunCompiled(*vm_.programs.back());
    }
  }
  // Reaching here is a compile bailout, and the label says so rather than
  // sharing a row with the machine: a program on the tree-walker is between one
  // and two orders of magnitude slower, and the counter that says which engine
  // ran it is invisible from a timing alone.
  phase.emplace("js::ExecuteTreeWalk");
  return RunProgram(program);
}

Result Interpreter::RunCompiled(const CompiledFunction& program, Environment* scope) {
  // A top-level script is a host turn. Microtasks and nested CallCompiled
  // (custom-element reactions during appendChild) share this budget — resetting
  // per CallCompiled let a youtube.com microtask storm run forever (each job
  // got a fresh 20M after kevlar spent the first one).
  BeginHostTurn();
  const std::size_t entry_depth = vm_.frames.size();
  const std::size_t callee_slot = vm_.stack.size();
  // The top level gets the same frame shape as a call, so that one set of
  // arithmetic serves both: a callee slot to write the result into, a receiver,
  // and no arguments.
  vm_.stack.push_back(Value::Undefined());
  vm_.stack.push_back(Value::Undefined());
  Frame frame;
  frame.code = &program;
  frame.scope = scope == nullptr ? global_scope_ : scope;
  frame.stack_base = callee_slot;
  frame.argument_base = callee_slot + 2;
  frame.scope_base = vm_.scopes.size();
  frame.locals_base = vm_.locals.size();
  frame.iteration_base = vm_.iterations.size();
  vm_.frames.push_back(frame);

  // The chunk ends by returning its completion slot, so what comes back here is
  // already the value a console would print.
  Result result = RunFrames(entry_depth);
  vm_.stack.resize(callee_slot);

  // Rooted across the drain, because draining is where the collector runs and
  // this value is a C++ local until the caller has it. See ValueRoot.
  const ValueRoot rooted(*this, result.value);
  if (result.IsAbrupt()) {
    // The queue is drained even when the script threw. A promise settled before
    // the throw still has handlers owed to it, and dropping them would leave a
    // page half-run rather than merely broken.
    DrainMicrotasks();
    return result;
  }
  DrainMicrotasks();
  MaybeCollect();
  return result;
}

Result Interpreter::RunProgram(const Node& program) {
  BeginHostTurn();
  HoistDeclarations(program, *global_scope_);
  // A script's top level is a function scope for `var`'s purposes.
  HoistVars(program, *global_scope_);
  Value last;
  std::optional<ValueRoot> rooted_last;
  for (const NodePtr& statement : program.children) {
    if (statement == nullptr) {
      continue;
    }
    Result result = EvaluateStatement(*statement, *global_scope_);
    if (result.IsAbrupt()) {
      // The queue is drained even when the script threw. A promise settled
      // before the throw still has handlers owed to it, and dropping them
      // would leave a page half-run rather than merely broken.
      const ValueRoot rooted_thrown(*this, result.value);
      DrainMicrotasks();
      return result;
    }
    last = result.value;
    // Re-rooted every statement, because the completion value has to survive
    // the *next* one: it is a C++ local, and the collection below is exactly
    // where a C++ local goes missing. `emplace` drops the previous root first,
    // which is the innermost one this function holds.
    rooted_last.emplace(*this, last);
    // Only here, at the top level with nothing in progress, is every live
    // value reachable from the roots. See the note in Heap.h.
    MaybeCollect();
  }
  // The end of the turn, which is the only place microtasks run. See the note
  // on DrainMicrotasks: this is a wakeup that was already happening.
  DrainMicrotasks();
  MaybeCollect();
  return Result::Normal(std::move(last));
}

}  // namespace microbrowser::js

#include "js/Interpreter.h"

#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include "js/BuiltinSupport.h"
#include "util/Env.h"

namespace microbrowser::js {

namespace {

// After this many allocations, tracing is cheaper than the memory it would
// reclaim. A number rather than a heuristic because the alternative is a
// heuristic nobody has measured.
constexpr std::size_t kCollectionThreshold = 4096;

}  // namespace

Interpreter::Interpreter() {
  // Reserved once, and never allowed to grow past it. The machine takes
  // references into the value stack and a reallocation would invalidate every
  // one of them, so a fixed capacity makes that safe by construction; see the
  // note at the top of Vm.cpp.
  vm_.stack.reserve(kValueStackCapacity);
  vm_.frames.reserve(kFrameCapacity);
  global_ = heap_.AllocateObject(Object::Kind::Plain);
  global_scope_ = heap_.AllocateEnvironment(nullptr);
  well_known_.object_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.array_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.function_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.string_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.regexp_prototype = heap_.AllocateObject(Object::Kind::Plain);
  well_known_.promise_prototype = heap_.AllocateObject(Object::Kind::Plain);
  InstallGlobals();
}

Interpreter::~Interpreter() = default;

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
  error->Set("name", Value::String(std::string(kind)));
  error->Set("message", Value::String(std::move(message)));
  return Value::Obj(error);
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
  if (heap_.AllocationsSinceCollection() < kCollectionThreshold || call_depth_ != 0) {
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
  std::vector<Environment*> environment_roots{global_scope_};
  environment_roots.insert(environment_roots.end(), active_scopes_.begin(), active_scopes_.end());
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
  if (node.number != 0.0) {
    // An async function, and this is the tree-walker's function -- the machine
    // makes its own in the Closure opcode. A tree-walker cannot run one: its
    // state is C++ stack frames and `await` has nowhere to put one down. So
    // calling it says so, rather than returning something that is not a
    // promise and letting the difference surface three lines later.
    Object* refuser = NewNative(node.string.c_str(), [](NativeCall& call) {
      return call.Throw("TypeError",
                        "an async function needs the bytecode machine; this program fell back "
                        "to the tree-walking interpreter");
    });
    return refuser == nullptr ? Value::Undefined() : Value::Obj(refuser);
  }
  Object* function = heap_.AllocateObject(Object::Kind::Function);
  if (function == nullptr) {
    return Value::Undefined();
  }
  function->SetPrototype(well_known_.function_prototype);
  function->MakeFunction(node.Child(0), node.Child(1), &scope, arrow);
  function->Set("name", Value::String(node.string));
  const Node* parameters = node.Child(0);
  function->Set("length",
                Value::Number(parameters == nullptr
                                  ? 0.0
                                  : static_cast<double>(parameters->children.size())));
  if (!arrow) {
    // Every ordinary function gets a fresh `prototype` object, because any of
    // them can be called with `new`. An arrow function cannot, which is why it
    // does not get one.
    if (Object* prototype = NewObject()) {
      prototype->Set("constructor", Value::Obj(function));
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
  function->Set("name", Value::String(code.name));
  function->Set("length", Value::Number(static_cast<double>(code.parameter_count)));
  if (!arrow) {
    if (Object* prototype = NewObject()) {
      prototype->Set("constructor", Value::Obj(function));
      function->Set("prototype", Value::Obj(prototype));
    }
  }
  return Value::Obj(function);
}

// --- Property access -------------------------------------------------------

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



Value Interpreter::GetProperty(const Value& base, const PropertyKey& key) {
  if (base.IsObject() && base.object->GetKind() == Object::Kind::Proxy) {
    Value target;
    if (Object* trap = ProxyTrap(base, "get", target)) {
      // (target, key, receiver), and the receiver is the proxy -- so a getter
      // reached through it sees the proxy as `this`, which is what makes a
      // reactive object notice a read of a computed property.
      const Result got = CallFunction(Value::Obj(trap), Value::Undefined(),
                                      {target, KeyValue(key), base});
      return got.IsAbrupt() ? Value::Undefined() : got.value;
    }
    return target.IsUndefined() ? Value::Undefined() : GetProperty(target, key);
  }
  // A symbol key names none of the built-in structure below -- there is no
  // symbol spelled "length" and no symbol that is an array index -- so those
  // tests are guarded rather than repeated inside each one.
  const bool named = !key.IsSymbol();
  if (base.IsString()) {
    const std::string& text = base.AsString();
    if (named && key.Text() == "length") {
      return Value::Number(static_cast<double>(text.size()));
    }
    if (const std::optional<std::size_t> index =
            named ? ParseArrayIndex(key.Text()) : std::nullopt) {
      return *index < text.size() ? Value::String(std::string(1, text[*index]))
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
      const Result got = CallFunction(Value::Obj(property->getter), base, {});
      return got.IsAbrupt() ? Value::Undefined() : got.value;
    }
    return property->IsAccessor() ? Value::Undefined() : property->value;
  }
  if (!base.IsObject()) {
    return Value::Undefined();
  }

  Object* object = base.object;
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
    const Result got = CallFunction(Value::Obj(property->getter), base, {});
    return got.IsAbrupt() ? Value::Undefined() : got.value;
  }
  if (property->setter != nullptr) {
    return Value::Undefined();  // set-only: reading gives undefined
  }
  return property->value;
}

Result Interpreter::SetProperty(const Value& base, const PropertyKey& key, const Value& value) {
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
  const CompiledFunction* code = callee.object->Code();
  if (code != nullptr && code->is_async) {
    // An async function returns a promise, so `new` on one has nothing to hand
    // back that is an instance. The spec says TypeError and this is the only
    // place that can tell.
    return Throw("TypeError", "an async function is not a constructor");
  }
  Object* instance = NewObject();
  if (instance == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  const Value* prototype = callee.object->Get("prototype");
  if (prototype != nullptr && prototype->IsObject()) {
    instance->SetPrototype(prototype->object);
  }
  const Value self = Value::Obj(instance);
  active_objects_.push_back(instance);
  // A base class initializes its fields before the constructor body runs. A
  // derived one does it after its super() call instead, which is the ordering
  // that lets a derived field read a base one.
  Object* parent = callee.object->SuperConstructor();
  if (parent == nullptr) {
    const Result fields = InitializeFields(instance, callee.object);
    if (fields.IsAbrupt()) {
      active_objects_.pop_back();
      return fields;
    }
  } else if (callee.object->Body() == nullptr && callee.object->Code() == nullptr) {
    // A derived class with no explicit constructor gets an implicit
    // `constructor(...args){ super(...args) }`. Without it, `class B extends A
    // { n = 5 }` runs no constructor at all and leaves both the base's state
    // and its own fields unset.
    const Result base = CallFunction(Value::Obj(parent), self, arguments);
    if (base.IsAbrupt()) {
      active_objects_.pop_back();
      return base;
    }
    const Result fields = InitializeFields(instance, callee.object);
    if (fields.IsAbrupt()) {
      active_objects_.pop_back();
      return fields;
    }
  }
  const Result constructed = CallFunction(callee, self, arguments);
  active_objects_.pop_back();
  if (constructed.IsAbrupt()) {
    return constructed;
  }
  // A constructor returning an object replaces the instance; returning a
  // primitive does not. The rule exists so a factory can be a constructor.
  return Result::Normal(constructed.value.IsObject() ? constructed.value : self);
}

Result Interpreter::CallFunction(const Value& callee, const Value& self,
                                 const std::vector<Value>& arguments) {
  if (!callee.IsObject() || !callee.object->IsCallable()) {
    return Throw("TypeError", ToString(callee) + " is not a function");
  }
  if (call_depth_ + static_cast<int>(vm_.frames.size()) >= kMaxCallDepth) {
    // A page can write unbounded recursion, and the C++ stack is what would
    // run out. A RangeError is what the language says happens.
    return Throw("RangeError", "maximum call stack size exceeded");
  }

  Object* function = callee.object;
  if (function->Code() != nullptr) {
    // A compiled body, entered from C++ -- a native calling a callback, the
    // host dispatching an event, or the tree-walker calling a function the
    // compiler got to. The depth goes up for the duration because the C++
    // frame underneath holds values the collector cannot see, and that is
    // exactly what makes it unsafe to collect at a safepoint inside.
    ++call_depth_;
    Result result = CallCompiled(function, self, arguments);
    --call_depth_;
    return result;
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
  scope->Declare("this", function->IsArrow() ? function->BoundThis() : self, true);
  // What `super` needs: the object the method was defined on, and the function
  // itself (for its superclass). Bound here rather than looked up later,
  // because by the time super runs the C++ frame that knew them is gone.
  if (function->HomeObject() != nullptr) {
    scope->Declare("__home__", Value::Obj(function->HomeObject()), true);
  }
  scope->Declare("__function__", callee, true);
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
    if (std::unique_ptr<CompiledFunction> compiled = Compile(program)) {
      vm_.programs.push_back(std::move(compiled));
      return RunCompiled(*vm_.programs.back());
    }
  }
  return RunProgram(program);
}

Result Interpreter::RunCompiled(const CompiledFunction& program) {
  steps_ = 0;
  const std::size_t entry_depth = vm_.frames.size();
  const std::size_t callee_slot = vm_.stack.size();
  // The top level gets the same frame shape as a call, so that one set of
  // arithmetic serves both: a callee slot to write the result into, a receiver,
  // and no arguments.
  vm_.stack.push_back(Value::Undefined());
  vm_.stack.push_back(Value::Undefined());
  Frame frame;
  frame.code = &program;
  frame.scope = global_scope_;
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
  steps_ = 0;
  HoistDeclarations(program, *global_scope_);
  Value last;
  for (const NodePtr& statement : program.children) {
    if (statement == nullptr) {
      continue;
    }
    Result result = EvaluateStatement(*statement, *global_scope_);
    if (result.IsAbrupt()) {
      // The queue is drained even when the script threw. A promise settled
      // before the throw still has handlers owed to it, and dropping them
      // would leave a page half-run rather than merely broken.
      DrainMicrotasks();
      return result;
    }
    last = std::move(result.value);
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

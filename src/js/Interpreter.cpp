#include "js/Interpreter.h"

#include <cmath>
#include <optional>
#include <utility>

#include "js/BuiltinSupport.h"

namespace microbrowser::js {

namespace {

// After this many allocations, tracing is cheaper than the memory it would
// reclaim. A number rather than a heuristic because the alternative is a
// heuristic nobody has measured.
constexpr std::size_t kCollectionThreshold = 4096;

}  // namespace

Interpreter::Interpreter() {
  global_ = heap_.AllocateObject(Object::Kind::Plain);
  global_scope_ = heap_.AllocateEnvironment(nullptr);
  object_prototype_ = heap_.AllocateObject(Object::Kind::Plain);
  array_prototype_ = heap_.AllocateObject(Object::Kind::Plain);
  function_prototype_ = heap_.AllocateObject(Object::Kind::Plain);
  string_prototype_ = heap_.AllocateObject(Object::Kind::Plain);
  InstallGlobals();
}

Interpreter::~Interpreter() = default;

Object* Interpreter::NewObject() {
  Object* object = heap_.AllocateObject(Object::Kind::Plain);
  if (object == nullptr) {
    return nullptr;  // the heap is full; the caller turns this into a RangeError
  }
  object->SetPrototype(object_prototype_);
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
  array->SetPrototype(array_prototype_);
  array->SetElements(std::move(elements), std::move(present));
  return array;
}

Value Interpreter::MakeError(std::string_view kind, std::string message) {
  Object* error = heap_.AllocateObject(Object::Kind::Error);
  if (error == nullptr) {
    // Out of memory while reporting being out of memory. A string is the one
    // thing that can still be made, and losing the error type is better than
    // recursing here.
    return Value::String(std::string(kind) + ": " + message);
  }
  error->SetPrototype(object_prototype_);
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
  std::vector<Object*> object_roots{global_, object_prototype_, array_prototype_,
                                    function_prototype_, string_prototype_};
  object_roots.insert(object_roots.end(), active_objects_.begin(), active_objects_.end());
  std::vector<Environment*> environment_roots{global_scope_};
  environment_roots.insert(environment_roots.end(), active_scopes_.begin(), active_scopes_.end());
  heap_.Collect(object_roots, environment_roots);
}

Value Interpreter::NewFunction(const Node& node, Environment& scope, bool arrow) {
  Object* function = heap_.AllocateObject(Object::Kind::Function);
  if (function == nullptr) {
    return Value::Undefined();
  }
  function->SetPrototype(function_prototype_);
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

// --- Property access -------------------------------------------------------

Value Interpreter::GetProperty(const Value& base, std::string_view key) {
  if (base.IsString()) {
    const std::string& text = base.AsString();
    if (key == "length") {
      return Value::Number(static_cast<double>(text.size()));
    }
    if (const std::optional<std::size_t> index = ParseArrayIndex(key)) {
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
        string_prototype_ == nullptr ? nullptr : string_prototype_->GetProperty(key);
    return method == nullptr || method->IsAccessor() ? Value::Undefined() : method->value;
  }
  if (!base.IsObject()) {
    return Value::Undefined();
  }

  Object* object = base.object;
  if (object->GetKind() == Object::Kind::Array) {
    if (key == "length") {
      return Value::Number(static_cast<double>(object->ElementCount()));
    }
    if (const std::optional<std::size_t> index = ParseArrayIndex(key)) {
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

Result Interpreter::SetProperty(const Value& base, std::string_view key, const Value& value) {
  if (!base.IsObject()) {
    // Assigning to a property of a primitive is a silent no-op outside strict
    // mode and a TypeError inside it. Null and undefined are always an error,
    // which is the one people actually hit.
    if (base.IsNullish()) {
      return Throw("TypeError",
                   "cannot set property '" + std::string(key) + "' of " + ToString(base));
    }
    return Result::Normal(value);
  }

  Object* object = base.object;
  if (object->GetKind() == Object::Kind::Array) {
    if (key == "length") {
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
    if (const std::optional<std::size_t> index = ParseArrayIndex(key)) {
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
  object->Set(std::string(key), value);
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

Result Interpreter::CallFunction(const Value& callee, const Value& self,
                                 const std::vector<Value>& arguments) {
  if (!callee.IsObject() || !callee.object->IsCallable()) {
    return Throw("TypeError", ToString(callee) + " is not a function");
  }
  if (call_depth_ >= kMaxCallDepth) {
    // A page can write unbounded recursion, and the C++ stack is what would
    // run out. A RangeError is what the language says happens.
    return Throw("RangeError", "maximum call stack size exceeded");
  }

  Object* function = callee.object;
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
  const ParseResult parsed = Parse(source);
  if (!parsed.errors.empty()) {
    // A syntax error is a thrown SyntaxError, so a caller has one failure path
    // rather than two.
    return Throw("SyntaxError", parsed.errors.front().message + " (line " +
                                    std::to_string(parsed.errors.front().line) + ")");
  }
  return RunProgram(*parsed.program);
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
      return result;
    }
    last = std::move(result.value);
    // Only here, at the top level with nothing in progress, is every live
    // value reachable from the roots. See the note in Heap.h.
    MaybeCollect();
  }
  return Result::Normal(std::move(last));
}

}  // namespace microbrowser::js

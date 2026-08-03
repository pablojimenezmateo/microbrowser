#include <cstddef>
#include <string>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// Function.prototype: call, apply and bind.
//
// The three ways script redirects `this`, and the reason they matter more here
// than their size suggests: every one of them is how a method gets used away
// from the object it was written on, which is what a DOM event handler, a
// borrowed array method and a partially applied callback all are.
//
// `bind` is the one with a real design problem. The function it returns has to
// remember a target, a `this`, and a list of arguments, and the obvious place
// to put them -- captured in the std::function -- is a place the collector
// cannot see. It would mark the bound function, find no reference to the
// target, free the target, and leave the capture dangling. So the state lives
// in properties on the bound function instead, where the existing property
// marking already finds it, and the native reads them back off `call.callee`.
//
// The cost is that the properties are visible to script as `__target__`,
// `__boundThis__` and `__boundArguments__`. A real engine hides them in
// internal slots. Visible-but-collected beats hidden-but-freed, and the day
// Object grows internal slots this moves into them.

namespace microbrowser::js {

namespace {

constexpr const char* kTarget = "__target__";
constexpr const char* kBoundThis = "__boundThis__";
constexpr const char* kBoundArguments = "__boundArguments__";

// A callable receiver, or nothing. All three of these are generic in the spec
// only up to the point of requiring a function, and saying so is better than
// the "undefined is not a function" that falls out otherwise.
Object* CallableSelf(NativeCall& call, const char* method) {
  if (call.self.IsObject() && call.self.object->IsCallable()) {
    return call.self.object;
  }
  call.Throw("TypeError",
             std::string("Function.prototype.") + method + " called on a non-function");
  return nullptr;
}

// The elements of an array argument, as a list. `apply` takes an array where
// `call` takes a parameter list, and that is the only difference between them.
std::vector<Value> ArrayElements(const Value& value) {
  std::vector<Value> elements;
  if (!value.IsObject() || value.object->GetKind() != Object::Kind::Array) {
    return elements;
  }
  elements.reserve(value.object->ElementCount());
  for (std::size_t i = 0; i < value.object->ElementCount(); ++i) {
    elements.push_back(value.object->GetElement(i));
  }
  return elements;
}

// Hands the result of an inner call back to the outer native, turning an
// abrupt completion into a throw rather than losing it.
Value Forward(NativeCall& call, const Result& result) {
  return result.IsAbrupt() ? call.ThrowValue(result.value) : result.value;
}

}  // namespace

void Interpreter::InstallFunctionPrototype() {
  // `Function` exists so that `Function.prototype` is reachable, and refuses to
  // be called. Compiling a function from a source string is the one path by
  // which a page turns data it received into code it runs, and this engine does
  // not have it -- there is no `eval` either. Saying so with a TypeError is
  // better than leaving the name undefined, because a page that feature-detects
  // gets an answer instead of a ReferenceError.
  Object* constructor = NewNative("Function", [](NativeCall& call) {
    return call.Throw("TypeError", "compiling a function from source is not supported");
  });
  constructor->Set("prototype", Value::Obj(well_known_.function_prototype));
  well_known_.function_prototype->Set("constructor", Value::Obj(constructor));
  global_scope_->Declare("Function", Value::Obj(constructor), false);

  InstallNative(well_known_.function_prototype, "call", [](NativeCall& call) {
    Object* target = CallableSelf(call, "call");
    if (target == nullptr) {
      return Value::Undefined();
    }
    // The first argument is the receiver; the rest are the call's own.
    const std::vector<Value> arguments(
        call.arguments.begin() + (call.arguments.empty() ? 0 : 1), call.arguments.end());
    return Forward(call, call.interpreter.CallFunction(Value::Obj(target),
                                                       Argument(call.arguments, 0), arguments));
  });

  InstallNative(well_known_.function_prototype, "apply", [](NativeCall& call) {
    Object* target = CallableSelf(call, "apply");
    if (target == nullptr) {
      return Value::Undefined();
    }
    return Forward(call, call.interpreter.CallFunction(Value::Obj(target),
                                                       Argument(call.arguments, 0),
                                                       ArrayElements(Argument(call.arguments, 1))));
  });

  InstallNative(well_known_.function_prototype, "bind", [](NativeCall& call) {
    Object* target = CallableSelf(call, "bind");
    if (target == nullptr) {
      return Value::Undefined();
    }
    Object* bound = call.interpreter.NewNative("bound", [](NativeCall& inner) {
      // Everything this needs is on the function object it was reached
      // through, which is what keeps it alive across a collection.
      if (inner.callee == nullptr) {
        return Value::Undefined();
      }
      const Value* target_value = inner.callee->GetOwn(kTarget);
      const Value* bound_this = inner.callee->GetOwn(kBoundThis);
      const Value* bound_arguments = inner.callee->GetOwn(kBoundArguments);
      if (target_value == nullptr || bound_this == nullptr || bound_arguments == nullptr) {
        return inner.Throw("TypeError", "bound function lost its target");
      }
      // The bound arguments come first and the call's own follow, which is
      // what makes bind partial application rather than a receiver swap.
      std::vector<Value> arguments = ArrayElements(*bound_arguments);
      arguments.insert(arguments.end(), inner.arguments.begin(), inner.arguments.end());
      return Forward(inner, inner.interpreter.CallFunction(*target_value, *bound_this, arguments));
    });
    if (bound == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    const std::vector<Value> rest(call.arguments.begin() + (call.arguments.empty() ? 0 : 1),
                                  call.arguments.end());
    bound->Set(kTarget, Value::Obj(target));
    bound->Set(kBoundThis, Argument(call.arguments, 0));
    bound->Set(kBoundArguments, call.interpreter.NewArrayValue(rest));
    const Value* name = target->GetOwn("name");
    bound->Set("name", Value::String("bound " + (name == nullptr ? std::string() : ToString(*name))));
    return Value::Obj(bound);
  });
}

}  // namespace microbrowser::js

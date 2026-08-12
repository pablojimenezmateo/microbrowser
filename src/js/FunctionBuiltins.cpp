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
  // `eval` and `Function(source)` compile a string into running code. They are
  // gated only by CSP `'unsafe-eval'` at the host (ADR 0039); without a policy
  // that forbids them they exist, which is what BotGuard / WebPO on youtube.com
  // needs (`(0,eval)(...)` in the challenge interpreter). Refusing them outright
  // left Wq2 waiting forever on `wne()` and blocked every SPA player fetch
  // (TD-0024).
  Object* eval_fn = NewNative("eval", [](NativeCall& call) {
    if (call.arguments.empty()) {
      return Value::Undefined();
    }
    const Value& argument = Argument(call.arguments, 0);
    if (argument.type != ValueType::String) {
      // Non-strings are returned unchanged (indirect-eval shape BotGuard uses
      // for Trusted-Types probes before it feeds a real source string).
      return argument;
    }
    if (call.interpreter.eval_forbidden_ != nullptr &&
        call.interpreter.eval_forbidden_(call.interpreter.eval_forbidden_context_)) {
      return call.Throw("EvalError", "call to eval() blocked by CSP");
    }
    // Always global scope: `(0,eval)` is indirect, and that is the only form
    // measured on youtube. Direct-eval scope chaining is a follow-up.
    const Result ran = call.interpreter.Run(argument.AsString());
    return ran.IsAbrupt() ? call.ThrowValue(ran.value) : ran.value;
  });
  global_scope_->Declare("eval", Value::Obj(eval_fn), false);

  Object* constructor = NewNative("Function", [](NativeCall& call) {
    if (call.interpreter.eval_forbidden_ != nullptr &&
        call.interpreter.eval_forbidden_(call.interpreter.eval_forbidden_context_)) {
      return call.Throw("EvalError", "Function() blocked by CSP");
    }
    // `new Function(a, b, body)` → `function anonymous(a,b){ body }`.
    std::string body = "return undefined;";
    std::string params;
    if (!call.arguments.empty()) {
      body = ToString(call.arguments.back());
      for (std::size_t i = 0; i + 1 < call.arguments.size(); ++i) {
        if (i > 0) {
          params += ',';
        }
        params += ToString(call.arguments[i]);
      }
    }
    // Expression form so `Run`'s completion value *is* the function, rather
    // than a declaration that only leaves a global binding.
    const std::string source = "(function anonymous(" + params + "){\n" + body + "\n})";
    const Result ran = call.interpreter.Run(source);
    return ran.IsAbrupt() ? call.ThrowValue(ran.value) : ran.value;
  });
  constructor->Set("prototype", Value::Obj(well_known_.function_prototype));
  well_known_.function_prototype->SetHidden("constructor", Value::Obj(constructor));
  global_scope_->Declare("Function", Value::Obj(constructor), false);

  InstallNative(well_known_.function_prototype, "toString", [](NativeCall& call) {
    // The source text is not kept -- a function object points at its AST or at
    // a chunk, and neither carries the span it came from -- so this is the
    // form the spec allows for anything whose source is unavailable. Pages use
    // it to read a function's name and to sniff whether something is native,
    // and both of those work.
    if (!call.self.IsObject() || !call.self.object->IsCallable()) {
      return call.Throw("TypeError", "Function.prototype.toString on a non-function");
    }
    const Value* name = call.self.object->GetOwn("name");
    const std::string text = name == nullptr ? std::string() : ToString(*name);
    if (call.self.object->GetKind() == Object::Kind::Native) {
      return Value::String("function " + text + "() { [native code] }");
    }
    return Value::String("function " + text + "() { [source unavailable] }");
  });

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
      // `new bound()` must Construct the target and ignore the bound `this`.
      // That is ECMA-262 BoundFunction [[Construct]], and it is not optional:
      // youtube's DI instantiates every provider with
      // `new (Function.prototype.bind.apply(Class, [null].concat(deps)))`,
      // and without this path `this` inside the class is the bound null --
      // `cannot set property 'store' of null` -- so the injector never
      // finishes registering and the page stays a white shell.
      if (inner.interpreter.IsConstructCall(inner.self)) {
        return Forward(inner, inner.interpreter.ConstructValue(*target_value, arguments));
      }
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
    bound->SetHidden("name",
                     Value::String("bound " + (name == nullptr ? std::string() : ToString(*name))));
    return Value::Obj(bound);
  });
}

}  // namespace microbrowser::js

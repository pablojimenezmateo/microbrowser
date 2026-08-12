#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// `Error` and the seven NativeError kinds.
//
// In their own translation unit because they are one feature with a shape the
// rest of the builtins do not have: a *chain of constructors*. `TypeError`
// inherits from `Error` -- `Object.getPrototypeOf(TypeError) === Error` -- and
// `TypeError.prototype` inherits from `Error.prototype`, and getting either
// wrong is invisible until something walks one. web-platform-tests'
// `assert_throws_js` walks the first on every negative test in the suite, which
// made one missing pointer worth 117 tests and 2,014 subtests.
//
// The other rule that runs through this file: every own property of an error
// instance is non-enumerable. `Object.keys(new TypeError('x'))` is empty, and a
// page that spreads or JSON-stringifies a caught error must not find `message`
// or `stack` in it.

namespace microbrowser::js {

void Interpreter::InstallErrors() {
  // Each kind gets its own constructor so that `e instanceof TypeError` has
  // something to be true of, and so a caught error prints as the kind it is.
  Object* error_prototype = NewObject();
  // `Error` itself, filled in by the first call below and read by every call
  // after it: a NativeError constructor *inherits from* `Error`, so
  // `Object.getPrototypeOf(TypeError)` is `Error` and not `Function.prototype`
  // (ECMA-262 §20.5.6.1). Nothing in a page reads that chain -- and
  // web-platform-tests' `assert_throws_js` walks it on every negative test in
  // the suite, which is 117 tests and 2,014 subtests of "is not an Error
  // subtype" for a missing pointer.
  Object* error_constructor = nullptr;
  const auto error_kind = [this, error_prototype, &error_constructor](const char* name) {
    Object* prototype = name == std::string_view("Error") ? error_prototype : NewObject();
    if (prototype == nullptr) {
      return;
    }
    if (prototype != error_prototype) {
      prototype->SetPrototype(error_prototype);
    }
    prototype->SetHidden("name", Value::String(name));
    Object* constructor = NewNative(name, [](NativeCall& call) {
      // Callable with or without `new`: `Error('x')` and `new Error('x')` are
      // the same thing, which is one of the few places the language says so.
      //
      // And callable as a *base*: `class HttpError extends Error` reaches here
      // through `super(m)` with the instance already allocated, and has to
      // have that one filled in rather than a second one allocated beside it.
      Object* error = ConstructionTarget(call);
      if (error == nullptr) {
        error = call.interpreter.GetHeap().AllocateObject(Object::Kind::Error);
        if (error == nullptr) {
          return call.Throw("RangeError", "out of memory");
        }
        const Value* fresh =
            call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
        if (fresh != nullptr && fresh->IsObject()) {
          error->SetPrototype(fresh->object);
        }
      }
      const Value* prototype_value =
          call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
      // AggregateError takes the errors first and the message second, which
      // is the one place the error constructors disagree about their
      // arguments. Told apart by the prototype's own `name` rather than by a
      // captured flag, because a capture is invisible to the collector.
      const Value* kind = prototype_value != nullptr && prototype_value->IsObject()
                              ? prototype_value->object->GetOwn("name")
                              : nullptr;
      const bool aggregate = kind != nullptr && kind->IsString() &&
                             kind->AsString() == "AggregateError";
      if (aggregate) {
        std::vector<Value> errors;
        const Result collected =
            call.interpreter.CollectIterable(Argument(call.arguments, 0), errors);
        if (collected.IsAbrupt()) {
          return call.ThrowValue(collected.value);
        }
        error->SetHidden("errors", call.interpreter.NewArrayValue(std::move(errors)));
      }
      const Value message = Argument(call.arguments, aggregate ? 1 : 0);
      if (!message.IsUndefined()) {
        std::string text;
        const Result converted = call.interpreter.ToStringOf(message, text);
        if (converted.IsAbrupt()) {
          return call.ThrowValue(converted.value);
        }
        // Non-enumerable, which is what every own property of an error is:
        // `Object.keys(new TypeError('x'))` is empty, and a page that spreads
        // or JSON-stringifies a caught error must not find `message` in it.
        error->SetHidden("message", Value::String(std::move(text)));
      }
      const Value options = Argument(call.arguments, aggregate ? 2 : 1);
      if (options.IsObject()) {
        if (const Value* cause = options.object->GetOwn("cause")) {
          error->SetHidden("cause", *cause);
        }
      }
      // Where it was made. Not part of the language, and every engine has one
      // anyway -- a page's own error reporting reads it, and code that checks
      // `e.stack` before using it is rarer than code that does not.
      error->SetHidden("stack", Value::String(call.interpreter.CaptureStack(
                                    kind == nullptr ? "Error" : ToString(*kind),
                                    ToString(Argument(call.arguments, aggregate ? 1 : 0)))));
      return Value::Obj(error);
    });
    if (constructor == nullptr) {
      return;
    }
    constructor->SetHidden("prototype", Value::Obj(prototype));
    prototype->SetHidden("constructor", Value::Obj(constructor));
    if (std::string_view(name) == "Error") {
      error_constructor = constructor;
    } else if (error_constructor != nullptr) {
      constructor->SetPrototype(error_constructor);
    }
    MarksConstructedKind(constructor, Object::Kind::Error);
    realm_->global_scope->Declare(name, Value::Obj(constructor), false);
  };
  if (error_prototype != nullptr) {
    error_prototype->SetHidden("message", Value::String(""));
    // §20.5.3.4, read off `this` rather than off the receiver's kind. Two
    // things depend on that: an object that is not an Error but has the two
    // properties still prints as one -- which is what `DOMException` is, and
    // what a library's error shim is -- and a *subclass* that overrode `name`
    // prints its own. Reading the kind answered "[object Object]" for both.
    InstallNative(error_prototype, "toString", [](NativeCall& call) {
      if (!call.self.IsObject()) {
        return call.Throw("TypeError", "Error.prototype.toString called on a non-object");
      }
      // Through the interpreter rather than `Object::Get`, because both of
      // these are *accessors* on `DOMException.prototype` and a raw slot read
      // answers undefined for one -- which printed every DOMException as
      // "Error".
      const Value name_value = call.interpreter.GetPropertyValue(call.self, "name");
      const Value message_value = call.interpreter.GetPropertyValue(call.self, "message");
      std::string name = "Error";
      if (!name_value.IsUndefined()) {
        const Result converted = call.interpreter.ToStringOf(name_value, name);
        if (converted.IsAbrupt()) {
          return call.ThrowValue(converted.value);
        }
      }
      std::string message;
      if (!message_value.IsUndefined()) {
        const Result converted = call.interpreter.ToStringOf(message_value, message);
        if (converted.IsAbrupt()) {
          return call.ThrowValue(converted.value);
        }
      }
      if (name.empty()) {
        return Value::String(std::move(message));
      }
      if (message.empty()) {
        return Value::String(std::move(name));
      }
      return Value::String(name + ": " + message);
    });
    error_kind("Error");
    error_kind("TypeError");
    error_kind("RangeError");
    error_kind("SyntaxError");
    error_kind("ReferenceError");
    error_kind("EvalError");
    error_kind("URIError");
    error_kind("AggregateError");
  }

}

}  // namespace microbrowser::js

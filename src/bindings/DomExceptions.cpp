#include "bindings/BindingSupport.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

// `DOMException`, and the one place a binding raises one.
//
// A web API does not throw an `Error`: it throws a `DOMException` whose `name`
// is a value out of WebIDL's error-names table, whose `code` is that name's
// legacy numeric constant, and which is an instance of both `DOMException` and
// `Error`. All four of those are load-bearing:
//
//   * `e.name` is what a page switches on -- `catch (e) { if (e.name ===
//     'QuotaExceededError') }` is written everywhere, and the storage,
//     IndexedDB and MSE paths in this module already depended on it.
//   * `e instanceof DOMException` is what youtube's player asks before it asks
//     anything else, so a missing binding made the whole media path a
//     ReferenceError.
//   * `e.code` is the pre-WebIDL constant. Nothing modern reads it, and
//     web-platform-tests' `assert_throws_dom` checks it on every negative test
//     it runs -- which is why it is here rather than left at zero.
//   * `e.constructor === DOMException` is that same assertion's last check, and
//     it is what tells an exception from *this* global apart from one that came
//     out of another.
//
// Before this file the type was an object with a `name` on it, made in three
// places with three subtly different shapes (`MakeError("DOMException", …)` in
// two of them, which leaves `.name` as the literal string "DOMException"). One
// constructor, one table, one `ThrowDom`.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;


// WebIDL's error names table (§ "DOMException error names"), in its order. The
// second column is the legacy code, which is 0 for every name added after the
// numbering was abandoned -- that is not a gap in this table, it is what the
// specification says, and `assert_throws_dom` reads a 0 as "do not check".
// The legacy constant is spelled out rather than derived from the name: the
// derivation is "upper-case, underscore before each capital, `Error` becomes
// `ERR`" and `URLMismatchError` breaks it. A table with one irregular entry
// beats an algorithm with one exception in it.
struct ErrorName {
  std::string_view name;
  int code;
  std::string_view constant;
};

// The twenty-five legacy code constants, in numeric order, which is the order Web IDL lists them
// in and therefore the order they enumerate in.
//
// Their own table rather than a column of the one below, because five of them (2, 6, 16, 17, 21)
// name no error the modern table has: the numbering was abandoned mid-way, and the constants that
// outlived their names still have to be there and still have to be in order.
struct LegacyConstant {
  std::string_view constant;
  int code;
};

constexpr LegacyConstant kLegacyConstants[] = {
    {"INDEX_SIZE_ERR", 1},
    {"DOMSTRING_SIZE_ERR", 2},
    {"HIERARCHY_REQUEST_ERR", 3},
    {"WRONG_DOCUMENT_ERR", 4},
    {"INVALID_CHARACTER_ERR", 5},
    {"NO_DATA_ALLOWED_ERR", 6},
    {"NO_MODIFICATION_ALLOWED_ERR", 7},
    {"NOT_FOUND_ERR", 8},
    {"NOT_SUPPORTED_ERR", 9},
    {"INUSE_ATTRIBUTE_ERR", 10},
    {"INVALID_STATE_ERR", 11},
    {"SYNTAX_ERR", 12},
    {"INVALID_MODIFICATION_ERR", 13},
    {"NAMESPACE_ERR", 14},
    {"INVALID_ACCESS_ERR", 15},
    {"VALIDATION_ERR", 16},
    {"TYPE_MISMATCH_ERR", 17},
    {"SECURITY_ERR", 18},
    {"NETWORK_ERR", 19},
    {"ABORT_ERR", 20},
    {"URL_MISMATCH_ERR", 21},
    {"QUOTA_EXCEEDED_ERR", 22},
    {"TIMEOUT_ERR", 23},
    {"INVALID_NODE_TYPE_ERR", 24},
    {"DATA_CLONE_ERR", 25},
};

constexpr ErrorName kErrorNames[] = {
    {"IndexSizeError", 1, "INDEX_SIZE_ERR"},
    {"HierarchyRequestError", 3, "HIERARCHY_REQUEST_ERR"},
    {"WrongDocumentError", 4, "WRONG_DOCUMENT_ERR"},
    {"InvalidCharacterError", 5, "INVALID_CHARACTER_ERR"},
    {"NoModificationAllowedError", 7, "NO_MODIFICATION_ALLOWED_ERR"},
    {"NotFoundError", 8, "NOT_FOUND_ERR"},
    {"NotSupportedError", 9, "NOT_SUPPORTED_ERR"},
    {"InUseAttributeError", 10, "INUSE_ATTRIBUTE_ERR"},
    {"InvalidStateError", 11, "INVALID_STATE_ERR"},
    {"SyntaxError", 12, "SYNTAX_ERR"},
    {"InvalidModificationError", 13, "INVALID_MODIFICATION_ERR"},
    {"NamespaceError", 14, "NAMESPACE_ERR"},
    {"InvalidAccessError", 15, "INVALID_ACCESS_ERR"},
    {"TypeMismatchError", 17, "TYPE_MISMATCH_ERR"},
    {"SecurityError", 18, "SECURITY_ERR"},
    {"NetworkError", 19, "NETWORK_ERR"},
    {"AbortError", 20, "ABORT_ERR"},
    {"URLMismatchError", 21, "URL_MISMATCH_ERR"},
    {"QuotaExceededError", 22, "QUOTA_EXCEEDED_ERR"},
    {"TimeoutError", 23, "TIMEOUT_ERR"},
    {"InvalidNodeTypeError", 24, "INVALID_NODE_TYPE_ERR"},
    {"DataCloneError", 25, "DATA_CLONE_ERR"},
    // Named after the numbering stopped. Code 0 and no constant, deliberately.
    {"EncodingError", 0, {}},
    {"NotReadableError", 0, {}},
    {"UnknownError", 0, {}},
    {"ConstraintError", 0, {}},
    {"DataError", 0, {}},
    {"TransactionInactiveError", 0, {}},
    {"ReadOnlyError", 0, {}},
    {"VersionError", 0, {}},
    {"OperationError", 0, {}},
    {"NotAllowedError", 0, {}},
    {"OptOutError", 0, {}},
};

int LegacyCodeFor(std::string_view name) {
  for (const ErrorName& entry : kErrorNames) {
    if (entry.name == name) {
      return entry.code;
    }
  }
  // A name that is not in the table -- including the default "Error" -- has no
  // legacy code. Zero rather than a guess: the table is the whole authority on
  // this mapping and inventing an entry would be a wrong answer that reads as
  // a right one.
  return 0;
}

// `DOMException.prototype`, or undefined before it is installed.
Value ExceptionPrototype(js::Interpreter& interpreter) {
  Value* constructor = interpreter.GlobalScope()->Lookup("DOMException");
  if (constructor == nullptr || !constructor->IsObject()) {
    return Value::Undefined();
  }
  const Value* prototype = constructor->object->GetOwn("prototype");
  return prototype == nullptr ? Value::Undefined() : *prototype;
}

// Fills an object in. Shared by the constructor a page calls and by
// `MakeDomException`, because two ways to build one is how `.code` ends up
// disagreeing with `.name`.
//
// The prototype is *not* set here: a `super()` from `class X extends
// DOMException` arrives with the derived prototype already on the instance, and
// writing the base one over it is how a subclass loses its own methods. The
// caller that allocated the object is the one that knows.
void FillException(js::Interpreter& interpreter, const Value& exception, std::string_view name,
                   std::string message) {
  if (!exception.IsObject()) {
    return;
  }
  // Own, non-enumerable data properties -- and `code` computed here, from the
  // name, in the one function that builds one, so the two cannot drift.
  //
  // WebIDL puts these three on the prototype as accessors, and this deliberately
  // does not. An accessor is a *call*, and the two things that print an
  // exception -- `js::ToString`, which every console line and every uncaught
  // report goes through, and the internal `String(e)` -- are pure functions with
  // no interpreter to call one with. A DOMException whose prototype held the
  // getters logged as "[object Object]", which is the single most useful line a
  // failing page produces reduced to nothing. Non-enumerable keeps the visible
  // half of the specified shape: `Object.keys(e)` is empty.
  exception.object->SetHidden("name", Value::String(std::string(name)));
  exception.object->SetHidden("code", Value::Number(static_cast<double>(LegacyCodeFor(name))));
  exception.object->SetHidden("message", Value::String(message));
  // Where it was raised, on the same terms as an error the engine made: not
  // part of any specification, present in every engine, and the only thing that
  // says *where* on a page whose script is one line of a megabyte.
  exception.object->SetHidden(
      "stack", Value::String(interpreter.CaptureStack(name, message)));
}

}  // namespace

Value MakeDomException(js::Interpreter& interpreter, std::string_view name, std::string message) {
  // Allocated as an `Error` rather than a plain object: that is the kind the
  // engine's own printer recognises, and it is what makes an uncaught
  // DOMException reach a console line as "NotFoundError: ..." rather than as
  // "[object Object]".
  js::Object* object = interpreter.GetHeap().AllocateObject(js::Object::Kind::Error);
  const Value exception = object == nullptr ? Value::Undefined() : Value::Obj(object);
  if (!exception.IsObject()) {
    // Out of memory while raising an exception. A string still carries the
    // name, which is more than losing the throw entirely would.
    return Value::String(std::string(name) + ": " + message);
  }
  const Value prototype = ExceptionPrototype(interpreter);
  if (prototype.IsObject()) {
    exception.object->SetPrototype(prototype.object);
  }
  FillException(interpreter, exception, name, std::move(message));
  return exception;
}

Value ThrowDom(NativeCall& call, std::string_view name, std::string message) {
  return call.ThrowValue(MakeDomException(call.interpreter, name, std::move(message)));
}

void InstallDomException(js::Interpreter& interpreter) {
  const Value prototype = interpreter.NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  // A DOMException *is* an Error -- `e instanceof Error` is true in every
  // browser, and a page's `catch (e) { if (e instanceof Error) }` is the
  // fallback path when the `instanceof DOMException` test above it is false.
  if (Value* error = interpreter.GlobalScope()->Lookup("Error");
      error != nullptr && error->IsObject()) {
    if (const Value* error_prototype = error->object->GetOwn("prototype");
        error_prototype != nullptr && error_prototype->IsObject()) {
      prototype.object->SetPrototype(error_prototype->object);
    }
  }

  // The defaults an exception built by something other than this file would
  // read: `DOMException.prototype.name` is "Error" and its code is 0, which is
  // what the specification says an unnamed one is. Every instance shadows all
  // three with its own.
  prototype.object->SetHidden("name", Value::String(std::string("Error")));
  prototype.object->SetHidden("message", Value::String(std::string()));
  prototype.object->SetHidden("code", Value::Number(0.0));

  const Value constructor =
      interpreter.NewNativeValue("DOMException", [](NativeCall& call) -> Value {
        // `new DOMException(message = "", name = "Error")` -- message first,
        // which is the opposite of every other error-shaped constructor here
        // and is what the specification says.
        std::string message;
        const Value message_argument = Argument(call.arguments, 0);
        if (!message_argument.IsUndefined() && !CoerceToString(call, message_argument, message)) {
          return Value::Undefined();
        }
        std::string name = "Error";
        const Value name_argument = Argument(call.arguments, 1);
        if (!name_argument.IsUndefined() && !CoerceToString(call, name_argument, name)) {
          return Value::Undefined();
        }
        // `class MyError extends DOMException` reaches here through `super()`
        // with the instance already allocated and carrying the *derived*
        // prototype. Filling that one in rather than allocating a second is
        // what makes the subclass work at all.
        if (call.interpreter.IsConstructCall(call.self) && call.self.IsObject()) {
          FillException(call.interpreter, call.self, name, std::move(message));
          return call.self;
        }
        return MakeDomException(call.interpreter, name, std::move(message));
      });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->SetHidden("prototype", prototype);
  prototype.object->SetHidden("constructor", constructor);
  // The legacy code constants, on both the constructor and the prototype:
  // `DOMException.NOT_FOUND_ERR` and `e.NOT_FOUND_ERR` are both written, and both were 8 before
  // anyone stopped writing either.
  //
  // **Enumerable**, which is what Web IDL says a constant on an interface object is, and is not a
  // detail: `Object.keys(DOMException)` is these twenty-five names, and anything that converts an
  // interface object to a record sees all of them or none. The URL Standard's own suite converts
  // this one, which is how the non-enumerable version was found.
  for (const LegacyConstant& entry : kLegacyConstants) {
    const std::string constant(entry.constant);
    const Value code = Value::Number(static_cast<double>(entry.code));
    constructor.object->Set(constant, code);
    prototype.object->Set(constant, code);
  }
  interpreter.Global()->Set("DOMException", constructor);
  interpreter.GlobalScope()->Declare("DOMException", constructor, false);
}

}  // namespace microbrowser::bindings

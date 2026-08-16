#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "js/StringUnits.h"
#include <cstdio>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::js {

namespace {

bool IsJsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// The prefix of `text` that parses as a number, or NaN when there is none.
//
// NaN rather than an optional, because NaN *is* what parseFloat and parseInt
// answer with -- an optional here would be a second way to say the same thing,
// unwrapped with `.value_or(NaN)` at the only two call sites. It also stops GCC
// warning that the empty optional may be used uninitialised, which is a false
// positive it only reaches at -O2, and which broke the ASan build.
double ParseFloatPrefix(std::string_view text) {
  std::size_t at = 0;
  while (at < text.size() && IsJsWhitespace(text[at])) {
    ++at;
  }
  const std::size_t start = at;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    ++at;
  }
  if (text.substr(at, 8) == "Infinity") {
    const bool negative = start < text.size() && text[start] == '-';
    return negative ? -std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::infinity();
  }

  bool saw_digit = false;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    saw_digit = true;
    ++at;
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      saw_digit = true;
      ++at;
    }
  }
  if (!saw_digit) {
    return std::nan("");
  }

  if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
    const std::size_t exponent_start = at;
    ++at;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
      ++at;
    }
    const std::size_t exponent_digits = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      ++at;
    }
    if (at == exponent_digits) {
      at = exponent_start;
    }
  }

  std::string_view prefix = text.substr(start, at - start);
  if (!prefix.empty() && prefix.front() == '+') {
    prefix.remove_prefix(1);
  }
  return util::ParseDouble(prefix).value_or(std::nan(""));
}

int DigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 10;
  }
  return -1;
}

double ParseIntPrefix(std::string_view text, int radix) {
  if (radix != 0 && (radix < 2 || radix > 36)) {
    return std::nan("");
  }

  std::size_t at = 0;
  while (at < text.size() && IsJsWhitespace(text[at])) {
    ++at;
  }
  double sign = 1.0;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    sign = text[at] == '-' ? -1.0 : 1.0;
    ++at;
  }
  if ((radix == 0 || radix == 16) && at + 1 < text.size() && text[at] == '0' &&
      (text[at + 1] == 'x' || text[at + 1] == 'X')) {
    radix = 16;
    at += 2;
  } else if (radix == 0) {
    radix = 10;
  }

  double value = 0.0;
  bool saw_digit = false;
  while (at < text.size()) {
    const int digit = DigitValue(text[at]);
    if (digit < 0 || digit >= radix) {
      break;
    }
    value = value * static_cast<double>(radix) + static_cast<double>(digit);
    saw_digit = true;
    ++at;
  }
  return saw_digit ? sign * value : std::nan("");
}

}  // namespace

Object* Interpreter::NewNative(const char* name, NativeFunction function) {
  Object* object = heap_.AllocateObject(Object::Kind::Native);
  if (object == nullptr) {
    // The heap is at its bound (ADR 0034). Null rather than a dereference, and every caller already
    // expects it: `NewNativeValue` answers undefined and `ResolvePromise` returns early on exactly
    // this. This function was the one link in that chain that did not check, and the whole chain was
    // only as good as it.
    //
    // **It was a segfault a page could cause on purpose.** `fetch/metadata/generated/
    // element-video-poster.sub.html` does it by accident: it polls
    // `new Promise(r => step_timeout(r, 0)).then(poll)` until a condition that never becomes true,
    // and each turn allocates the resolve/reject pair that `ResolvePromise` makes here. When the
    // heap fills, the *bound working correctly* turned into a null dereference -- which is the
    // opposite of what a bound is for. ADR 0034 exists so that running out of heap is a RangeError
    // a page can catch, not a crash it can trigger.
    //
    // Invisible under both sanitizers, which is worth knowing before trusting a clean asan run here:
    // the asan build is slow enough that the test times out before the heap fills, and the perf
    // build reaches the limit in five seconds. UBSan found it, because "member call on null" is a
    // thing it checks and a segfault is not.
    return nullptr;
  }
  object->SetPrototype(intrinsics().function_prototype);
  object->MakeNative(std::move(function));
  // Own, non-enumerable, non-writable: that is what a function's `name` is, and
  // idlharness checks all three. Hidden-only made `URL.name` unwritable in
  // appearance to `for...in` but `writable: true` on the descriptor.
  Object::Property name_property;
  name_property.value = Value::String(name);
  name_property.writable = false;
  name_property.enumerable = false;
  object->Define("name", std::move(name_property));
  return object;
}

void Interpreter::InstallNative(Object* target, const char* name, NativeFunction function) {
  Object* native = NewNative(name, std::move(function));
  if (native == nullptr) {
    // Out of heap. The name is simply absent, which is the same answer a page gets for any builtin
    // this browser does not have -- and far better than a property whose value is `Value::Obj(nullptr)`,
    // which is not undefined, is not an object, and would fault at whatever touched it next.
    return;
  }
  target->Set(name, Value::Obj(native));
}

void Interpreter::InstallGlobals() {
  const auto native = [this](const char* name, NativeFunction function) {
    return NewNative(name, std::move(function));
  };
  const auto install = [this](Object* target, const char* name, NativeFunction function) {
    InstallNative(target, name, std::move(function));
  };

  // Every built-in prototype chains to Object.prototype, which is what makes
  // `[].hasOwnProperty` and `(1).toString` resolve at all. Done here rather
  // than at each allocation because forgetting one is invisible until a page
  // calls the one method that lives up the chain.
  for (Object* prototype :
       {intrinsics().array_prototype, intrinsics().function_prototype,
        intrinsics().string_prototype, intrinsics().regexp_prototype,
        intrinsics().promise_prototype}) {
    if (prototype != nullptr) {
      prototype->SetPrototype(intrinsics().object_prototype);
    }
  }

  // First, because every native installed after this one inherits from it.
  InstallFunctionPrototype();

  realm_->global_scope->Declare("globalThis", Value::Obj(realm_->global), false);
  // At the top level of a *script* `this` is the global object, and it stays
  // the global object under `"use strict"` -- strict mode changes what `this`
  // is inside a function, not what it is at the top of a program. A module is
  // the exception and binds its own, in Modules.cpp.
  //
  // This is not a corner: `this.x = this.x || {}` is how a bundle claims its
  // namespace, and `(function (global) { ... })(this)` is how a library found
  // the global object for twenty years. Both read undefined without it, and
  // youtube.com's application bundle fails on its first statement.
  realm_->global_scope->Declare("this", Value::Obj(realm_->global), true);
  realm_->global_scope->Declare("undefined", Value::Undefined(), true);
  realm_->global_scope->Declare("NaN", Value::Number(std::nan("")), true);
  realm_->global_scope->Declare("Infinity", Value::Number(HUGE_VAL), true);

  InstallConsole();


  // --- Math -----------------------------------------------------------------
  Object* math = NewObject();
  math->Set("PI", Value::Number(3.14159265358979323846));
  math->Set("E", Value::Number(2.71828182845904523536));
  install(math, "abs", [](NativeCall& call) {
    return Value::Number(std::fabs(ToNumber(Argument(call.arguments, 0))));
  });
  install(math, "floor", [](NativeCall& call) {
    return Value::Number(std::floor(ToNumber(Argument(call.arguments, 0))));
  });
  install(math, "ceil", [](NativeCall& call) {
    return Value::Number(std::ceil(ToNumber(Argument(call.arguments, 0))));
  });
  install(math, "round", [](NativeCall& call) {
    // JavaScript rounds half *up*, not to even, so -0.5 rounds to -0 and 0.5
    // to 1. std::round rounds half away from zero and gets the negative case
    // wrong.
    const double value = ToNumber(Argument(call.arguments, 0));
    return Value::Number(std::floor(value + 0.5));
  });
  install(math, "sqrt", [](NativeCall& call) {
    return Value::Number(std::sqrt(ToNumber(Argument(call.arguments, 0))));
  });
  install(math, "pow", [](NativeCall& call) {
    return Value::Number(std::pow(ToNumber(Argument(call.arguments, 0)),
                                  ToNumber(Argument(call.arguments, 1))));
  });
  install(math, "min", [](NativeCall& call) {
    double best = HUGE_VAL;
    for (const Value& argument : call.arguments) {
      const double value = ToNumber(argument);
      if (std::isnan(value)) {
        return Value::Number(std::nan(""));  // any NaN makes the answer NaN
      }
      best = std::min(best, value);
    }
    return Value::Number(best);
  });
  install(math, "max", [](NativeCall& call) {
    double best = -HUGE_VAL;
    for (const Value& argument : call.arguments) {
      const double value = ToNumber(argument);
      if (std::isnan(value)) {
        return Value::Number(std::nan(""));
      }
      best = std::max(best, value);
    }
    return Value::Number(best);
  });
  realm_->global_scope->Declare("Math", Value::Obj(math), false);
  // Deferred: it reads `Number`, `parseInt` and `parseFloat` back out of the
  // global scope, and none of them is declared yet at this point.
  const auto install_numbers = [this, math] {
    InstallNumbers(math);
    InstallDate();
    // Last of the three: the typed arrays copy the generic methods off
    // Array.prototype, so every one of them has to exist first.
    InstallTypedArrays();
  };

  // --- JSON -----------------------------------------------------------------
  Object* json = NewObject();
  InstallJsonAndUri(json);
  realm_->global_scope->Declare("JSON", Value::Obj(json), false);

  // --- Object ---------------------------------------------------------------
  Object* object_constructor = native("Object", [](NativeCall& call) {
    return Argument(call.arguments, 0);
  });
  install(object_constructor, "create", [object_constructor](NativeCall& call) {
    const Value prototype = Argument(call.arguments, 0);
    if (!prototype.IsObject() && !prototype.IsNull()) {
      return call.Throw("TypeError", "Object.create prototype must be an object or null");
    }
    Object* object = call.interpreter.NewObject();
    if (object == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    object->SetPrototype(prototype.IsObject() ? prototype.object : nullptr);
    const Value made = Value::Obj(object);
    // The second argument is a descriptor map, and it goes through
    // `defineProperties` rather than being read here -- so a descriptor means
    // one thing in the engine rather than two.
    const Value descriptors = Argument(call.arguments, 1);
    if (descriptors.IsObject()) {
      // core-js keeps `Object.create` in a local and calls it unbound; `this`
      // is then undefined and must not be used to reach back to Object.
      const Value object_value = Value::Obj(object_constructor);
      const Value define = call.interpreter.GetPropertyValue(object_value, "defineProperties");
      const Result defined =
          call.interpreter.CallFunction(define, object_value, {made, descriptors});
      if (defined.IsAbrupt()) {
        return call.ThrowValue(defined.value);
      }
    }
    return made;
  });
  install(object_constructor, "keys", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    std::vector<Value> keys;
    if (target.IsObject()) {
      if (target.object->GetKind() == Object::Kind::Array) {
        for (std::size_t i = 0; i < target.object->ElementCount(); ++i) {
          if (target.object->HasElement(i)) {
            keys.push_back(Value::String(std::to_string(i)));
          }
        }
      }
      for (const std::string& key : call.interpreter.OwnKeys(target, true)) {
        keys.push_back(Value::String(key));
      }
    }
    return call.interpreter.NewArrayValue(std::move(keys));
  });
  install(object_constructor, "values", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    std::vector<Value> values;
    if (target.IsObject()) {
      if (target.object->GetKind() == Object::Kind::Array) {
        for (std::size_t i = 0; i < target.object->ElementCount(); ++i) {
          if (target.object->HasElement(i)) {
            values.push_back(target.object->GetElement(i));
          }
        }
      }
      for (const std::string& key : call.interpreter.OwnKeys(target, true)) {
        values.push_back(call.interpreter.GetPropertyValue(target, key));
      }
    }
    return call.interpreter.NewArrayValue(std::move(values));
  });
  install(object_constructor, "defineProperty", [](NativeCall& call) {
    // The workhorse of every transpiled module and every framework's own
    // property machinery -- and the attributes are the point of it. An
    // omitted attribute defaults to *false* here, which is the opposite of
    // what an ordinary assignment leaves: `Object.defineProperty(exports,
    // '__esModule', {value: true})` is non-enumerable, and every module a
    // bundler emits contains that line.
    const Value target = Argument(call.arguments, 0);
    if (!target.IsObject()) {
      return call.Throw("TypeError", "Object.defineProperty called on a non-object");
    }
    const Value descriptor = Argument(call.arguments, 2);
    if (!descriptor.IsObject()) {
      return call.Throw("TypeError", "a property descriptor must be an object");
    }
    PropertyKey key;
    const Result converted = call.interpreter.ToKeyOf(Argument(call.arguments, 1), key);
    if (converted.IsAbrupt()) {
      return call.ThrowValue(converted.value);
    }
    const auto present = [&descriptor](const char* name) {
      return descriptor.object->GetProperty(name) != nullptr;
    };
    const auto flag = [&call, &descriptor](const char* name) {
      const Value read = call.interpreter.GetPropertyValue(descriptor, name);
      return ToBoolean(read);
    };
    // A *new* property defaults every omitted attribute to false, which is
    // defineProperty's whole difference from assignment. An *existing*
    // configurable property keeps whatever the descriptor does not name:
    // `{value: "Test"}` on a configurable @@toStringTag must not clear
    // configurable, or `delete obj[Symbol.toStringTag]` then fails.
    Object::Property property;
    if (const Object::Property* current = target.object->GetOwnProperty(key)) {
      property = *current;
    } else {
      property.enumerable = false;
      property.configurable = false;
      property.writable = false;
    }
    if (present("enumerable")) {
      property.enumerable = flag("enumerable");
    }
    if (present("configurable")) {
      property.configurable = flag("configurable");
    }
    const Value getter = call.interpreter.GetPropertyValue(descriptor, "get");
    const Value setter = call.interpreter.GetPropertyValue(descriptor, "set");
    if (present("get") || present("set")) {
      property.getter = getter.IsObject() ? getter.object : nullptr;
      property.setter = setter.IsObject() ? setter.object : nullptr;
      property.value = Value::Undefined();
      target.object->Define(std::move(key), std::move(property));
      return target;
    }
    if (present("writable")) {
      property.writable = flag("writable");
    }
    if (present("value")) {
      property.value = call.interpreter.GetPropertyValue(descriptor, "value");
      property.getter = nullptr;
      property.setter = nullptr;
    }
    target.object->Define(std::move(key), std::move(property));
    return target;
  });
  install(object_constructor, "defineProperties", [object_constructor](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    const Value descriptors = Argument(call.arguments, 1);
    if (!target.IsObject() || !descriptors.IsObject()) {
      return call.Throw("TypeError", "Object.defineProperties requires two objects");
    }
    const Value object_value = Value::Obj(object_constructor);
    const Value define = call.interpreter.GetPropertyValue(object_value, "defineProperty");
    for (const std::string& key : descriptors.object->Keys()) {
      const Value* descriptor = descriptors.object->GetOwn(key);
      if (descriptor == nullptr) {
        continue;
      }
      // Through defineProperty rather than around it, so the two cannot
      // disagree about what a descriptor means.
      const Result defined = call.interpreter.CallFunction(
          define, object_value, {target, Value::String(key), *descriptor});
      if (defined.IsAbrupt()) {
        return call.ThrowValue(defined.value);
      }
    }
    return target;
  });
  install(object_constructor, "getOwnPropertyDescriptor", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    if (!target.IsObject()) {
      return Value::Undefined();
    }
    const PropertyKey key = KeyFrom(Argument(call.arguments, 1));
    // An array's indices live in the element storage rather than in the
    // property map, so they have no Property record to describe -- and a page
    // asking about one wants an answer rather than undefined.
    if (target.object->GetKind() == Object::Kind::Array && !key.IsSymbol()) {
      if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
        if (!target.object->HasElement(*index)) {
          return Value::Undefined();
        }
        const Value element = call.interpreter.NewObjectValue();
        if (element.IsObject()) {
          element.object->Set("value", target.object->GetElement(*index));
          element.object->Set("writable", Value::Bool(!target.object->IsFrozen()));
          element.object->Set("enumerable", Value::Bool(true));
          element.object->Set("configurable", Value::Bool(!target.object->IsSealed()));
        }
        return element;
      }
    }
    const Object::Property* property = target.object->GetOwnProperty(key);
    if (property == nullptr) {
      return Value::Undefined();
    }
    const Value descriptor = call.interpreter.NewObjectValue();
    if (!descriptor.IsObject()) {
      return descriptor;
    }
    if (property->IsAccessor()) {
      descriptor.object->Set("get", property->getter == nullptr
                                        ? Value::Undefined()
                                        : Value::Obj(property->getter));
      descriptor.object->Set("set", property->setter == nullptr
                                        ? Value::Undefined()
                                        : Value::Obj(property->setter));
    } else {
      descriptor.object->Set("value", property->value);
      descriptor.object->Set("writable",
                             Value::Bool(property->writable && !target.object->IsFrozen()));
    }
    descriptor.object->Set("enumerable", Value::Bool(property->enumerable));
    descriptor.object->Set("configurable",
                           Value::Bool(property->configurable && !target.object->IsSealed()));
    return descriptor;
  });
  install(object_constructor, "getOwnPropertyNames", [](NativeCall& call) {
    // Every own string key, including an array's indices -- which `keys` also
    // reports, but which live in the element storage rather than the property
    // map, so they have to be listed separately.
    const Value target = Argument(call.arguments, 0);
    std::vector<Value> names;
    if (target.IsObject()) {
      if (target.object->GetKind() == Object::Kind::Array) {
        for (std::size_t i = 0; i < target.object->ElementCount(); ++i) {
          if (target.object->HasElement(i)) {
            names.push_back(Value::String(std::to_string(i)));
          }
        }
        names.push_back(Value::String("length"));
      }
      for (const std::string& key : call.interpreter.OwnKeys(target, false)) {
        names.push_back(Value::String(key));
      }
    }
    return call.interpreter.NewArrayValue(std::move(names));
  });
  install(object_constructor, "entries", [](NativeCall& call) {
    // Own string keys and their values, in insertion order -- which is what
    // `Object.keys` already uses, so the three statics agree by construction.
    std::vector<Value> pairs;
    const Value target = Argument(call.arguments, 0);
    if (target.IsObject()) {
      for (const std::string& key : call.interpreter.OwnKeys(target, true)) {
        pairs.push_back(call.interpreter.NewArrayValue(
            {Value::String(key), call.interpreter.GetPropertyValue(target, key)}));
      }
    }
    return call.interpreter.NewArrayValue(std::move(pairs));
  });
  install(object_constructor, "fromEntries", [](NativeCall& call) {
    const Value built = call.interpreter.NewObjectValue();
    if (!built.IsObject()) {
      return call.Throw("RangeError", "out of memory");
    }
    std::vector<Value> pairs;
    const Result collected =
        call.interpreter.CollectIterable(Argument(call.arguments, 0), pairs);
    if (collected.IsAbrupt()) {
      return call.ThrowValue(collected.value);
    }
    for (const Value& pair : pairs) {
      // Each pair is read with the protocol too, so `Object.fromEntries(map)`
      // works -- which is the call this method mostly exists for.
      std::vector<Value> parts;
      const Result unpacked = call.interpreter.CollectIterable(pair, parts);
      if (unpacked.IsAbrupt()) {
        return call.ThrowValue(unpacked.value);
      }
      built.object->Set(KeyFrom(parts.empty() ? Value::Undefined() : parts[0]),
                        parts.size() < 2 ? Value::Undefined() : parts[1]);
    }
    return built;
  });
  install(object_constructor, "assign", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    if (!target.IsObject()) {
      return call.Throw("TypeError", "cannot assign to a non-object");
    }
    for (std::size_t i = 1; i < call.arguments.size(); ++i) {
      const Value source = call.arguments[i];
      if (!source.IsObject()) {
        continue;  // null and undefined sources are skipped rather than fatal
      }
      // Written through SetProperty rather than `object->Set`, which is the
      // difference between assigning and storing a slot: the specification says
      // assign invokes the target's setter, and a setter is where a host puts a
      // reflected DOM attribute. Writing the slot instead put `name`, `type`
      // and `value` on the wrapper object and left the element unchanged --
      // which is `Object.assign(document.createElement('input'), {...})`, the
      // shape reddit's challenge is written in, doing nothing at all.
      const auto copy = [&call, &target](const PropertyKey& key, const Value& value) {
        return call.interpreter.SetProperty(target, key, value);
      };
      if (source.object->GetKind() == Object::Kind::Array) {
        for (std::size_t index = 0; index < source.object->ElementCount(); ++index) {
          if (source.object->HasElement(index)) {
            const Result written = copy(std::to_string(index), source.object->GetElement(index));
            if (written.IsAbrupt()) {
              return call.ThrowValue(written.value);
            }
          }
        }
      }
      for (const std::string& key : call.interpreter.OwnKeys(source, true)) {
        // Read through GetProperty, so a getter on the source runs -- assign
        // copies values, not accessors.
        const Result written = copy(key, call.interpreter.GetPropertyValue(source, key));
        if (written.IsAbrupt()) {
          return call.ThrowValue(written.value);
        }
      }
    }
    return target;
  });
  install(object_constructor, "hasOwn", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    if (!target.IsObject()) {
      return Value::Bool(false);
    }
    const PropertyKey key = KeyFrom(Argument(call.arguments, 1));
    // Proxies store `#target`/`#handler` as own hidden slots; HasOwn on the
    // proxy object itself would answer about those, never the wrapped object.
    if (target.object->GetKind() == Object::Kind::Proxy) {
      Value behind;
      if (Object* trap =
              call.interpreter.ProxyTrap(target, "getOwnPropertyDescriptor", behind)) {
        const Result described = call.interpreter.CallFunction(
            Value::Obj(trap), Value::Undefined(), {behind, KeyValue(key)});
        if (described.IsAbrupt()) {
          return call.ThrowValue(described.value);
        }
        return Value::Bool(described.value.IsObject());
      }
      return Value::Bool(behind.IsObject() && behind.object->HasOwn(key));
    }
    return Value::Bool(target.object->HasOwn(key));
  });
  install(object_constructor, "getPrototypeOf", [](NativeCall& call) {
    // Through the Proxy trap (and the target when none is defined). Reading
    // `proxy->Prototype()` is always null — AllocateObject never sets one —
    // which made Lit's `Object.getPrototypeOf(o) === Object.prototype` false
    // for every proxied plain object (U3D / Error "ad").
    Value current = Argument(call.arguments, 0);
    for (int depth = 0; depth < 32; ++depth) {
      if (!current.IsObject()) {
        return Value::Null();
      }
      if (current.object->GetKind() == Object::Kind::Proxy) {
        Value behind;
        if (Object* trap =
                call.interpreter.ProxyTrap(current, "getPrototypeOf", behind)) {
          const Result asked =
              call.interpreter.CallFunction(Value::Obj(trap), Value::Undefined(), {behind});
          if (asked.IsAbrupt()) {
            return call.ThrowValue(asked.value);
          }
          if (asked.value.IsNull() || asked.value.IsObject()) {
            return asked.value;
          }
          return call.Throw("TypeError", "getPrototypeOf trap must return an object or null");
        }
        current = behind;
        continue;
      }
      if (current.object->Prototype() == nullptr) {
        return Value::Null();
      }
      return Value::Obj(current.object->Prototype());
    }
    return Value::Null();
  });
  install(object_constructor, "setPrototypeOf", [](NativeCall& call) {
    Value current = Argument(call.arguments, 0);
    const Value prototype = Argument(call.arguments, 1);
    if (!current.IsObject()) {
      return current;
    }
    if (current.object->GetKind() == Object::Kind::Proxy) {
      Value behind;
      if (Object* trap =
              call.interpreter.ProxyTrap(current, "setPrototypeOf", behind)) {
        const Result asked = call.interpreter.CallFunction(
            Value::Obj(trap), Value::Undefined(), {behind, prototype});
        if (asked.IsAbrupt()) {
          return call.ThrowValue(asked.value);
        }
        return current;
      }
      if (behind.IsObject()) {
        behind.object->SetPrototype(prototype.IsObject() ? prototype.object : nullptr);
      }
      return current;
    }
    current.object->SetPrototype(prototype.IsObject() ? prototype.object : nullptr);
    return current;
  });
  install(object_constructor, "freeze", [](NativeCall& call) {
    // Real, not a no-op. A freeze that reported success and changed nothing
    // would be worse than not having one: a page uses it to protect state it
    // then assumes is unchanged.
    const Value target = Argument(call.arguments, 0);
    if (target.IsObject()) {
      target.object->Freeze();
    }
    return target;
  });
  install(object_constructor, "isFrozen", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    // A primitive is frozen by definition, which is what the spec says and
    // what makes `Object.isFrozen(1)` true.
    return Value::Bool(!target.IsObject() || target.object->IsFrozen());
  });
  // The two weaker levels. Nested rather than independent: sealing is
  // preventing extensions plus refusing deletes, and freezing is sealing plus
  // refusing writes.
  install(object_constructor, "seal", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    if (target.IsObject()) {
      target.object->Restrict(Object::Integrity::Sealed);
    }
    return target;
  });
  install(object_constructor, "isSealed", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    return Value::Bool(!target.IsObject() || target.object->IsSealed());
  });
  install(object_constructor, "preventExtensions", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    if (target.IsObject()) {
      target.object->Restrict(Object::Integrity::NonExtensible);
    }
    return target;
  });
  install(object_constructor, "isExtensible", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    return Value::Bool(target.IsObject() && target.object->IsExtensible());
  });
  // SameValueZero's stricter sibling: NaN is itself and the two zeros are not
  // each other. Neither `===` nor `==` answers both that way, which is the
  // only reason this function exists.
  install(object_constructor, "is", [](NativeCall& call) {
    const Value a = Argument(call.arguments, 0);
    const Value b = Argument(call.arguments, 1);
    if (a.IsNumber() && b.IsNumber()) {
      if (std::isnan(a.number) && std::isnan(b.number)) {
        return Value::Bool(true);
      }
      if (a.number == 0.0 && b.number == 0.0) {
        return Value::Bool(std::signbit(a.number) == std::signbit(b.number));
      }
    }
    return Value::Bool(StrictEquals(a, b));
  });
  install(object_constructor, "getOwnPropertySymbols", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    std::vector<Value> symbols;
    if (target.IsObject()) {
      for (const PropertyKey& key : target.object->SymbolKeys()) {
        symbols.push_back(KeyValue(key));
      }
    }
    return call.interpreter.NewArrayValue(std::move(symbols));
  });
  install(object_constructor, "getOwnPropertyDescriptors", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    const Value out = call.interpreter.NewObjectValue();
    if (!target.IsObject() || !out.IsObject()) {
      return out;
    }
    // Through getOwnPropertyDescriptor rather than around it, so the two
    // cannot disagree about what a descriptor is.
    const Value one = call.interpreter.GetPropertyValue(call.self, "getOwnPropertyDescriptor");
    for (const std::string& key : target.object->Keys()) {
      const Result described =
          call.interpreter.CallFunction(one, call.self, {target, Value::String(key)});
      if (described.IsAbrupt()) {
        return call.ThrowValue(described.value);
      }
      out.object->Set(key, described.value);
    }
    return out;
  });
  install(object_constructor, "groupBy", [](NativeCall& call) {
    const Value items = Argument(call.arguments, 0);
    const Value callback = Argument(call.arguments, 1);
    const Value out = call.interpreter.NewObjectValue();
    if (!out.IsObject()) {
      return out;
    }
    // A null prototype, which is what makes the result safe to index with a
    // key a page supplied: `groups['toString']` must be the group and not
    // Object.prototype's method.
    out.object->SetPrototype(nullptr);
    std::vector<Value> collected;
    const Result gathered = call.interpreter.CollectIterable(items, collected);
    if (gathered.IsAbrupt()) {
      return call.ThrowValue(gathered.value);
    }
    for (std::size_t i = 0; i < collected.size(); ++i) {
      const Result named = call.interpreter.CallFunction(
          callback, Value::Undefined(), {collected[i], Value::Number(static_cast<double>(i))});
      if (named.IsAbrupt()) {
        return call.ThrowValue(named.value);
      }
      PropertyKey key;
      const Result converted = call.interpreter.ToKeyOf(named.value, key);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
      const Value* existing = out.object->GetOwn(key);
      if (existing != nullptr && existing->IsObject()) {
        existing->object->PushElement(collected[i]);
        continue;
      }
      out.object->Set(key, call.interpreter.NewArrayValue({collected[i]}));
    }
    return out;
  });
  // `Object.prototype` had nothing on it, so `({}).hasOwnProperty` was
  // undefined -- and that method is how a great deal of code asks whether a
  // key is its own rather than inherited.
  object_constructor->Set("prototype", Value::Obj(intrinsics().object_prototype));
  intrinsics().object_prototype->SetHidden("constructor", Value::Obj(object_constructor));
  install(intrinsics().object_prototype, "hasOwnProperty", [](NativeCall& call) {
    return Value::Bool(call.self.IsObject() &&
                       call.self.object->HasOwn(KeyFrom(Argument(call.arguments, 0))));
  });
  install(intrinsics().object_prototype, "isPrototypeOf", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    if (!value.IsObject() || !call.self.IsObject()) {
      return Value::Bool(false);
    }
    // Bounded, because a prototype cycle is buildable from a page.
    const Object* walk = value.object->Prototype();
    for (int depth = 0; walk != nullptr && depth < 1000; ++depth) {
      if (walk == call.self.object) {
        return Value::Bool(true);
      }
      walk = walk->Prototype();
    }
    return Value::Bool(false);
  });
  install(intrinsics().object_prototype, "propertyIsEnumerable", [](NativeCall& call) {
    // Every own property is enumerable here: the object model has no
    // attributes to say otherwise, so this is `hasOwnProperty` under another
    // name rather than a second answer.
    return Value::Bool(call.self.IsObject() &&
                       call.self.object->HasOwn(KeyFrom(Argument(call.arguments, 0))));
  });
  install(intrinsics().object_prototype, "valueOf", [](NativeCall& call) { return call.self; });
  install(intrinsics().object_prototype, "toLocaleString", [](NativeCall& call) {
    // Whatever `toString` says. No locale data here, and inventing one is a
    // lie a page cannot detect.
    const Value method = call.interpreter.GetPropertyValue(call.self, "toString");
    const Result text = call.interpreter.CallFunction(method, call.self, {});
    return text.IsAbrupt() ? call.ThrowValue(text.value) : text.value;
  });
  // `__proto__`, as the accessor it is rather than as a property. Legacy, and
  // the web depends on it: it predates Object.getPrototypeOf and a great deal
  // of code still reaches for it.
  if (Object* proto_get = native("__proto__", [](NativeCall& call) {
        if (!call.self.IsObject()) {
          return Value::Undefined();
        }
        Object* prototype = call.self.object->Prototype();
        return prototype == nullptr ? Value::Null() : Value::Obj(prototype);
      })) {
    Object* proto_set = native("__proto__", [](NativeCall& call) {
      const Value prototype = Argument(call.arguments, 0);
      if (call.self.IsObject() && (prototype.IsObject() || prototype.IsNull())) {
        call.self.object->SetPrototype(prototype.IsObject() ? prototype.object : nullptr);
      }
      return Value::Undefined();
    });
    intrinsics().object_prototype->DefineAccessor("__proto__", proto_get, proto_set);
  }
  install(intrinsics().object_prototype, "toString", [](NativeCall& call) {
    // The `[object Kind]` form, which is what a page uses to tell an array
    // from a plain object without trusting `instanceof` across realms.
    //
    // `null` and `undefined` are the two the spec answers before looking at
    // anything, and they are *different* answers -- a library that feature-
    // detects by calling this on null gets "[object Null]" and nothing else.
    if (call.self.IsNull()) {
      return Value::String(std::string("[object Null]"));
    }
    if (call.self.IsUndefined()) {
      return Value::String(std::string("[object Undefined]"));
    }
    if (!call.self.IsObject()) {
      switch (call.self.type) {
        case ValueType::Boolean: return Value::String(std::string("[object Boolean]"));
        case ValueType::Number: return Value::String(std::string("[object Number]"));
        case ValueType::String: return Value::String(std::string("[object String]"));
        case ValueType::Symbol: return Value::String(std::string("[object Symbol]"));
        default: return Value::String(std::string("[object Object]"));
      }
    }
    // `Symbol.toStringTag` overrides the kind, which is how a class names
    // itself to a feature test it cannot otherwise reach.
    if (Object* tag = call.interpreter.SymbolToStringTag()) {
      const Value named =
          call.interpreter.GetPropertyValue(call.self, PropertyKey::Symbol(tag));
      if (named.IsString()) {
        return Value::String("[object " + named.AsString() + "]");
      }
    }
    // Through the proxy: `Object.prototype.toString.call(new Proxy([], {}))`
    // is "[object Array]", which is the cross-realm array test a page makes
    // when `instanceof` cannot be trusted.
    switch (call.self.object->TargetKind()) {
      case Object::Kind::Array: return Value::String(std::string("[object Array]"));
      case Object::Kind::Function:
      case Object::Kind::Native: return Value::String(std::string("[object Function]"));
      case Object::Kind::Error: return Value::String(std::string("[object Error]"));
      case Object::Kind::RegExp: return Value::String(std::string("[object RegExp]"));
      case Object::Kind::Symbol: return Value::String(std::string("[object Symbol]"));
      case Object::Kind::ArrayBuffer: return Value::String(std::string("[object ArrayBuffer]"));
      case Object::Kind::DataView: return Value::String(std::string("[object DataView]"));
      // A typed array reports its element type, which is what a page uses to
      // tell a Uint8Array from a Float64Array without trusting `constructor`.
      case Object::Kind::TypedArray: {
        const Value* name =
            call.self.object->Prototype() == nullptr
                ? nullptr
                : call.self.object->Prototype()->Get("constructor");
        const Value* text =
            name != nullptr && name->IsObject() ? name->object->GetOwn("name") : nullptr;
        return Value::String("[object " +
                             (text == nullptr ? std::string("TypedArray") : ToString(*text)) +
                             "]");
      }
      // Only a proxy whose target is itself a proxy, which is a cycle a page
      // built rather than anything with a kind to report.
      case Object::Kind::Proxy: return Value::String(std::string("[object Object]"));
      case Object::Kind::HTMLAllCollection:
        return Value::String(std::string("[object HTMLAllCollection]"));
      case Object::Kind::Plain: break;
    }
    return Value::String(std::string("[object Object]"));
  });
  // --- Proxy ----------------------------------------------------------------
  // A wrapper whose every property operation goes to a handler first. The
  // check for it sits on the hot path of GetProperty and SetProperty, which is
  // why the proxy is a *kind* rather than a marker property: the kind byte is
  // already being read there.
  //
  // A trap the handler does not define falls through to the target rather than
  // failing, which is what makes `new Proxy(o, {})` behave exactly like `o`.
  realm_->global_scope->Declare(
      "Proxy",
      NewNativeValue(
          "Proxy",
          [](NativeCall& call) {
            const Value target = Argument(call.arguments, 0);
            const Value handler = Argument(call.arguments, 1);
            if (!target.IsObject() || !handler.IsObject()) {
              return call.Throw("TypeError", "Proxy requires a target and a handler");
            }
            Object* proxy = call.interpreter.GetHeap().AllocateObject(Object::Kind::Proxy);
            if (proxy == nullptr) {
              return call.Throw("RangeError", "out of memory");
            }
            // Own properties, so the collector marks both without the proxy
            // needing a slot of its own.
            proxy->SetHidden("#target", target);
            proxy->SetHidden("#handler", handler);
            return Value::Obj(proxy);
          }),
      false);

  // --- Reflect --------------------------------------------------------------
  // The same operations the language performs implicitly, as ordinary
  // functions. Almost all of it is a thin name over something that already
  // exists here -- which is the point: a framework calls `Reflect.get` where
  // it would otherwise write `obj[key]`, so that the two cannot diverge when a
  // Proxy is in the way.
  Object* reflect = NewObject();
  if (reflect != nullptr) {
    install(reflect, "get", [](NativeCall& call) {
      const Value target = Argument(call.arguments, 0);
      if (!target.IsObject()) {
        return call.Throw("TypeError", "Reflect.get called on a non-object");
      }
      return call.interpreter.GetPropertyValue(target, KeyFrom(Argument(call.arguments, 1)));
    });
    install(reflect, "set", [](NativeCall& call) {
      const Value target = Argument(call.arguments, 0);
      if (!target.IsObject()) {
        return call.Throw("TypeError", "Reflect.set called on a non-object");
      }
      // Through SetProperty rather than `object->Set`, for the same reason
      // Object.assign is: a setter on the prototype (Polymer property effects,
      // reflected DOM attributes) must run. `object->Set` stores a data slot
      // and clears accessors, so `Reflect.set(el, 'items', arr)` left
      // `dom-repeat.items` null while ordinary assignment would have worked.
      const Result written =
          call.interpreter.SetProperty(target, KeyFrom(Argument(call.arguments, 1)),
                                       Argument(call.arguments, 2));
      if (written.IsAbrupt()) {
        return call.ThrowValue(written.value);
      }
      return Value::Bool(true);
    });
    install(reflect, "has", [](NativeCall& call) {
      const Value target = Argument(call.arguments, 0);
      if (!target.IsObject()) {
        return call.Throw("TypeError", "Reflect.has called on a non-object");
      }
      // `in`, which walks the prototype chain -- unlike hasOwnProperty, which
      // is the distinction this pair exists to keep straight.
      return Value::Bool(target.object->GetProperty(KeyFrom(Argument(call.arguments, 1))) !=
                         nullptr);
    });
    install(reflect, "deleteProperty", [](NativeCall& call) {
      const Value target = Argument(call.arguments, 0);
      if (!target.IsObject()) {
        return call.Throw("TypeError", "Reflect.deleteProperty called on a non-object");
      }
      return Value::Bool(
          call.interpreter.DeleteProperty(target, KeyFrom(Argument(call.arguments, 1))));
    });
    install(reflect, "apply", [](NativeCall& call) {
      const Value target = Argument(call.arguments, 0);
      if (!target.IsObject() || !target.object->IsCallable()) {
        return call.Throw("TypeError", "Reflect.apply requires a function");
      }
      std::vector<Value> arguments;
      const Value list = Argument(call.arguments, 2);
      if (!list.IsNullish()) {
        const Result collected = call.interpreter.CollectIterable(list, arguments);
        if (collected.IsAbrupt()) {
          return call.ThrowValue(collected.value);
        }
      }
      const Result applied =
          call.interpreter.CallFunction(target, Argument(call.arguments, 1), arguments);
      if (applied.IsAbrupt()) {
        return call.ThrowValue(applied.value);
      }
      return applied.value;
    });
    install(reflect, "construct", [](NativeCall& call) {
      const Value target = Argument(call.arguments, 0);
      std::vector<Value> arguments;
      const Value list = Argument(call.arguments, 1);
      if (!list.IsNullish()) {
        const Result collected = call.interpreter.CollectIterable(list, arguments);
        if (collected.IsAbrupt()) {
          return call.ThrowValue(collected.value);
        }
      }
      const Result made = call.interpreter.ConstructValue(target, arguments);
      return made.IsAbrupt() ? call.ThrowValue(made.value) : made.value;
    });
    // The rest are the same answers as their Object counterparts, read off the
    // constructor rather than written twice.
    for (const char* name : {"getPrototypeOf", "setPrototypeOf", "defineProperty", "ownKeys",
                             "getOwnPropertyDescriptor", "isExtensible", "preventExtensions"}) {
      const char* source = std::string_view(name) == "ownKeys" ? "getOwnPropertyNames" : name;
      if (const Value* existing = object_constructor->GetOwn(source)) {
        reflect->Set(name, *existing);
      }
    }
    realm_->global_scope->Declare("Reflect", Value::Obj(reflect), false);
  }

  realm_->global_scope->Declare("Object", Value::Obj(object_constructor), false);

  // Array.prototype and the Array constructor, in their own translation unit
  // for the reason String.prototype has one: it is the second-largest group of
  // builtins and this file is where everything else lands.
  InstallArrayPrototype();

  // --- Errors ---------------------------------------------------------------
  // `Error` and the seven NativeError kinds, in their own translation unit:
  // they are one feature with a prototype chain of their own -- the
  // constructors inherit from `Error`, not from `Function.prototype` -- and
  // this file is where everything without a home lands.
  InstallErrors();

  // --- String and number conversions ---------------------------------------
  Object* string_constructor = native("String", [](NativeCall& call) {
    // `String()` with no argument is the empty string, not "undefined". Every
    // other value goes through the ordinary conversion -- the interpreter's
    // one, which runs a `toString` a page wrote. The pure ToString would
    // answer "[object Object]" for every object, which is what made
    // `String(new Date())` useless.
    std::string text;
    if (!call.arguments.empty()) {
      // A symbol is the one value `String()` may convert and `${}` may not:
      // the explicit call is allowed and the implicit one is a TypeError.
      // `new String(symbol)` goes through ToString and throws, which is the
      // spec; only the non-construct call gets the descriptive string.
      if (call.arguments[0].IsSymbol() && ConstructionTarget(call) == nullptr) {
        return Value::String(ToString(call.arguments[0]));
      }
      const Result converted = call.interpreter.ToStringOf(call.arguments[0], text);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
    }
    if (Object* target = ConstructionTarget(call)) {
      // `new` keeps the instance even when the native returns a primitive, so
      // the characters have to live on that object. Blob parts and
      // String.prototype methods both read this slot.
      target->SetHidden("#string", Value::String(text));
      return Value::Obj(target);
    }
    return Value::String(std::move(text));
  });
  InstallStringPrototype(string_constructor);
  // After it, because the pattern-taking String methods are installed on the
  // same prototype and would otherwise be overwritten by it.
  InstallRegExpPrototype();
  // After both prototypes exist: the iteration hooks are installed on them.
  InstallIteration();
  InstallCollections();
  InstallPromises();
  // After InstallIteration: %GeneratorPrototype% carries a `Symbol.iterator`,
  // and the cell it is keyed on is made there.
  InstallGeneratorPrototype();
  realm_->global_scope->Declare("String", Value::Obj(string_constructor), false);
  realm_->global_scope->Declare(
      "Number", Value::Obj(native("Number", [](NativeCall& call) {
        // `Number()` with no argument is 0, not NaN. Through the interpreter's
        // conversion for the reason `String` is: an object with a `valueOf`
        // has to run it.
        if (call.arguments.empty()) {
          return Value::Number(0.0);
        }
        double number = 0;
        const Result converted = call.interpreter.ToNumberOf(call.arguments[0], number);
        return converted.IsAbrupt() ? call.ThrowValue(converted.value)
                                    : Value::Number(number);
      })),
      false);
  Object* boolean_constructor = native("Boolean", [](NativeCall& call) {
    return Value::Bool(ToBoolean(Argument(call.arguments, 0)));
  });
  if (boolean_constructor != nullptr) {
    // Two methods, and both are reached by conversion far more often than by
    // a page writing them: `true.toString()` is what ToPrimitive calls, and
    // without it a boolean in a string context is a TypeError.
    intrinsics().boolean_prototype = NewObject();
    if (intrinsics().boolean_prototype != nullptr) {
      intrinsics().boolean_prototype->SetPrototype(intrinsics().object_prototype);
      install(intrinsics().boolean_prototype, "toString", [](NativeCall& call) {
        return Value::String(std::string(ToBoolean(call.self) ? "true" : "false"));
      });
      install(intrinsics().boolean_prototype, "valueOf",
              [](NativeCall& call) { return Value::Bool(ToBoolean(call.self)); });
      boolean_constructor->Set("prototype", Value::Obj(intrinsics().boolean_prototype));
      intrinsics().boolean_prototype->SetHidden("constructor", Value::Obj(boolean_constructor));
    }
    realm_->global_scope->Declare("Boolean", Value::Obj(boolean_constructor), false);
  }
  realm_->global_scope->Declare(
      "parseInt", Value::Obj(native("parseInt", [](NativeCall& call) {
        // Unlike Number(), parseInt stops at the first character it cannot use
        // -- which is why parseInt('12px') is 12 and Number('12px') is NaN.
        const std::string text = ToString(Argument(call.arguments, 0));
        const int radix =
            call.arguments.size() > 1 ? ToInt32(ToNumber(call.arguments[1])) : 0;
        return Value::Number(ParseIntPrefix(text, radix));
      })),
      false);
  realm_->global_scope->Declare(
      "parseFloat", Value::Obj(native("parseFloat", [](NativeCall& call) {
        const std::string text = ToString(Argument(call.arguments, 0));
        return Value::Number(ParseFloatPrefix(text));
      })),
      false);
  realm_->global_scope->Declare(
      "isNaN", Value::Obj(native("isNaN", [](NativeCall& call) {
        return Value::Bool(std::isnan(ToNumber(Argument(call.arguments, 0))));
      })),
      false);
  // The global one converts, unlike `Number.isFinite`: `isFinite('1')` is true
  // and `Number.isFinite('1')` is false, because the string is not a number
  // rather than being one that is infinite.
  realm_->global_scope->Declare(
      "isFinite", Value::Obj(native("isFinite", [](NativeCall& call) {
        return Value::Bool(std::isfinite(ToNumber(Argument(call.arguments, 0))));
      })),
      false);

  // Last: it reads `Number`, `parseInt` and `parseFloat` back out of the
  // global scope, so every one of them has to be declared first.
  install_numbers();

  // `Symbol.species`, which library code reads to find out what a derived
  // method should construct. Every built-in answers with itself, which is the
  // default the spec gives them and the answer a subclass overrides.
  if (Object* species_cell = nullptr; true) {
    Value* symbol_object = realm_->global_scope->Lookup("Symbol");
    if (symbol_object != nullptr && symbol_object->IsObject()) {
      const Value* cell = symbol_object->object->GetOwn("species");
      species_cell = cell != nullptr && cell->IsSymbol() ? cell->object : nullptr;
    }
    if (species_cell != nullptr) {
      for (const char* name : {"Array", "Map", "Set", "RegExp", "Promise", "ArrayBuffer"}) {
        Value* declared = realm_->global_scope->Lookup(name);
        if (declared != nullptr && declared->IsObject()) {
          declared->object->SetHidden(PropertyKey::Symbol(species_cell), *declared);
        }
      }
    }
  }

  // `escape` and `unescape`. Annex B, superseded by the URI functions in 1999,
  // and still called by code that has not been touched since. Refusing them is
  // a ReferenceError in the middle of a page that would otherwise work.
  for (const bool encoding : {true, false}) {
    const char* name = encoding ? "escape" : "unescape";
    realm_->global_scope->Declare(
        name, NewNativeValue(name, [encoding](NativeCall& call) {
          std::string text;
          const Result converted =
              call.interpreter.ToStringOf(Argument(call.arguments, 0), text);
          if (converted.IsAbrupt()) {
            return call.ThrowValue(converted.value);
          }
          std::string out;
          if (!encoding) {
            // Through AppendCodeUnit, which holds a high surrogate until its
            // partner arrives: `%uD83D%uDE00` is one character and writing the
            // halves separately would make it two broken ones.
            std::uint32_t pending = 0;
            for (std::size_t at = 0; at < text.size();) {
              // `%uXXXX` first: it is the longer form and shares its prefix.
              if (text[at] == '%' && at + 5 < text.size() && text[at + 1] == 'u') {
                int value = 0;
                bool ok = true;
                for (int i = 0; i < 4; ++i) {
                  const int digit = util::HexDigit(text[at + 2 + static_cast<std::size_t>(i)]);
                  ok = ok && digit >= 0;
                  value = value * 16 + (digit < 0 ? 0 : digit);
                }
                if (ok) {
                  AppendCodeUnit(out, static_cast<std::uint16_t>(value), pending);
                  at += 6;
                  continue;
                }
              }
              if (text[at] == '%' && at + 2 < text.size()) {
                const int high = util::HexDigit(text[at + 1]);
                const int low = util::HexDigit(text[at + 2]);
                if (high >= 0 && low >= 0) {
                  AppendCodeUnit(out, static_cast<std::uint16_t>(high * 16 + low), pending);
                  at += 3;
                  continue;
                }
              }
              FlushCodeUnit(out, pending);
              out.push_back(text[at++]);
            }
            FlushCodeUnit(out, pending);
            return Value::String(std::move(out));
          }
          // The unreserved set is the one Annex B names, and it is not the
          // URI one: `@*_+-./` are left alone and everything else is escaped.
          for (std::size_t at = 0; at < text.size();) {
            std::uint32_t code = 0;
            std::size_t next = at;
            if (!util::DecodeUtf8(text, next, code)) {
              code = static_cast<unsigned char>(text[at]);
              next = at + 1;
            }
            const bool plain =
                (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') ||
                (code >= '0' && code <= '9') || code == '@' || code == '*' ||
                code == '_' || code == '+' || code == '-' || code == '.' || code == '/';
            char buffer[16];
            if (plain) {
              out.push_back(static_cast<char>(code));
            } else if (code < 256) {
              std::snprintf(buffer, sizeof(buffer), "%%%02X", code);
              out += buffer;
            } else if (code < 0x10000u) {
              std::snprintf(buffer, sizeof(buffer), "%%u%04X", code);
              out += buffer;
            } else {
              // `%uXXXX` names a *code unit*, so an astral character is two of
              // them -- the surrogate pair, which is what `unescape` puts back
              // together.
              const std::uint32_t offset = code - 0x10000u;
              std::snprintf(buffer, sizeof(buffer), "%%u%04X%%u%04X",
                            0xD800u + (offset >> 10), 0xDC00u + (offset & 0x3FFu));
              out += buffer;
            }
            at = next;
          }
          return Value::String(std::move(out));
        }),
        false);
  }

  // --- Making the built-ins invisible to enumeration -------------------------
  //
  // Every method above was installed with an ordinary assignment, which leaves
  // it enumerable -- and in the language none of them is. That did not show
  // while `for...in` walked only own properties; now that it walks the
  // prototype chain, `for (const k in [])` would report every array method.
  //
  // One sweep rather than two hundred careful install sites, because a site
  // that forgot would be invisible until a page enumerated the one object it
  // touched. Names rather than a saved list of pointers, so that a constructor
  // added later is covered by adding it here and nowhere else.
  //
  // `console` is deliberately not here: it is a Web IDL namespace, and those
  // operations are enumerable. Hiding them made every idlharness operation
  // check fail while `for...in` on Array.prototype stayed correct.
  static constexpr const char* kBuiltinNames[] = {
      "Object",   "Array",     "String",     "Number",       "Boolean",
      "Function", "Symbol",    "Math",       "JSON",         "Date",
      "RegExp",   "Map",       "Set",        "WeakMap",      "WeakSet",
      "WeakRef",  "Promise",   "Proxy",      "Reflect",
      "Error",    "TypeError", "RangeError", "SyntaxError",  "ReferenceError",
      "EvalError", "URIError", "AggregateError", "ArrayBuffer", "DataView",
      "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
      "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
      "FinalizationRegistry",
  };
  for (const char* name : kBuiltinNames) {
    Value* declared = realm_->global_scope->Lookup(name);
    if (declared == nullptr || !declared->IsObject()) {
      continue;
    }
    declared->object->HideProperties();
    if (const Value* prototype = declared->object->GetOwn("prototype")) {
      if (prototype->IsObject()) {
        prototype->object->HideProperties();
      }
    }
  }
  // The prototypes nothing names: a generator's, an async generator's, and the
  // one the nine typed arrays share.
  for (Object* prototype :
       {intrinsics().object_prototype, intrinsics().array_prototype,
        intrinsics().function_prototype, intrinsics().string_prototype,
        intrinsics().number_prototype, intrinsics().boolean_prototype,
        intrinsics().regexp_prototype, intrinsics().promise_prototype,
        intrinsics().generator_prototype, intrinsics().async_generator_prototype,
        intrinsics().typed_array_prototype, intrinsics().array_buffer_prototype}) {
    if (prototype != nullptr) {
      prototype->HideProperties();
    }
  }
  // Console is installed before well-known symbols exist. The namespace
  // object's @@toStringTag has to wait until `Symbol.toStringTag` is a real
  // symbol, or `Object.prototype.toString.call(console)` stays `[object Object]`.
  if (Value* console_value = realm_->global_scope->Lookup("console")) {
    if (console_value->IsObject()) {
      if (Object* tag = SymbolToStringTag()) {
        Object::Property tag_property;
        tag_property.value = Value::String("console");
        tag_property.enumerable = false;
        tag_property.writable = false;
        tag_property.configurable = true;
        console_value->object->Define(PropertyKey::Symbol(tag), std::move(tag_property));
      }
    }
  }
}

}  // namespace microbrowser::js

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "util/Parse.h"

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
  object->SetPrototype(well_known_.function_prototype);
  object->MakeNative(std::move(function));
  object->Set("name", Value::String(name));
  return object;
}

void Interpreter::InstallNative(Object* target, const char* name, NativeFunction function) {
  target->Set(name, Value::Obj(NewNative(name, std::move(function))));
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
       {well_known_.array_prototype, well_known_.function_prototype,
        well_known_.string_prototype, well_known_.regexp_prototype,
        well_known_.promise_prototype}) {
    if (prototype != nullptr) {
      prototype->SetPrototype(well_known_.object_prototype);
    }
  }

  // First, because every native installed after this one inherits from it.
  InstallFunctionPrototype();

  global_scope_->Declare("globalThis", Value::Obj(global_), false);
  global_scope_->Declare("undefined", Value::Undefined(), true);
  global_scope_->Declare("NaN", Value::Number(std::nan("")), true);
  global_scope_->Declare("Infinity", Value::Number(HUGE_VAL), true);

  // --- console --------------------------------------------------------------
  // Collected rather than printed. A page must not be able to write to the
  // terminal the browser was started from, and a test needs to read what was
  // logged.
  Object* console = NewObject();
  const auto log = [this](NativeCall& call) {
    std::string line;
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      if (i != 0) {
        line.push_back(' ');
      }
      line += ToString(call.arguments[i]);
    }
    console_.push_back(std::move(line));
    return Value::Undefined();
  };
  install(console, "log", log);
  install(console, "warn", log);
  install(console, "error", log);
  global_scope_->Declare("console", Value::Obj(console), false);

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
  global_scope_->Declare("Math", Value::Obj(math), false);
  // Deferred: it reads `Number`, `parseInt` and `parseFloat` back out of the
  // global scope, and none of them is declared yet at this point.
  const auto install_numbers = [this, math] { InstallNumbers(math); };

  // --- JSON -----------------------------------------------------------------
  Object* json = NewObject();
  install(json, "stringify", [](NativeCall& call) {
    // Enough of JSON.stringify to serialize plain data. Cycles are refused by
    // depth rather than by tracking visited objects, which is the cheaper of
    // the two and gives the same answer for anything that is not a cycle.
    struct Writer {
      std::string out;
      bool Write(const Value& value, int depth) {
        if (depth > 64) {
          return false;
        }
        switch (value.type) {
          case ValueType::Undefined:
            out += "null";  // undefined in an array serializes as null
            return true;
          case ValueType::Null:
            out += "null";
            return true;
          case ValueType::Symbol:
            // JSON has no symbol. The spec drops one wherever `undefined`
            // would be dropped, and this writer's callers are the array and
            // object paths that already handle that.
            out += "null";
            return true;
          case ValueType::Boolean:
            out += value.boolean ? "true" : "false";
            return true;
          case ValueType::Number:
            out += std::isfinite(value.number) ? NumberToString(value.number) : "null";
            return true;
          case ValueType::String:
            WriteString(value.AsString());
            return true;
          case ValueType::Object:
            break;
        }
        if (value.object->IsCallable()) {
          out += "null";
          return true;
        }
        if (value.object->GetKind() == Object::Kind::Array) {
          out.push_back('[');
          bool first = true;
          for (std::size_t i = 0; i < value.object->ElementCount(); ++i) {
            if (!first) {
              out.push_back(',');
            }
            first = false;
            if (!Write(value.object->GetElement(i), depth + 1)) {
              return false;
            }
          }
          out.push_back(']');
          return true;
        }
        out.push_back('{');
        bool first = true;
        for (const std::string& key : value.object->Keys()) {
          const Value* property = value.object->GetOwn(key);
          if (property == nullptr || property->IsUndefined()) {
            continue;  // an undefined property is omitted, not written as null
          }
          if (!first) {
            out.push_back(',');
          }
          first = false;
          WriteString(key);
          out.push_back(':');
          if (!Write(*property, depth + 1)) {
            return false;
          }
        }
        out.push_back('}');
        return true;
      }

      void WriteString(const std::string& text) {
        out.push_back('"');
        for (const char c : text) {
          switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
              if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
              } else {
                out.push_back(c);
              }
              break;
          }
        }
        out.push_back('"');
      }
    };

    Writer writer;
    if (!writer.Write(Argument(call.arguments, 0), 0)) {
      return Value::Undefined();
    }
    return Value::String(std::move(writer.out));
  });
  InstallJsonAndUri(json);
  global_scope_->Declare("JSON", Value::Obj(json), false);

  // --- Object ---------------------------------------------------------------
  Object* object_constructor = native("Object", [](NativeCall& call) {
    return Argument(call.arguments, 0);
  });
  install(object_constructor, "create", [](NativeCall& call) {
    const Value prototype = Argument(call.arguments, 0);
    if (!prototype.IsObject() && !prototype.IsNull()) {
      return call.Throw("TypeError", "Object.create prototype must be an object or null");
    }
    Object* object = call.interpreter.NewObject();
    if (object == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    object->SetPrototype(prototype.IsObject() ? prototype.object : nullptr);
    return Value::Obj(object);
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
      for (const std::string& key : target.object->Keys()) {
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
      for (const std::string& key : target.object->Keys()) {
        values.push_back(call.interpreter.GetProperty(target, key));
      }
    }
    return call.interpreter.NewArrayValue(std::move(values));
  });
  install(object_constructor, "defineProperty", [](NativeCall& call) {
    // The workhorse of every transpiled module and every framework's own
    // property machinery. Only the parts that mean something here are read:
    // `value`, `get` and `set`. `writable`, `enumerable` and `configurable`
    // have nowhere to be stored -- a property has no attributes in this object
    // model -- so they are accepted and ignored rather than refused, which is
    // the behaviour a page survives.
    const Value target = Argument(call.arguments, 0);
    if (!target.IsObject()) {
      return call.Throw("TypeError", "Object.defineProperty called on a non-object");
    }
    const Value descriptor = Argument(call.arguments, 2);
    if (!descriptor.IsObject()) {
      return call.Throw("TypeError", "a property descriptor must be an object");
    }
    const PropertyKey key = KeyFrom(Argument(call.arguments, 1));
    const Value* getter = descriptor.object->GetOwn("get");
    const Value* setter = descriptor.object->GetOwn("set");
    if (getter != nullptr || setter != nullptr) {
      target.object->DefineAccessor(
          key, getter != nullptr && getter->IsObject() ? getter->object : nullptr,
          setter != nullptr && setter->IsObject() ? setter->object : nullptr);
      return target;
    }
    const Value* value = descriptor.object->GetOwn("value");
    target.object->Set(key, value == nullptr ? Value::Undefined() : *value);
    return target;
  });
  install(object_constructor, "defineProperties", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    const Value descriptors = Argument(call.arguments, 1);
    if (!target.IsObject() || !descriptors.IsObject()) {
      return call.Throw("TypeError", "Object.defineProperties requires two objects");
    }
    const Value define = call.interpreter.GetPropertyValue(call.self, "defineProperty");
    for (const std::string& key : descriptors.object->Keys()) {
      const Value* descriptor = descriptors.object->GetOwn(key);
      if (descriptor == nullptr) {
        continue;
      }
      // Through defineProperty rather than around it, so the two cannot
      // disagree about what a descriptor means.
      const Result defined = call.interpreter.CallFunction(
          define, call.self, {target, Value::String(key), *descriptor});
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
    const Object::Property* property =
        target.object->GetOwnProperty(KeyFrom(Argument(call.arguments, 1)));
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
      descriptor.object->Set("writable", Value::Bool(!target.object->IsFrozen()));
    }
    descriptor.object->Set("enumerable", Value::Bool(true));
    descriptor.object->Set("configurable", Value::Bool(!target.object->IsFrozen()));
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
      for (const std::string& key : target.object->Keys()) {
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
      for (const std::string& key : target.object->Keys()) {
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
      if (source.object->GetKind() == Object::Kind::Array) {
        for (std::size_t index = 0; index < source.object->ElementCount(); ++index) {
          if (source.object->HasElement(index)) {
            target.object->Set(std::to_string(index), source.object->GetElement(index));
          }
        }
      }
      for (const std::string& key : source.object->Keys()) {
        // Read through GetProperty, so a getter on the source runs -- assign
        // copies values, not accessors.
        target.object->Set(key, call.interpreter.GetPropertyValue(source, key));
      }
    }
    return target;
  });
  install(object_constructor, "hasOwn", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    return Value::Bool(target.IsObject() &&
                       target.object->HasOwn(KeyFrom(Argument(call.arguments, 1))));
  });
  install(object_constructor, "getPrototypeOf", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    if (!target.IsObject() || target.object->Prototype() == nullptr) {
      return Value::Null();
    }
    return Value::Obj(target.object->Prototype());
  });
  install(object_constructor, "setPrototypeOf", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    const Value prototype = Argument(call.arguments, 1);
    if (target.IsObject()) {
      target.object->SetPrototype(prototype.IsObject() ? prototype.object : nullptr);
    }
    return target;
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
  // `Object.prototype` had nothing on it, so `({}).hasOwnProperty` was
  // undefined -- and that method is how a great deal of code asks whether a
  // key is its own rather than inherited.
  object_constructor->Set("prototype", Value::Obj(well_known_.object_prototype));
  well_known_.object_prototype->Set("constructor", Value::Obj(object_constructor));
  install(well_known_.object_prototype, "hasOwnProperty", [](NativeCall& call) {
    return Value::Bool(call.self.IsObject() &&
                       call.self.object->HasOwn(KeyFrom(Argument(call.arguments, 0))));
  });
  install(well_known_.object_prototype, "isPrototypeOf", [](NativeCall& call) {
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
  install(well_known_.object_prototype, "propertyIsEnumerable", [](NativeCall& call) {
    // Every own property is enumerable here: the object model has no
    // attributes to say otherwise, so this is `hasOwnProperty` under another
    // name rather than a second answer.
    return Value::Bool(call.self.IsObject() &&
                       call.self.object->HasOwn(KeyFrom(Argument(call.arguments, 0))));
  });
  install(well_known_.object_prototype, "valueOf", [](NativeCall& call) { return call.self; });
  install(well_known_.object_prototype, "toString", [](NativeCall& call) {
    // The `[object Kind]` form, which is what a page uses to tell an array
    // from a plain object without trusting `instanceof` across realms.
    if (!call.self.IsObject()) {
      return Value::String(std::string("[object ") +
                           (call.self.IsNullish() ? "Undefined" : "Object") + "]");
    }
    switch (call.self.object->GetKind()) {
      case Object::Kind::Array: return Value::String(std::string("[object Array]"));
      case Object::Kind::Function:
      case Object::Kind::Native: return Value::String(std::string("[object Function]"));
      case Object::Kind::Error: return Value::String(std::string("[object Error]"));
      case Object::Kind::RegExp: return Value::String(std::string("[object RegExp]"));
      case Object::Kind::Symbol: return Value::String(std::string("[object Symbol]"));
      case Object::Kind::Plain: break;
    }
    return Value::String(std::string("[object Object]"));
  });
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
      target.object->Set(KeyFrom(Argument(call.arguments, 1)), Argument(call.arguments, 2));
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
      return Value::Bool(target.object->Delete(KeyFrom(Argument(call.arguments, 1))));
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
    // The three that are the same answer as their Object counterparts, read
    // off the constructor rather than written twice.
    for (const char* name : {"getPrototypeOf", "setPrototypeOf", "defineProperty", "ownKeys"}) {
      const char* source = std::string_view(name) == "ownKeys" ? "getOwnPropertyNames" : name;
      if (const Value* existing = object_constructor->GetOwn(source)) {
        reflect->Set(name, *existing);
      }
    }
    global_scope_->Declare("Reflect", Value::Obj(reflect), false);
  }

  global_scope_->Declare("Object", Value::Obj(object_constructor), false);

  // Array.prototype and the Array constructor, in their own translation unit
  // for the reason String.prototype has one: it is the second-largest group of
  // builtins and this file is where everything else lands.
  InstallArrayPrototype();

  // --- Errors ---------------------------------------------------------------
  // `new Error('x')` is how a page raises one, and until now there was no
  // constructor to call: errors existed only as the things the engine threw.
  // Each kind gets its own so that `e instanceof TypeError` has something to
  // be true of, and so a caught error prints as the kind it is.
  Object* error_prototype = NewObject();
  const auto error_kind = [this, error_prototype](const char* name) {
    Object* prototype = name == std::string_view("Error") ? error_prototype : NewObject();
    if (prototype == nullptr) {
      return;
    }
    if (prototype != error_prototype) {
      prototype->SetPrototype(error_prototype);
    }
    prototype->Set("name", Value::String(name));
    Object* constructor = NewNative(name, [](NativeCall& call) {
      // Callable with or without `new`: `Error('x')` and `new Error('x')` are
      // the same thing, which is one of the few places the language says so.
      Object* error = call.interpreter.GetHeap().AllocateObject(Object::Kind::Error);
      if (error == nullptr) {
        return call.Throw("RangeError", "out of memory");
      }
      const Value* prototype_value =
          call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
      if (prototype_value != nullptr && prototype_value->IsObject()) {
        error->SetPrototype(prototype_value->object);
      }
      const Value message = Argument(call.arguments, 0);
      if (!message.IsUndefined()) {
        error->Set("message", Value::String(ToString(message)));
      }
      const Value options = Argument(call.arguments, 1);
      if (options.IsObject()) {
        if (const Value* cause = options.object->GetOwn("cause")) {
          error->Set("cause", *cause);
        }
      }
      return Value::Obj(error);
    });
    if (constructor == nullptr) {
      return;
    }
    constructor->Set("prototype", Value::Obj(prototype));
    prototype->Set("constructor", Value::Obj(constructor));
    global_scope_->Declare(name, Value::Obj(constructor), false);
  };
  if (error_prototype != nullptr) {
    error_prototype->Set("message", Value::String(""));
    install(error_prototype, "toString", [](NativeCall& call) {
      return Value::String(ToString(call.self));
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

  // --- String and number conversions ---------------------------------------
  Object* string_constructor = native("String", [](NativeCall& call) {
    // `String()` with no argument is the empty string, not "undefined". Every
    // other value goes through the ordinary conversion.
    return Value::String(call.arguments.empty() ? std::string()
                                                : ToString(call.arguments[0]));
  });
  InstallStringPrototype(string_constructor);
  // After it, because the pattern-taking String methods are installed on the
  // same prototype and would otherwise be overwritten by it.
  InstallRegExpPrototype();
  // After both prototypes exist: the iteration hooks are installed on them.
  InstallIteration();
  InstallCollections();
  InstallPromises();
  global_scope_->Declare("String", Value::Obj(string_constructor), false);
  global_scope_->Declare(
      "Number", Value::Obj(native("Number", [](NativeCall& call) {
        return Value::Number(ToNumber(Argument(call.arguments, 0)));
      })),
      false);
  global_scope_->Declare(
      "Boolean", Value::Obj(native("Boolean", [](NativeCall& call) {
        return Value::Bool(ToBoolean(Argument(call.arguments, 0)));
      })),
      false);
  global_scope_->Declare(
      "parseInt", Value::Obj(native("parseInt", [](NativeCall& call) {
        // Unlike Number(), parseInt stops at the first character it cannot use
        // -- which is why parseInt('12px') is 12 and Number('12px') is NaN.
        const std::string text = ToString(Argument(call.arguments, 0));
        const int radix =
            call.arguments.size() > 1 ? ToInt32(ToNumber(call.arguments[1])) : 0;
        return Value::Number(ParseIntPrefix(text, radix));
      })),
      false);
  global_scope_->Declare(
      "parseFloat", Value::Obj(native("parseFloat", [](NativeCall& call) {
        const std::string text = ToString(Argument(call.arguments, 0));
        return Value::Number(ParseFloatPrefix(text));
      })),
      false);
  global_scope_->Declare(
      "isNaN", Value::Obj(native("isNaN", [](NativeCall& call) {
        return Value::Bool(std::isnan(ToNumber(Argument(call.arguments, 0))));
      })),
      false);

  // Last: it reads `Number`, `parseInt` and `parseFloat` back out of the
  // global scope, so every one of them has to be declared first.
  install_numbers();
}

}  // namespace microbrowser::js

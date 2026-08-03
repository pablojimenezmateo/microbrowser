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
  object->SetPrototype(function_prototype_);
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
  global_scope_->Declare("Object", Value::Obj(object_constructor), false);

  // --- Array prototype ------------------------------------------------------
  install(array_prototype_, "push", [](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->GetKind() != Object::Kind::Array) {
      return Value::Undefined();
    }
    for (const Value& argument : call.arguments) {
      call.self.object->PushElement(argument);
    }
    return Value::Number(static_cast<double>(call.self.object->ElementCount()));
  });
  install(array_prototype_, "pop", [](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->ElementCount() == 0) {
      return Value::Undefined();
    }
    return call.self.object->PopElement();
  });
  install(array_prototype_, "join", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::String("");
    }
    const Value separator_value = Argument(call.arguments, 0);
    const std::string separator =
        separator_value.IsUndefined() ? "," : ToString(separator_value);
    std::string joined;
    for (std::size_t i = 0; i < call.self.object->ElementCount(); ++i) {
      if (i != 0) {
        joined += separator;
      }
      const Value element = call.self.object->GetElement(i);
      if (!element.IsNullish()) {
        joined += ToString(element);
      }
    }
    return Value::String(std::move(joined));
  });
  install(array_prototype_, "indexOf", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Number(-1);
    }
    const Value needle = Argument(call.arguments, 0);
    for (std::size_t i = 0; i < call.self.object->ElementCount(); ++i) {
      if (call.self.object->HasElement(i) &&
          StrictEquals(call.self.object->GetElement(i), needle)) {
        return Value::Number(static_cast<double>(i));
      }
    }
    return Value::Number(-1);
  });
  install(array_prototype_, "includes", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Bool(false);
    }
    const Value needle = Argument(call.arguments, 0);
    for (std::size_t i = 0; i < call.self.object->ElementCount(); ++i) {
      if (StrictEquals(call.self.object->GetElement(i), needle)) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  install(array_prototype_, "slice", [](NativeCall& call) {
    std::vector<Value> out;
    std::vector<bool> present;
    if (call.self.IsObject()) {
      const double size = static_cast<double>(call.self.object->ElementCount());
      double begin = call.arguments.empty() ? 0.0 : ToNumber(call.arguments[0]);
      double end = call.arguments.size() < 2 ? size : ToNumber(call.arguments[1]);
      // A negative index counts from the end, which is what makes slice(-1)
      // idiomatic.
      begin = begin < 0 ? std::max(0.0, size + begin) : std::min(begin, size);
      end = end < 0 ? std::max(0.0, size + end) : std::min(end, size);
      for (double i = begin; i < end; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        out.push_back(call.self.object->GetElement(index));
        present.push_back(call.self.object->HasElement(index));
      }
    }
    return call.interpreter.NewArrayValue(std::move(out), std::move(present));
  });
  install(array_prototype_, "map", [](NativeCall& call) {
    std::vector<Value> out;
    std::vector<bool> present;
    if (call.self.IsObject()) {
      const Value callback = Argument(call.arguments, 0);
      for (std::size_t i = 0; i < call.self.object->ElementCount(); ++i) {
        if (!call.self.object->HasElement(i)) {
          out.push_back(Value::Undefined());
          present.push_back(false);
          continue;
        }
        const Result mapped = call.interpreter.CallFunction(
            callback, Value::Undefined(),
            {call.self.object->GetElement(i), Value::Number(static_cast<double>(i)), call.self});
        if (mapped.IsAbrupt()) {
          return call.ThrowValue(mapped.value);
        }
        out.push_back(mapped.value);
        present.push_back(true);
      }
    }
    return call.interpreter.NewArrayValue(std::move(out), std::move(present));
  });
  install(array_prototype_, "filter", [](NativeCall& call) {
    std::vector<Value> out;
    if (call.self.IsObject()) {
      const Value callback = Argument(call.arguments, 0);
      for (std::size_t i = 0; i < call.self.object->ElementCount(); ++i) {
        if (!call.self.object->HasElement(i)) {
          continue;
        }
        const Value element = call.self.object->GetElement(i);
        const Result kept = call.interpreter.CallFunction(
            callback, Value::Undefined(), {element, Value::Number(static_cast<double>(i)),
                                           call.self});
        if (kept.IsAbrupt()) {
          return call.ThrowValue(kept.value);
        }
        if (ToBoolean(kept.value)) {
          out.push_back(element);
        }
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });
  install(array_prototype_, "reduce", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    const Value callback = Argument(call.arguments, 0);
    std::size_t index = 0;
    Value accumulator;
    if (call.arguments.size() >= 2) {
      accumulator = call.arguments[1];
    } else {
      while (index < call.self.object->ElementCount() && !call.self.object->HasElement(index)) {
        ++index;
      }
      if (index >= call.self.object->ElementCount()) {
        return Value::Undefined();
      }
      accumulator = call.self.object->GetElement(index);
      ++index;
    }
    for (; index < call.self.object->ElementCount(); ++index) {
      if (!call.self.object->HasElement(index)) {
        continue;
      }
      const Result next = call.interpreter.CallFunction(
          callback, Value::Undefined(),
          {accumulator, call.self.object->GetElement(index),
           Value::Number(static_cast<double>(index)), call.self});
      if (next.IsAbrupt()) {
        return call.ThrowValue(next.value);
      }
      accumulator = next.value;
    }
    return accumulator;
  });

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
}

}  // namespace microbrowser::js

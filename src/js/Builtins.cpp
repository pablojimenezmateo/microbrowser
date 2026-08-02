#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "js/Interpreter.h"
#include "util/Parse.h"

namespace microbrowser::js {

namespace {

Value Argument(const std::vector<Value>& arguments, std::size_t index) {
  return index < arguments.size() ? arguments[index] : Value::Undefined();
}

bool IsJsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::optional<double> ParseFloatPrefix(std::string_view text) {
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
    return std::nullopt;
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
  return util::ParseDouble(prefix);
}

}  // namespace

void Interpreter::InstallGlobals() {
  const auto native = [this](const char* name, NativeFunction function) {
    Object* object = heap_.AllocateObject(Object::Kind::Native);
    object->SetPrototype(function_prototype_);
    object->MakeNative(std::move(function));
    object->Set("name", Value::String(name));
    return object;
  };
  const auto install = [&](Object* target, const char* name, NativeFunction function) {
    target->Set(name, Value::Obj(native(name, std::move(function))));
  };

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
          for (const Value& element : value.object->Elements()) {
            if (!first) {
              out.push_back(',');
            }
            first = false;
            if (!Write(element, depth + 1)) {
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
  install(object_constructor, "keys", [](NativeCall& call) {
    const Value target = Argument(call.arguments, 0);
    std::vector<Value> keys;
    if (target.IsObject()) {
      if (target.object->GetKind() == Object::Kind::Array) {
        for (std::size_t i = 0; i < target.object->Elements().size(); ++i) {
          keys.push_back(Value::String(std::to_string(i)));
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
        for (const Value& element : target.object->Elements()) {
          values.push_back(element);
        }
      }
      for (const std::string& key : target.object->Keys()) {
        const Value* property = target.object->GetOwn(key);
        values.push_back(property == nullptr ? Value::Undefined() : *property);
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
      call.self.object->Elements().push_back(argument);
    }
    return Value::Number(static_cast<double>(call.self.object->Elements().size()));
  });
  install(array_prototype_, "pop", [](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->Elements().empty()) {
      return Value::Undefined();
    }
    Value last = call.self.object->Elements().back();
    call.self.object->Elements().pop_back();
    return last;
  });
  install(array_prototype_, "join", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::String("");
    }
    const Value separator_value = Argument(call.arguments, 0);
    const std::string separator =
        separator_value.IsUndefined() ? "," : ToString(separator_value);
    std::string joined;
    const std::vector<Value>& elements = call.self.object->Elements();
    for (std::size_t i = 0; i < elements.size(); ++i) {
      if (i != 0) {
        joined += separator;
      }
      if (!elements[i].IsNullish()) {
        joined += ToString(elements[i]);
      }
    }
    return Value::String(std::move(joined));
  });
  install(array_prototype_, "indexOf", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Number(-1);
    }
    const Value needle = Argument(call.arguments, 0);
    const std::vector<Value>& elements = call.self.object->Elements();
    for (std::size_t i = 0; i < elements.size(); ++i) {
      if (StrictEquals(elements[i], needle)) {
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
    for (const Value& element : call.self.object->Elements()) {
      if (StrictEquals(element, needle)) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  install(array_prototype_, "slice", [](NativeCall& call) {
    std::vector<Value> out;
    if (call.self.IsObject()) {
      const std::vector<Value>& elements = call.self.object->Elements();
      const double size = static_cast<double>(elements.size());
      double begin = call.arguments.empty() ? 0.0 : ToNumber(call.arguments[0]);
      double end = call.arguments.size() < 2 ? size : ToNumber(call.arguments[1]);
      // A negative index counts from the end, which is what makes slice(-1)
      // idiomatic.
      begin = begin < 0 ? std::max(0.0, size + begin) : std::min(begin, size);
      end = end < 0 ? std::max(0.0, size + end) : std::min(end, size);
      for (double i = begin; i < end; ++i) {
        out.push_back(elements[static_cast<std::size_t>(i)]);
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });
  install(array_prototype_, "map", [](NativeCall& call) {
    std::vector<Value> out;
    if (call.self.IsObject()) {
      const Value callback = Argument(call.arguments, 0);
      const std::vector<Value> elements = call.self.object->Elements();
      for (std::size_t i = 0; i < elements.size(); ++i) {
        const Result mapped = call.interpreter.CallFunction(
            callback, Value::Undefined(),
            {elements[i], Value::Number(static_cast<double>(i)), call.self});
        if (mapped.IsAbrupt()) {
          return Value::Undefined();
        }
        out.push_back(mapped.value);
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });
  install(array_prototype_, "filter", [](NativeCall& call) {
    std::vector<Value> out;
    if (call.self.IsObject()) {
      const Value callback = Argument(call.arguments, 0);
      const std::vector<Value> elements = call.self.object->Elements();
      for (std::size_t i = 0; i < elements.size(); ++i) {
        const Result kept = call.interpreter.CallFunction(
            callback, Value::Undefined(),
            {elements[i], Value::Number(static_cast<double>(i)), call.self});
        if (kept.IsAbrupt()) {
          return Value::Undefined();
        }
        if (ToBoolean(kept.value)) {
          out.push_back(elements[i]);
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
    const std::vector<Value> elements = call.self.object->Elements();
    std::size_t index = 0;
    Value accumulator;
    if (call.arguments.size() >= 2) {
      accumulator = call.arguments[1];
    } else if (!elements.empty()) {
      accumulator = elements[0];
      index = 1;
    }
    for (; index < elements.size(); ++index) {
      const Result next = call.interpreter.CallFunction(
          callback, Value::Undefined(),
          {accumulator, elements[index], Value::Number(static_cast<double>(index)), call.self});
      if (next.IsAbrupt()) {
        return Value::Undefined();
      }
      accumulator = next.value;
    }
    return accumulator;
  });

  // --- String and number conversions ---------------------------------------
  global_scope_->Declare(
      "String", Value::Obj(native("String", [](NativeCall& call) {
        return Value::String(ToString(Argument(call.arguments, 0)));
      })),
      false);
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
        const int radix = call.arguments.size() > 1
                              ? static_cast<int>(ToNumber(call.arguments[1]))
                              : 10;
        char* stop = nullptr;
        const long parsed = std::strtol(text.c_str(), &stop, radix == 0 ? 10 : radix);
        if (stop == text.c_str()) {
          return Value::Number(std::nan(""));
        }
        return Value::Number(static_cast<double>(parsed));
      })),
      false);
  global_scope_->Declare(
      "parseFloat", Value::Obj(native("parseFloat", [](NativeCall& call) {
        const std::string text = ToString(Argument(call.arguments, 0));
        return Value::Number(ParseFloatPrefix(text).value_or(std::nan("")));
      })),
      false);
  global_scope_->Declare(
      "isNaN", Value::Obj(native("isNaN", [](NativeCall& call) {
        return Value::Bool(std::isnan(ToNumber(Argument(call.arguments, 0))));
      })),
      false);
}

}  // namespace microbrowser::js

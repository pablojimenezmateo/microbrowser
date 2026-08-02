#include "js/Value.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>

#include "js/Heap.h"
#include "util/Parse.h"

namespace microbrowser::js {

namespace {

const std::string& EmptyString() {
  static const std::string empty;
  return empty;
}

std::optional<double> ParseJsDecimal(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  if (text == "Infinity" || text == "+Infinity") {
    return std::numeric_limits<double>::infinity();
  }
  if (text == "-Infinity") {
    return -std::numeric_limits<double>::infinity();
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
  }
  return util::ParseDouble(text);
}

}  // namespace

Value Value::String(std::string text) {
  Value value;
  value.type = ValueType::String;
  value.string = std::make_shared<const std::string>(std::move(text));
  return value;
}

const std::string& Value::AsString() const {
  return string == nullptr ? EmptyString() : *string;
}

std::string NumberToString(double number) {
  if (std::isnan(number)) {
    return "NaN";
  }
  if (std::isinf(number)) {
    return number > 0 ? "Infinity" : "-Infinity";
  }
  if (number == 0.0) {
    // Both zeros print as "0". Only Object.is and 1/x can tell them apart, and
    // neither goes through here.
    return "0";
  }
  // An integral value prints without a decimal point, which `%g` and `%f` both
  // get wrong at the edges -- `%g` switches to exponential at six digits and
  // `%f` writes ".000000".
  if (number == std::floor(number) && std::fabs(number) < 1e21) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", number);
    return buffer;
  }
  // Shortest representation that round-trips. 17 significant digits always
  // round-trip; trailing zeros are then trimmed, which is what makes 0.1 print
  // as "0.1" rather than "0.10000000000000001".
  for (int precision = 1; precision <= 17; ++precision) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, number);
    if (util::ParseDouble(buffer).value_or(std::nan("")) == number) {
      return buffer;
    }
  }
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", number);
  return buffer;
}

std::int32_t ToInt32(double number) {
  if (!std::isfinite(number)) {
    return 0;
  }
  double wrapped = std::fmod(std::trunc(number), 4294967296.0);
  if (wrapped < 0) {
    wrapped += 4294967296.0;
  }
  // Through uint32 first: the value is in [0, 2^32) here, which int32 cannot
  // hold, and converting the out-of-range half directly is the undefined cast
  // this function exists to avoid.
  const std::uint32_t bits = static_cast<std::uint32_t>(wrapped);
  return static_cast<std::int32_t>(bits);
}

std::uint32_t ToUint32(double number) { return static_cast<std::uint32_t>(ToInt32(number)); }

bool ToBoolean(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
    case ValueType::Null:
      return false;
    case ValueType::Boolean:
      return value.boolean;
    case ValueType::Number:
      // NaN is falsy, and so is -0. `number != 0` covers both, which is why it
      // is written that way rather than as a comparison against 0.0 only.
      return !std::isnan(value.number) && value.number != 0.0;
    case ValueType::String:
      return !value.AsString().empty();
    case ValueType::Object:
      // Every object is truthy, including an empty array and a Boolean(false)
      // wrapper. This is the rule people are most often surprised by.
      return true;
  }
  return false;
}

double ToNumber(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
      return std::nan("");
    case ValueType::Null:
      return 0.0;
    case ValueType::Boolean:
      return value.boolean ? 1.0 : 0.0;
    case ValueType::Number:
      return value.number;
    case ValueType::String: {
      const std::string& text = value.AsString();
      std::size_t begin = text.find_first_not_of(" \t\n\r\f\v");
      if (begin == std::string::npos) {
        return 0.0;  // an empty or all-whitespace string is 0, not NaN
      }
      const std::size_t end = text.find_last_not_of(" \t\n\r\f\v");
      const std::string trimmed = text.substr(begin, end - begin + 1);
      // The whole string has to be consumed: "12abc" is NaN, not 12.
      return ParseJsDecimal(trimmed).value_or(std::nan(""));
    }
    case ValueType::Object:
      // Without valueOf/toString dispatch this is as far as it goes; an object
      // in arithmetic is NaN, which is the answer for every object that does
      // not override the conversion.
      return std::nan("");
  }
  return std::nan("");
}

std::string ToString(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
      return "undefined";
    case ValueType::Null:
      return "null";
    case ValueType::Boolean:
      return value.boolean ? "true" : "false";
    case ValueType::Number:
      return NumberToString(value.number);
    case ValueType::String:
      return value.AsString();
    case ValueType::Object:
      if (value.object->GetKind() == Object::Kind::Array) {
        // Array.prototype.toString joins with commas, and a null or undefined
        // element contributes nothing rather than its name.
        std::string joined;
        for (std::size_t i = 0; i < value.object->Elements().size(); ++i) {
          if (i != 0) {
            joined.push_back(',');
          }
          const Value& element = value.object->Elements()[i];
          if (!element.IsNullish()) {
            joined += ToString(element);
          }
        }
        return joined;
      }
      if (value.object->GetKind() == Object::Kind::Error) {
        // `String(err)` is "TypeError: message", which is what a caught error
        // printed to a console has to say -- "[object Object]" tells nobody
        // anything.
        const Value* name = value.object->Get("name");
        const Value* message = value.object->Get("message");
        std::string out = name == nullptr ? "Error" : ToString(*name);
        if (message != nullptr && !ToString(*message).empty()) {
          out += ": " + ToString(*message);
        }
        return out;
      }
      if (value.object->IsCallable()) {
        return "function";
      }
      return "[object Object]";
  }
  return "undefined";
}

std::string_view TypeOf(const Value& value) {
  switch (value.type) {
    case ValueType::Undefined:
      return "undefined";
    case ValueType::Null:
      // The famous mistake, preserved because the web depends on it.
      return "object";
    case ValueType::Boolean:
      return "boolean";
    case ValueType::Number:
      return "number";
    case ValueType::String:
      return "string";
    case ValueType::Object:
      return value.object->IsCallable() ? "function" : "object";
  }
  return "undefined";
}

bool StrictEquals(const Value& a, const Value& b) {
  if (a.type != b.type) {
    return false;
  }
  switch (a.type) {
    case ValueType::Undefined:
    case ValueType::Null:
      return true;
    case ValueType::Boolean:
      return a.boolean == b.boolean;
    case ValueType::Number:
      // NaN is not equal to itself, which `==` on doubles already gives.
      return a.number == b.number;
    case ValueType::String:
      return a.AsString() == b.AsString();
    case ValueType::Object:
      return a.object == b.object;
  }
  return false;
}

bool LooseEquals(const Value& a, const Value& b) {
  // null and undefined are equal to each other and to nothing else. This one
  // rule is why `x == null` is the idiomatic nullish check.
  if (a.IsNullish() || b.IsNullish()) {
    return a.IsNullish() && b.IsNullish();
  }
  if (a.type == b.type) {
    return StrictEquals(a, b);
  }
  if (a.type == ValueType::Object || b.type == ValueType::Object) {
    // Without valueOf dispatch an object equals nothing it is not identical
    // to. Saying so is better than a coercion that is wrong in a way nobody
    // can predict.
    return false;
  }
  return ToNumber(a) == ToNumber(b);
}

}  // namespace microbrowser::js

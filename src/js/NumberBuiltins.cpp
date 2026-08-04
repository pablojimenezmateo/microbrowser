#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// `Math` and `Number`.
//
// Grouped because both are arithmetic wearing different names. `Date` was here
// too and is next door now: it grew a calendar, a parser and a formatter, and
// none of those is arithmetic on numbers.
//
// `Math.random` is the one privacy decision in this file, and it is documented
// where it is seeded.

namespace microbrowser::js {

namespace {

double ArgumentNumber(const NativeCall& call, std::size_t index) {
  return ToNumber(Argument(call.arguments, index));
}

}  // namespace

void Interpreter::InstallNumbers(Object* math) {
  // --- Math -----------------------------------------------------------------

  const auto unary = [this, math](const char* name, double (*function)(double)) {
    InstallNative(math, name, [function](NativeCall& call) {
      return Value::Number(function(ArgumentNumber(call, 0)));
    });
  };
  unary("trunc", [](double v) { return std::trunc(v); });
  unary("sign", [](double v) {
    // Not `v > 0 ? 1 : -1`: NaN has no sign and both zeros keep theirs.
    return std::isnan(v) || v == 0.0 ? v : (v > 0.0 ? 1.0 : -1.0);
  });
  unary("log", [](double v) { return std::log(v); });
  unary("log2", [](double v) { return std::log2(v); });
  unary("log10", [](double v) { return std::log10(v); });
  unary("exp", [](double v) { return std::exp(v); });
  unary("sin", [](double v) { return std::sin(v); });
  unary("cos", [](double v) { return std::cos(v); });
  unary("tan", [](double v) { return std::tan(v); });
  unary("asin", [](double v) { return std::asin(v); });
  unary("acos", [](double v) { return std::acos(v); });
  unary("atan", [](double v) { return std::atan(v); });
  unary("cbrt", [](double v) {
    // One Newton step over the library's answer. glibc's `cbrt(27)` is
    // 3.0000000000000004, which is within a ulp and is still the wrong answer
    // to a page that compares it against 3 -- and a cube root of a perfect
    // cube is exactly what a page computes one for.
    const double root = std::cbrt(v);
    if (!std::isfinite(root) || root == 0.0) {
      return root;
    }
    return root - (root * root * root - v) / (3.0 * root * root);
  });
  unary("sinh", [](double v) { return std::sinh(v); });
  unary("cosh", [](double v) { return std::cosh(v); });
  unary("tanh", [](double v) { return std::tanh(v); });
  unary("asinh", [](double v) { return std::asinh(v); });
  unary("acosh", [](double v) { return std::acosh(v); });
  unary("atanh", [](double v) { return std::atanh(v); });
  // The two that exist because `exp(x) - 1` and `log(1 + x)` lose every
  // significant digit near zero. A page doing compound interest or a decay
  // curve is the one that notices.
  unary("expm1", [](double v) { return std::expm1(v); });
  unary("log1p", [](double v) { return std::log1p(v); });
  unary("fround", [](double v) {
    // Round-trip through single precision, which is what a page uses to ask
    // "what would a Float32Array make of this".
    return static_cast<double>(static_cast<float>(v));
  });
  InstallNative(math, "clz32", [](NativeCall& call) {
    const std::uint32_t bits = ToUint32(ArgumentNumber(call, 0));
    if (bits == 0) {
      return Value::Number(32.0);
    }
    int count = 0;
    for (std::uint32_t mask = 0x80000000u; (bits & mask) == 0; mask >>= 1) {
      ++count;
    }
    return Value::Number(count);
  });
  InstallNative(math, "imul", [](NativeCall& call) {
    // Through uint32 and back: signed overflow is undefined behaviour and both
    // operands came from a page. The wrap is the whole point of the function.
    const std::uint32_t left = ToUint32(ArgumentNumber(call, 0));
    const std::uint32_t right = ToUint32(ArgumentNumber(call, 1));
    return Value::Number(static_cast<std::int32_t>(left * right));
  });
  InstallNative(math, "atan2", [](NativeCall& call) {
    return Value::Number(std::atan2(ArgumentNumber(call, 0), ArgumentNumber(call, 1)));
  });
  InstallNative(math, "hypot", [](NativeCall& call) {
    double total = 0.0;
    for (const Value& argument : call.arguments) {
      const double value = ToNumber(argument);
      total += value * value;
    }
    return Value::Number(std::sqrt(total));
  });
  math->Set("LN2", Value::Number(0.6931471805599453));
  math->Set("LN10", Value::Number(2.302585092994046));
  math->Set("SQRT2", Value::Number(1.4142135623730951));
  math->Set("SQRT1_2", Value::Number(0.7071067811865476));
  math->Set("LOG2E", Value::Number(1.4426950408889634));
  math->Set("LOG10E", Value::Number(0.4342944819032518));

  // `Math.random`.
  //
  // Seeded from the clock, and deliberately not from anything better. This is
  // not a source a page may rely on for secrecy -- the spec says so, and a
  // page that needs unpredictability has to ask for `crypto.getRandomValues`,
  // which is a separate thing this engine does not have yet and which must not
  // be silently stood in for. What it does need to be is *different between
  // runs*, or a page that shuffles a list would shuffle it the same way every
  // time.
  //
  // xorshift64*, which is small enough to read and far better distributed than
  // the linear congruential generator it would otherwise be tempting to write.
  if (Object* random = NewNative("random", [](NativeCall& call) {
        const Value* state = call.callee == nullptr ? nullptr : call.callee->GetOwn("#state");
        // The state lives on the function object rather than in a static, so
        // two Interpreters do not share a stream -- which would make one
        // page's draws depend on another's.
        auto bits = static_cast<std::uint64_t>(
            state == nullptr ? 1.0 : ToNumber(*state));
        bits ^= bits << 13;
        bits ^= bits >> 7;
        bits ^= bits << 17;
        if (call.callee != nullptr) {
          call.callee->SetHidden("#state", Value::Number(static_cast<double>(bits)));
        }
        // The top 53 bits, which is exactly what a double can hold without
        // rounding -- taking the low ones instead is the classic way to get a
        // generator with a short period in the least significant bit.
        return Value::Number(static_cast<double>(bits >> 11) / 9007199254740992.0);
      })) {
    const auto seed = static_cast<std::uint64_t>(std::time(nullptr));
    random->SetHidden("#state", Value::Number(static_cast<double>(seed * 6364136223846793005ull + 1)));
    math->Set("random", Value::Obj(random));
  }

  // --- Number ---------------------------------------------------------------

  Object* number_prototype = NewObject();
  Value* declared = global_scope_->Lookup("Number");
  Object* number = declared != nullptr && declared->IsObject() ? declared->object : nullptr;
  if (number == nullptr || number_prototype == nullptr) {
    return;
  }
  number_prototype->SetPrototype(well_known_.object_prototype);
  number->Set("prototype", Value::Obj(number_prototype));
  number_prototype->SetHidden("constructor", Value::Obj(number));
  well_known_.number_prototype = number_prototype;

  number->Set("MAX_SAFE_INTEGER", Value::Number(9007199254740991.0));
  number->Set("MIN_SAFE_INTEGER", Value::Number(-9007199254740991.0));
  number->Set("MAX_VALUE", Value::Number(std::numeric_limits<double>::max()));
  number->Set("MIN_VALUE", Value::Number(std::numeric_limits<double>::denorm_min()));
  number->Set("EPSILON", Value::Number(std::numeric_limits<double>::epsilon()));
  number->Set("POSITIVE_INFINITY", Value::Number(HUGE_VAL));
  number->Set("NEGATIVE_INFINITY", Value::Number(-HUGE_VAL));
  number->Set("NaN", Value::Number(std::nan("")));

  // These differ from the global `isNaN` and `isFinite` in one way that
  // matters: they do not convert. `isNaN('x')` is true and `Number.isNaN('x')`
  // is false, because the string is not a number rather than being one that is
  // NaN.
  InstallNative(number, "isNaN", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    return Value::Bool(value.IsNumber() && std::isnan(value.number));
  });
  InstallNative(number, "isFinite", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    return Value::Bool(value.IsNumber() && std::isfinite(value.number));
  });
  InstallNative(number, "isInteger", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    return Value::Bool(value.IsNumber() && std::isfinite(value.number) &&
                       value.number == std::trunc(value.number));
  });
  InstallNative(number, "isSafeInteger", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    return Value::Bool(value.IsNumber() && std::isfinite(value.number) &&
                       value.number == std::trunc(value.number) &&
                       std::fabs(value.number) <= 9007199254740991.0);
  });
  // `Number.parseInt` and `parseInt` are required to be the same function
  // object, so this reads the global one back rather than installing a second.
  for (const char* name : {"parseInt", "parseFloat"}) {
    if (Value* global = global_scope_->Lookup(name)) {
      number->Set(name, *global);
    }
  }

  InstallNative(number_prototype, "toFixed", [](NativeCall& call) {
    const double value = ToNumber(call.self);
    const double digits = ArgumentNumber(call, 0);
    if (digits < 0.0 || digits > 100.0) {
      return call.Throw("RangeError", "toFixed digits must be between 0 and 100");
    }
    if (!std::isfinite(value)) {
      return Value::String(NumberToString(value));
    }
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%.*f",
                  static_cast<int>(std::isnan(digits) ? 0.0 : digits), value);
    return Value::String(std::string(buffer));
  });
  InstallNative(number_prototype, "toString", [](NativeCall& call) {
    const double value = ToNumber(call.self);
    const Value radix_value = Argument(call.arguments, 0);
    const double radix = radix_value.IsUndefined() ? 10.0 : ToNumber(radix_value);
    if (radix == 10.0) {
      return Value::String(NumberToString(value));
    }
    if (radix < 2.0 || radix > 36.0) {
      return call.Throw("RangeError", "toString radix must be between 2 and 36");
    }
    if (!std::isfinite(value)) {
      return Value::String(NumberToString(value));
    }
    // Integral part only. A fractional value in another base is a repeating
    // expansion with no natural stopping point, and every use a page has for
    // this -- a colour, a hash, a bitmask -- is of an integer.
    const auto base = static_cast<std::uint64_t>(radix);
    const bool negative = value < 0.0;
    auto magnitude = static_cast<std::uint64_t>(std::fabs(std::trunc(value)));
    std::string digits;
    do {
      const auto digit = static_cast<std::size_t>(magnitude % base);
      digits.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[digit]);
      magnitude /= base;
    } while (magnitude != 0);
    if (negative) {
      digits.push_back('-');
    }
    return Value::String(std::string(digits.rbegin(), digits.rend()));
  });
  InstallNative(number_prototype, "valueOf", [](NativeCall& call) {
    return Value::Number(ToNumber(call.self));
  });
  InstallNative(number_prototype, "toPrecision", [](NativeCall& call) {
    const double value = ToNumber(call.self);
    const Value digits_value = Argument(call.arguments, 0);
    if (digits_value.IsUndefined()) {
      return Value::String(NumberToString(value));
    }
    const double digits = ToNumber(digits_value);
    if (digits < 1.0 || digits > 100.0) {
      return call.Throw("RangeError", "toPrecision digits must be between 1 and 100");
    }
    if (!std::isfinite(value)) {
      return Value::String(NumberToString(value));
    }
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%.*g", static_cast<int>(digits), value);
    return Value::String(std::string(buffer));
  });
  InstallNative(number_prototype, "toExponential", [](NativeCall& call) {
    const double value = ToNumber(call.self);
    const Value digits_value = Argument(call.arguments, 0);
    const double digits = digits_value.IsUndefined() ? 6.0 : ToNumber(digits_value);
    if (digits < 0.0 || digits > 100.0) {
      return call.Throw("RangeError", "toExponential digits must be between 0 and 100");
    }
    if (!std::isfinite(value)) {
      return Value::String(NumberToString(value));
    }
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%.*e", static_cast<int>(digits), value);
    // C prints at least two exponent digits and JavaScript prints the fewest
    // it can: 1e+5, not 1e+05.
    std::string text(buffer);
    const std::size_t marker = text.find('e');
    if (marker != std::string::npos && marker + 2 < text.size()) {
      std::size_t first = marker + 2;
      while (first + 1 < text.size() && text[first] == '0') {
        text.erase(first, 1);
      }
    }
    return Value::String(std::move(text));
  });
  // No locale data here, and standing in a formatted number for one would be
  // a lie a page cannot detect. The plain form is the honest answer.
  InstallNative(number_prototype, "toLocaleString", [](NativeCall& call) {
    return Value::String(NumberToString(ToNumber(call.self)));
  });

}

}  // namespace microbrowser::js

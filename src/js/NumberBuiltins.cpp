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

// `Math`, `Number`, and `Date`.
//
// Grouped because all three are arithmetic wearing different names, and
// because two of them have a property consequence that is easier to state once
// than three times.
//
// **The clock is a fingerprinting surface.** `Date.now()` at millisecond
// resolution is what every timing attack and every "how fast is this machine"
// probe is built on, and a browser that hands out a nanosecond clock has given
// away more than the time. Millisecond resolution is what the platform offers
// and what the spec requires, so it is what is given -- but `Math.random` is a
// separate decision and is documented where it is seeded.

namespace microbrowser::js {

namespace {

double ArgumentNumber(const NativeCall& call, std::size_t index) {
  return ToNumber(Argument(call.arguments, index));
}

// Splits a timestamp into the fields a Date reports.
//
// Local time, via the platform's own conversion: the alternative is carrying a
// timezone database, and a browser that reported UTC as local time would be
// wrong for almost everyone.
std::tm BreakDown(double milliseconds) {
  const auto seconds = static_cast<std::time_t>(std::floor(milliseconds / 1000.0));
  std::tm parts{};
#if defined(_WIN32)
  localtime_s(&parts, &seconds);
#else
  localtime_r(&seconds, &parts);
#endif
  return parts;
}

std::string TwoDigits(int value) {
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02d", value);
  return buffer;
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
  unary("cbrt", [](double v) { return std::cbrt(v); });
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
          call.callee->Set("#state", Value::Number(static_cast<double>(bits)));
        }
        // The top 53 bits, which is exactly what a double can hold without
        // rounding -- taking the low ones instead is the classic way to get a
        // generator with a short period in the least significant bit.
        return Value::Number(static_cast<double>(bits >> 11) / 9007199254740992.0);
      })) {
    const auto seed = static_cast<std::uint64_t>(std::time(nullptr));
    random->Set("#state", Value::Number(static_cast<double>(seed * 6364136223846793005ull + 1)));
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
  number_prototype->Set("constructor", Value::Obj(number));
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

  // --- Date -----------------------------------------------------------------

  Object* date_prototype = NewObject();
  Object* date = NewNative("Date", [](NativeCall& call) {
    Object* instance = call.interpreter.GetHeap().AllocateObject(Object::Kind::Plain);
    if (instance == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    const Value* prototype =
        call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
    if (prototype != nullptr && prototype->IsObject()) {
      instance->SetPrototype(prototype->object);
    }
    // `new Date()` is now, `new Date(ms)` is that instant, and `new Date(text)`
    // is not parsed -- date parsing is a format zoo and a wrong answer there is
    // worse than an honest NaN.
    double milliseconds = 0.0;
    if (call.arguments.empty()) {
      milliseconds = static_cast<double>(std::time(nullptr)) * 1000.0;
    } else if (call.arguments[0].IsNumber()) {
      milliseconds = call.arguments[0].number;
    } else {
      milliseconds = std::nan("");
    }
    instance->Set("#time", Value::Number(milliseconds));
    return Value::Obj(instance);
  });
  if (date == nullptr || date_prototype == nullptr) {
    return;
  }
  date_prototype->SetPrototype(well_known_.object_prototype);
  date->Set("prototype", Value::Obj(date_prototype));
  date_prototype->Set("constructor", Value::Obj(date));
  global_scope_->Declare("Date", Value::Obj(date), false);

  InstallNative(date, "now", [](NativeCall&) {
    // Millisecond resolution, which is what the spec asks for and as far as
    // this should go: a finer clock is a timing side channel handed to every
    // page that asks for the time.
    return Value::Number(static_cast<double>(std::time(nullptr)) * 1000.0);
  });

  const auto field = [this, date_prototype](const char* name, int std::tm::*member,
                                            int offset) {
    InstallNative(date_prototype, name, [member, offset](NativeCall& call) {
      const Value* time = call.self.IsObject() ? call.self.object->GetOwn("#time") : nullptr;
      if (time == nullptr || std::isnan(ToNumber(*time))) {
        return Value::Number(std::nan(""));
      }
      const std::tm parts = BreakDown(ToNumber(*time));
      return Value::Number(static_cast<double>(parts.*member + offset));
    });
  };
  field("getFullYear", &std::tm::tm_year, 1900);
  // Zero-based, which is the language's most notorious off-by-one and is
  // preserved rather than fixed: every page that formats a date adds one.
  field("getMonth", &std::tm::tm_mon, 0);
  field("getDate", &std::tm::tm_mday, 0);
  field("getDay", &std::tm::tm_wday, 0);
  field("getHours", &std::tm::tm_hour, 0);
  field("getMinutes", &std::tm::tm_min, 0);
  field("getSeconds", &std::tm::tm_sec, 0);

  const auto milliseconds = [](NativeCall& call) {
    const Value* time = call.self.IsObject() ? call.self.object->GetOwn("#time") : nullptr;
    return Value::Number(time == nullptr ? std::nan("") : ToNumber(*time));
  };
  InstallNative(date_prototype, "getTime", milliseconds);
  InstallNative(date_prototype, "valueOf", milliseconds);
  InstallNative(date_prototype, "getMilliseconds", [](NativeCall& call) {
    const Value* time = call.self.IsObject() ? call.self.object->GetOwn("#time") : nullptr;
    const double value = time == nullptr ? std::nan("") : ToNumber(*time);
    return Value::Number(std::isnan(value) ? value : std::fmod(value, 1000.0));
  });
  InstallNative(date_prototype, "toISOString", [](NativeCall& call) {
    const Value* time = call.self.IsObject() ? call.self.object->GetOwn("#time") : nullptr;
    const double value = time == nullptr ? std::nan("") : ToNumber(*time);
    if (std::isnan(value)) {
      return call.Throw("RangeError", "an invalid date has no ISO form");
    }
    const auto seconds = static_cast<std::time_t>(std::floor(value / 1000.0));
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    return Value::String(std::to_string(parts.tm_year + 1900) + "-" +
                         TwoDigits(parts.tm_mon + 1) + "-" + TwoDigits(parts.tm_mday) + "T" +
                         TwoDigits(parts.tm_hour) + ":" + TwoDigits(parts.tm_min) + ":" +
                         TwoDigits(parts.tm_sec) + "Z");
  });
}

}  // namespace microbrowser::js

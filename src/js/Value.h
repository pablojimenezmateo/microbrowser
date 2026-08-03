#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::js {

class Object;

enum class ValueType : std::uint8_t { Undefined, Null, Boolean, Number, String, Object, Symbol };

// A JavaScript value.
//
// Strings are reference-counted and objects are garbage-collected, and the
// split is not an inconsistency: a string is immutable and cannot participate
// in a cycle, so a refcount is exactly right and costs nothing to reason about.
// An object can hold a reference to itself through a closure before it is
// finished being constructed, which is why objects need a tracing collector
// and strings do not.
struct Value {
  ValueType type = ValueType::Undefined;
  bool boolean = false;
  double number = 0.0;
  std::shared_ptr<const std::string> string;
  // Owned by the Heap, never by this. A raw pointer says so; a smart pointer
  // here would be a second ownership story competing with the collector.
  Object* object = nullptr;

  static Value Undefined() { return Value{}; }
  static Value Null() {
    Value value;
    value.type = ValueType::Null;
    return value;
  }
  static Value Bool(bool boolean) {
    Value value;
    value.type = ValueType::Boolean;
    value.boolean = boolean;
    return value;
  }
  static Value Number(double number) {
    Value value;
    value.type = ValueType::Number;
    value.number = number;
    return value;
  }
  static Value String(std::string text);
  static Value Obj(Object* object) {
    Value value;
    value.type = ValueType::Object;
    value.object = object;
    return value;
  }
  // A symbol. Its cell is collected like an object and its *identity* is the
  // cell, which is what makes two symbols with the same description different
  // -- and what makes a symbol usable as a key no page can write out.
  static Value Sym(Object* cell) {
    Value value;
    value.type = ValueType::Symbol;
    value.object = cell;
    return value;
  }

  bool IsUndefined() const { return type == ValueType::Undefined; }
  bool IsNull() const { return type == ValueType::Null; }
  bool IsNullish() const { return IsUndefined() || IsNull(); }
  bool IsObject() const { return type == ValueType::Object && object != nullptr; }
  bool IsSymbol() const { return type == ValueType::Symbol && object != nullptr; }
  bool IsString() const { return type == ValueType::String; }
  bool IsNumber() const { return type == ValueType::Number; }

  const std::string& AsString() const;
};

// The abstract operations. These are the conversions the language performs
// implicitly, and they are the reason `[] + {}` has an answer at all -- so they
// live in one place, spelled the way the spec spells them, rather than being
// re-derived at each operator.
bool ToBoolean(const Value& value);
double ToNumber(const Value& value);
std::string ToString(const Value& value);
// Number to string, per the spec's rules: integers print without a decimal
// point, NaN prints as "NaN", and 1e21 switches to exponential. A plain printf
// gets every one of those wrong.
std::string NumberToString(double number);
// ToInt32, per the spec: truncate, wrap modulo 2^32, reinterpret as signed.
//
// Not a cast. `static_cast<std::int64_t>(6.7e70)` is undefined behaviour, and
// every bitwise operator in the language runs its operands through this -- so
// a value from a page reaches it directly. Found by the fuzzer, in `~`.
std::int32_t ToInt32(double number);
std::uint32_t ToUint32(double number);

// The `typeof` operator's answer.
std::string_view TypeOf(const Value& value);
// `===`. `==` is deliberately absent until something needs it: the coercing
// comparison is a table of special cases and adding it untested would be worse
// than not having it.
bool StrictEquals(const Value& a, const Value& b);
// `==`, with the coercions the spec defines.
bool LooseEquals(const Value& a, const Value& b);

}  // namespace microbrowser::js

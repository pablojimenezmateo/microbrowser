#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "js/Bytecode.h"
#include "js/Interpreter.h"

// The binary operators, in one place.
//
// They were written out inside the tree-walker's EvaluateBinary, which was fine
// while there was one evaluator. There are two now, and `+` is the operator
// whose asymmetry surprises people -- so a second copy of it would be a second
// answer to `[] + {}` waiting to drift from the first. Both paths call
// ApplyBinary and neither knows how it works.

namespace microbrowser::js {

bool ParseBinaryOp(std::string_view text, BinaryOp& op) {
  struct Entry {
    std::string_view text;
    BinaryOp op;
  };
  // Ordered by nothing in particular: the lookup is a linear scan over twenty
  // two entries done once per operator *at compile time*, not per evaluation.
  static constexpr Entry kOperators[] = {
      {"+", BinaryOp::Add},
      {"-", BinaryOp::Subtract},
      {"*", BinaryOp::Multiply},
      {"/", BinaryOp::Divide},
      {"%", BinaryOp::Remainder},
      {"**", BinaryOp::Exponent},
      {"==", BinaryOp::LooseEqual},
      {"!=", BinaryOp::LooseNotEqual},
      {"===", BinaryOp::StrictEqual},
      {"!==", BinaryOp::StrictNotEqual},
      {"<", BinaryOp::Less},
      {">", BinaryOp::Greater},
      {"<=", BinaryOp::LessEqual},
      {">=", BinaryOp::GreaterEqual},
      {"&", BinaryOp::BitAnd},
      {"|", BinaryOp::BitOr},
      {"^", BinaryOp::BitXor},
      {"<<", BinaryOp::ShiftLeft},
      {">>", BinaryOp::ShiftRight},
      {">>>", BinaryOp::ShiftRightUnsigned},
      {"in", BinaryOp::In},
      {"instanceof", BinaryOp::InstanceOf},
  };
  for (const Entry& entry : kOperators) {
    if (entry.text == text) {
      op = entry.op;
      return true;
    }
  }
  return false;
}

// ToPrimitive, and the three conversions built on it.
//
// The pure ToNumber and ToString in Value.h cannot call `valueOf`, because
// they have no interpreter to call it with. These can, which is why they are
// methods and why they return a Result: a `valueOf` a page wrote can throw,
// and so can the conversion itself when nothing it tries gives a primitive.

Result Interpreter::ToPrimitive(const Value& value, Hint hint, Value& out) {
  if (!value.IsObject()) {
    out = value;
    return Result::Normal();
  }
  // The override comes first, and finding it is a property lookup rather than
  // a check on the object's kind: an ordinary object with the method behaves
  // exactly as a Date does, which is the point of it being a symbol.
  if (shared_.symbol_to_primitive != nullptr) {
    const Value exotic = GetProperty(value, PropertyKey::Symbol(shared_.symbol_to_primitive));
    if (exotic.IsObject() && exotic.object->IsCallable()) {
      const char* name = hint == Hint::String   ? "string"
                         : hint == Hint::Number ? "number"
                                                : "default";
      const Result asked =
          CallFunction(exotic, value, {Value::String(std::string(name))});
      if (asked.IsAbrupt()) {
        return asked;
      }
      if (asked.value.IsObject()) {
        return Throw("TypeError", "Symbol.toPrimitive returned an object");
      }
      out = asked.value;
      return Result::Normal();
    }
  }
  // OrdinaryToPrimitive. The hint only reorders these two; it does not change
  // which are tried, which is why an object with neither is a TypeError under
  // every hint.
  const char* first = hint == Hint::String ? "toString" : "valueOf";
  const char* second = hint == Hint::String ? "valueOf" : "toString";
  for (const char* name : {first, second}) {
    const Value method = GetProperty(value, PropertyKey(name));
    if (!method.IsObject() || !method.object->IsCallable()) {
      continue;
    }
    const Result asked = CallFunction(method, value, {});
    if (asked.IsAbrupt()) {
      return asked;
    }
    if (!asked.value.IsObject()) {
      out = asked.value;
      return Result::Normal();
    }
  }
  return Throw("TypeError", "cannot convert an object to a primitive value");
}

Result Interpreter::ToNumberOf(const Value& value, double& out) {
  if (value.IsSymbol()) {
    // The spec's TypeError, which the pure ToNumber cannot throw and so
    // answers NaN for. Arithmetic on a symbol is a mistake, and saying so at
    // the operator is the whole reason this path exists.
    return Throw("TypeError", "cannot convert a symbol to a number");
  }
  Value primitive;
  const Result converted = ToPrimitive(value, Hint::Number, primitive);
  if (converted.IsAbrupt()) {
    return converted;
  }
  if (primitive.IsSymbol()) {
    return Throw("TypeError", "cannot convert a symbol to a number");
  }
  out = ToNumber(primitive);
  return Result::Normal();
}

Result Interpreter::ToStringOf(const Value& value, std::string& out) {
  if (value.IsSymbol()) {
    return Throw("TypeError", "cannot convert a symbol to a string");
  }
  Value primitive;
  const Result converted = ToPrimitive(value, Hint::String, primitive);
  if (converted.IsAbrupt()) {
    return converted;
  }
  if (primitive.IsSymbol()) {
    return Throw("TypeError", "cannot convert a symbol to a string");
  }
  out = ToString(primitive);
  return Result::Normal();
}

Result Interpreter::ToKeyOf(const Value& value, PropertyKey& out) {
  // A symbol is already a key and must not be stringified -- that is what
  // makes `Symbol.iterator` unreachable by writing its description out.
  if (value.IsSymbol()) {
    out = PropertyKey::Symbol(value.object);
    return Result::Normal();
  }
  if (!value.IsObject()) {
    out = KeyFrom(value);
    return Result::Normal();
  }
  Value primitive;
  const Result converted = ToPrimitive(value, Hint::String, primitive);
  if (converted.IsAbrupt()) {
    return converted;
  }
  out = KeyFrom(primitive);
  return Result::Normal();
}

Result Interpreter::LooseEqualsOf(const Value& a, const Value& b, bool& out) {
  // Bounded rather than recursive. Each side converts at most once -- an
  // object becomes a primitive and a primitive never becomes an object -- so
  // two passes is the fixpoint, and writing the bound out is cheaper than
  // trusting a `valueOf` a page wrote not to hand back another object.
  Value x = a;
  Value y = b;
  const auto nullish_or_dda = [](const Value& value) {
    return value.IsNullish() || (value.IsObject() && value.object->IsHTMLDDA());
  };
  for (int pass = 0; pass < 2; ++pass) {
    if (nullish_or_dda(x) || nullish_or_dda(y)) {
      out = nullish_or_dda(x) && nullish_or_dda(y);
      return Result::Normal();
    }
    if (x.type == y.type) {
      out = StrictEquals(x, y);
      return Result::Normal();
    }
    // An object compares by becoming a primitive, and only then. `{} == {}`
    // is false because both sides are objects and the type test above caught
    // it, not because the conversion said so. HTMLDDA was handled above.
    if (x.IsObject()) {
      const Result converted = ToPrimitive(x, Hint::Default, x);
      if (converted.IsAbrupt()) {
        return converted;
      }
      continue;
    }
    if (y.IsObject()) {
      const Result converted = ToPrimitive(y, Hint::Default, y);
      if (converted.IsAbrupt()) {
        return converted;
      }
      continue;
    }
    // Two primitives of different types. A symbol equals nothing it is not
    // identical to, which the type test already settled; everything else
    // compares as a number -- and a bigint compares by *mathematical value*,
    // which is the one place the two numeric types meet.
    if (x.IsSymbol() || y.IsSymbol()) {
      out = false;
      return Result::Normal();
    }
    if (x.IsBigInt() || y.IsBigInt()) {
      out = LooseEquals(x, y);
      return Result::Normal();
    }
    out = ToNumber(x) == ToNumber(y);
    return Result::Normal();
  }
  out = StrictEquals(x, y);
  return Result::Normal();
}

// The bigint half of an operator, when either side is one.
//
// Handled before the numeric conversions rather than inside them, and that is
// the whole design of the type: mixing a bigint and a number is a **TypeError**,
// not a coercion. A bigint that silently became a double would lose the
// precision it exists for, and a page that added one to a database identifier
// would find out by writing the wrong row.
//
// The exceptions are written out below and there are exactly three: the
// comparisons, `==`, and `+` when the other side is a string -- all of which
// compare or concatenate rather than compute.
Result Interpreter::ApplyBigIntBinary(BinaryOp op, const Value& a, const Value& b,
                                      bool& handled) {
  handled = false;
  if (!a.IsBigInt() && !b.IsBigInt()) {
    return Result::Normal();
  }

  // The relational operators, which compare by mathematical value across the
  // two numeric types. `1n < 1.5` is true and neither side converts.
  const auto relational = [&](int order) {
    const bool answer = op == BinaryOp::Less       ? order < 0
                        : op == BinaryOp::Greater  ? order > 0
                        : op == BinaryOp::LessEqual ? order <= 0
                                                    : order >= 0;
    // An unordered comparison -- against NaN -- is false whichever way it is
    // written, which `order == 2` stands for.
    return Result::Normal(Value::Bool(order != 2 && answer));
  };
  if (op == BinaryOp::Less || op == BinaryOp::Greater || op == BinaryOp::LessEqual ||
      op == BinaryOp::GreaterEqual) {
    Value left;
    Value right;
    const Result converted_left = ToPrimitive(a, Hint::Number, left);
    if (converted_left.IsAbrupt()) {
      return converted_left;
    }
    const Result converted_right = ToPrimitive(b, Hint::Number, right);
    if (converted_right.IsAbrupt()) {
      return converted_right;
    }
    if (!left.IsBigInt() && !right.IsBigInt()) {
      return Result::Normal();  // neither is one after conversion
    }
    handled = true;
    if (left.IsBigInt() && right.IsBigInt()) {
      return relational(BigInt::Compare(*BigIntOf(left), *BigIntOf(right)));
    }
    const bool big_left = left.IsBigInt();
    const Value& big = big_left ? left : right;
    const Value& other = big_left ? right : left;
    if (other.IsString()) {
      BigInt parsed;
      if (!BigInt::Parse(other.AsString(), parsed)) {
        return Result::Normal(Value::Bool(false));  // unparseable is unordered
      }
      const int order = BigInt::Compare(*BigIntOf(big), parsed);
      return relational(big_left ? order : -order);
    }
    const int order = BigInt::CompareDouble(*BigIntOf(big), ToNumber(other));
    return relational(order == 2 ? 2 : (big_left ? order : -order));
  }

  // `==` across the two, which LooseEqualsOf already routes through
  // LooseEquals -- so nothing to do here.
  if (op == BinaryOp::LooseEqual || op == BinaryOp::LooseNotEqual ||
      op == BinaryOp::StrictEqual || op == BinaryOp::StrictNotEqual ||
      op == BinaryOp::In || op == BinaryOp::InstanceOf) {
    return Result::Normal();
  }

  // Everything left is arithmetic. Both sides become primitives first, because
  // an object with a `valueOf` that answers a bigint is a bigint here.
  Value left;
  Value right;
  const Result converted_left = ToPrimitive(a, Hint::Number, left);
  if (converted_left.IsAbrupt()) {
    return converted_left;
  }
  const Result converted_right = ToPrimitive(b, Hint::Number, right);
  if (converted_right.IsAbrupt()) {
    return converted_right;
  }
  if (!left.IsBigInt() && !right.IsBigInt()) {
    return Result::Normal();
  }
  // `1n + 'x'` concatenates, which is the one arithmetic operator that does
  // not require both sides to be the same type.
  if (op == BinaryOp::Add && (left.IsString() || right.IsString())) {
    return Result::Normal();
  }
  handled = true;
  if (!left.IsBigInt() || !right.IsBigInt()) {
    return Throw("TypeError", "cannot mix BigInt and other types");
  }

  const BigInt& x = *BigIntOf(left);
  const BigInt& y = *BigIntOf(right);
  BigInt made;
  bool ok = false;
  switch (op) {
    case BinaryOp::Add: ok = BigInt::Add(x, y, made); break;
    case BinaryOp::Subtract: ok = BigInt::Subtract(x, y, made); break;
    case BinaryOp::Multiply: ok = BigInt::Multiply(x, y, made); break;
    case BinaryOp::Divide: {
      if (y.IsZero()) {
        return Throw("RangeError", "division by zero");
      }
      BigInt remainder;
      ok = BigInt::Divide(x, y, made, remainder);
      break;
    }
    case BinaryOp::Remainder: {
      if (y.IsZero()) {
        return Throw("RangeError", "division by zero");
      }
      BigInt quotient;
      ok = BigInt::Divide(x, y, quotient, made);
      break;
    }
    case BinaryOp::Exponent:
      if (y.Negative()) {
        return Throw("RangeError", "a BigInt cannot be raised to a negative power");
      }
      ok = BigInt::Power(x, y, made);
      break;
    case BinaryOp::BitAnd: ok = BigInt::Bitwise(x, y, 0, made); break;
    case BinaryOp::BitOr: ok = BigInt::Bitwise(x, y, 1, made); break;
    case BinaryOp::BitXor: ok = BigInt::Bitwise(x, y, 2, made); break;
    case BinaryOp::ShiftLeft:
    case BinaryOp::ShiftRight: {
      // The shift count is a bigint too, and an unbounded one -- so it is
      // taken through a double, where anything past the limb cap is refused
      // rather than truncated into something plausible.
      const double by = y.ToDouble();
      if (!std::isfinite(by) || std::fabs(by) > 1e9) {
        return Throw("RangeError", "shift count is too large");
      }
      const auto amount = static_cast<std::int64_t>(by);
      ok = BigInt::ShiftLeft(x, op == BinaryOp::ShiftLeft ? amount : -amount, made);
      break;
    }
    case BinaryOp::ShiftRightUnsigned:
      // There is no unsigned right shift for an unbounded integer: it is
      // defined over a fixed width, and a bigint has none. The spec makes it a
      // TypeError rather than picking one.
      return Throw("TypeError", "BigInts have no unsigned right shift");
    default:
      return Throw("TypeError", "unsupported BigInt operation");
  }
  if (!ok) {
    return Throw("RangeError", "BigInt is too large");
  }
  const Value value = NewBigIntValue(std::move(made));
  if (value.IsUndefined()) {
    return Throw("RangeError", "out of memory");
  }
  return Result::Normal(value);
}

Result Interpreter::ApplyBigIntUnary(const char* op, const Value& operand, bool& handled) {
  handled = false;
  if (!operand.IsBigInt()) {
    return Result::Normal();
  }
  handled = true;
  const std::string_view which(op);
  if (which == "+") {
    // The one unary operator a bigint refuses. `+x` is defined as ToNumber,
    // and a bigint has no number to become -- so the spec makes it a
    // TypeError rather than a lossy conversion.
    return Throw("TypeError", "cannot convert a BigInt to a number");
  }
  const BigInt& digits = *BigIntOf(operand);
  BigInt made;
  bool ok = true;
  if (which == "-") {
    made = BigInt::Negate(digits);
  } else if (which == "~") {
    ok = BigInt::Not(digits, made);
  } else if (which == "++") {
    ok = BigInt::Add(digits, BigInt::FromInt64(1), made);
  } else if (which == "--") {
    ok = BigInt::Subtract(digits, BigInt::FromInt64(1), made);
  } else {
    handled = false;
    return Result::Normal();
  }
  if (!ok) {
    return Throw("RangeError", "BigInt is too large");
  }
  const Value value = NewBigIntValue(std::move(made));
  if (value.IsUndefined()) {
    return Throw("RangeError", "out of memory");
  }
  return Result::Normal(value);
}

Result Interpreter::ApplyNumericUnary(Op op, std::uint32_t operand, const Value& value) {
  // A bigint answers all of these itself: `-1n` and `~1n` are bigints, `+1n`
  // is a TypeError, and `++` steps by `1n`. None of that is expressible as a
  // conversion, which is why the type is checked before one runs.
  if (value.IsBigInt()) {
    const char* which = op == Op::Negate      ? "-"
                        : op == Op::BitNot    ? "~"
                        : op == Op::UnaryPlus ? "+"
                        : op == Op::StepValue ? (operand == 1 ? "++" : "--")
                                              : "";
    bool handled = false;
    const Result stepped = ApplyBigIntUnary(which, value, handled);
    if (stepped.IsAbrupt()) {
      return stepped;
    }
    // ToNumberOp on a bigint is the identity: the step that follows is the
    // bigint's own.
    return handled ? stepped : Result::Normal(value);
  }
  if (op == Op::StepValue) {
    return Result::Normal(Value::Number(ToNumber(value) + (operand == 1 ? 1.0 : -1.0)));
  }
  // Through ToNumberOf, which can run a page's `valueOf` and therefore can
  // throw.
  double number = 0;
  const Result converted = ToNumberOf(value, number);
  if (converted.IsAbrupt()) {
    return converted;
  }
  if (op == Op::Negate) {
    return Result::Normal(Value::Number(-number));
  }
  if (op == Op::BitNot) {
    return Result::Normal(Value::Number(~ToInt32(number)));
  }
  return Result::Normal(Value::Number(number));
}

Result Interpreter::ApplyBinary(BinaryOp op, const Value& a, const Value& b) {
  // The bigint cases first: mixing a bigint and a number is a TypeError rather
  // than a coercion, so the ordinary conversions below must not get the chance.
  bool handled = false;
  const Result big = ApplyBigIntBinary(op, a, b, handled);
  if (handled || big.IsAbrupt()) {
    return big;
  }

  // The arithmetic and relational operators all begin by converting both
  // operands, and every one of those conversions can run a page's `valueOf`
  // and therefore throw. Doing it once here rather than per case is what keeps
  // that from being twelve places to forget it.
  const auto numbers = [&](double& x, double& y) -> Result {
    const Result left = ToNumberOf(a, x);
    if (left.IsAbrupt()) {
      return left;
    }
    return ToNumberOf(b, y);
  };
  const auto bits = [&](std::int32_t& x, std::int32_t& y) -> Result {
    double lhs = 0;
    double rhs = 0;
    const Result converted = numbers(lhs, rhs);
    if (converted.IsAbrupt()) {
      return converted;
    }
    x = ToInt32(lhs);
    y = ToInt32(rhs);
    return Result::Normal();
  };

  switch (op) {
    case BinaryOp::Add: {
      // The one operator that is not arithmetic-only. Both sides convert with
      // the *default* hint first -- which is why `date + 1` concatenates and
      // `date - 1` subtracts -- and only then does either being a string
      // decide between concatenation and addition.
      Value left;
      Value right;
      const Result converted_left = ToPrimitive(a, Hint::Default, left);
      if (converted_left.IsAbrupt()) {
        return converted_left;
      }
      const Result converted_right = ToPrimitive(b, Hint::Default, right);
      if (converted_right.IsAbrupt()) {
        return converted_right;
      }
      if (left.IsString() || right.IsString()) {
        std::string x;
        std::string y;
        const Result text_left = ToStringOf(left, x);
        if (text_left.IsAbrupt()) {
          return text_left;
        }
        const Result text_right = ToStringOf(right, y);
        if (text_right.IsAbrupt()) {
          return text_right;
        }
        return Result::Normal(Value::String(x + y));
      }
      double x = 0;
      double y = 0;
      const Result number_left = ToNumberOf(left, x);
      if (number_left.IsAbrupt()) {
        return number_left;
      }
      const Result number_right = ToNumberOf(right, y);
      if (number_right.IsAbrupt()) {
        return number_right;
      }
      return Result::Normal(Value::Number(x + y));
    }
    case BinaryOp::Subtract:
    case BinaryOp::Multiply:
    case BinaryOp::Divide:
    case BinaryOp::Remainder:
    case BinaryOp::Exponent: {
      double x = 0;
      double y = 0;
      const Result converted = numbers(x, y);
      if (converted.IsAbrupt()) {
        return converted;
      }
      const double answer = op == BinaryOp::Subtract  ? x - y
                            : op == BinaryOp::Multiply ? x * y
                            : op == BinaryOp::Divide   ? x / y
                            : op == BinaryOp::Remainder ? std::fmod(x, y)
                                                        : std::pow(x, y);
      return Result::Normal(Value::Number(answer));
    }

    case BinaryOp::LooseEqual:
    case BinaryOp::LooseNotEqual: {
      bool equal = false;
      const Result compared = LooseEqualsOf(a, b, equal);
      if (compared.IsAbrupt()) {
        return compared;
      }
      return Result::Normal(Value::Bool(op == BinaryOp::LooseEqual ? equal : !equal));
    }
    case BinaryOp::StrictEqual:
      return Result::Normal(Value::Bool(StrictEquals(a, b)));
    case BinaryOp::StrictNotEqual:
      return Result::Normal(Value::Bool(!StrictEquals(a, b)));

    case BinaryOp::Less:
    case BinaryOp::Greater:
    case BinaryOp::LessEqual:
    case BinaryOp::GreaterEqual: {
      // Both sides become primitives with the *number* hint, and only two
      // strings compare as text. That order matters: `[2] < [11]` is false,
      // because both become strings and "2" > "11".
      Value left;
      Value right;
      const Result converted_left = ToPrimitive(a, Hint::Number, left);
      if (converted_left.IsAbrupt()) {
        return converted_left;
      }
      const Result converted_right = ToPrimitive(b, Hint::Number, right);
      if (converted_right.IsAbrupt()) {
        return converted_right;
      }
      if (left.IsString() && right.IsString()) {
        const std::string& x = left.AsString();
        const std::string& y = right.AsString();
        const bool answer = op == BinaryOp::Less       ? x < y
                            : op == BinaryOp::Greater  ? x > y
                            : op == BinaryOp::LessEqual ? x <= y
                                                        : x >= y;
        return Result::Normal(Value::Bool(answer));
      }
      double x = 0;
      double y = 0;
      const Result number_left = ToNumberOf(left, x);
      if (number_left.IsAbrupt()) {
        return number_left;
      }
      const Result number_right = ToNumberOf(right, y);
      if (number_right.IsAbrupt()) {
        return number_right;
      }
      const bool answer = op == BinaryOp::Less       ? x < y
                          : op == BinaryOp::Greater  ? x > y
                          : op == BinaryOp::LessEqual ? x <= y
                                                      : x >= y;
      return Result::Normal(Value::Bool(answer));
    }

    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor: {
      std::int32_t x = 0;
      std::int32_t y = 0;
      const Result converted = bits(x, y);
      if (converted.IsAbrupt()) {
        return converted;
      }
      const std::int32_t answer = op == BinaryOp::BitAnd  ? (x & y)
                                  : op == BinaryOp::BitOr ? (x | y)
                                                          : (x ^ y);
      return Result::Normal(Value::Number(answer));
    }
    case BinaryOp::ShiftLeft:
    case BinaryOp::ShiftRight:
    case BinaryOp::ShiftRightUnsigned: {
      double lhs = 0;
      double rhs = 0;
      const Result converted = numbers(lhs, rhs);
      if (converted.IsAbrupt()) {
        return converted;
      }
      const std::uint32_t shift = ToUint32(rhs) & 31;
      if (op == BinaryOp::ShiftRightUnsigned) {
        return Result::Normal(Value::Number(ToUint32(lhs) >> shift));
      }
      // Through uint32 for the left shift: shifting a negative int32 is
      // undefined behaviour, and the value came from a page.
      if (op == BinaryOp::ShiftLeft) {
        return Result::Normal(
            Value::Number(static_cast<std::int32_t>(ToUint32(lhs) << shift)));
      }
      return Result::Normal(Value::Number(ToInt32(lhs) >> shift));
    }

    case BinaryOp::In: {
      if (!b.IsObject()) {
        return Throw("TypeError", "'in' requires an object");
      }
      // Through KeyFrom, not ToString: a Symbol key must stay a Symbol.
      // Lit's signal brand check is `SSn in getter` where SSn is a Symbol —
      // stringifying it answered false, gvU failed, and every reactive merge
      // threw Error("ad") (youtube observer callbacks / SPA watch stamp).
      const PropertyKey key = KeyFrom(a);
      if (b.object->GetKind() == Object::Kind::Proxy) {
        Value target;
        if (Object* trap = ProxyTrap(b, "has", target)) {
          const Result asked =
              CallFunction(Value::Obj(trap), Value::Undefined(), {target, KeyValue(key)});
          return asked.IsAbrupt() ? asked : Result::Normal(Value::Bool(ToBoolean(asked.value)));
        }
        // No `has` trap: the question goes straight to the target, which is
        // what makes a handler that defines nothing transparent.
        return Result::Normal(
            Value::Bool(target.IsObject() && target.object->GetProperty(key) != nullptr));
      }
      if (!key.IsSymbol() && b.object->GetKind() == Object::Kind::Array) {
        const std::string& text = key.Text();
        if (text == "length") {
          return Result::Normal(Value::Bool(true));
        }
        if (const std::optional<std::size_t> index = ParseArrayIndex(text)) {
          return Result::Normal(Value::Bool(b.object->HasElement(*index)));
        }
      }
      if (b.object->GetProperty(key) != nullptr) {
        return Result::Normal(Value::Bool(true));
      }
      // Same "one namespace, two spellings" rule GetProperty/SetProperty use on
      // the global object: builtins and `MakeInterface` constructors live as
      // scope bindings rather than own properties (so a page that assigns
      // `window.ShadowRoot = …` cannot fork the bare name from the property).
      // `'IDBTransaction' in self` must therefore see the binding too —
      // youtube's `yPS` refuses IndexedDB entirely when this answers false.
      if (!key.IsSymbol() && b.object == Global() && GlobalScope() != nullptr &&
          GlobalScope()->Lookup(key.Text()) != nullptr) {
        return Result::Normal(Value::Bool(true));
      }
      return Result::Normal(Value::Bool(false));
    }

    case BinaryOp::InstanceOf: {
      if (!b.IsObject()) {
        return Throw("TypeError", "right-hand side of 'instanceof' is not an object");
      }
      // The override comes before the callable check, because an object with
      // the trap need not be callable -- which is what lets a plain object
      // stand in for a constructor in a type test.
      if (shared_.symbol_has_instance != nullptr) {
        const Value trap =
            GetProperty(b, PropertyKey::Symbol(shared_.symbol_has_instance));
        if (trap.IsObject() && trap.object->IsCallable()) {
          const Result asked = CallFunction(trap, b, {a});
          return asked.IsAbrupt() ? asked
                                  : Result::Normal(Value::Bool(ToBoolean(asked.value)));
        }
      }
      if (!b.object->IsCallable()) {
        return Throw("TypeError", "right-hand side of 'instanceof' is not callable");
      }
      if (!a.IsObject()) {
        return Result::Normal(Value::Bool(false));
      }
      const Value* prototype = b.object->Get("prototype");
      if (prototype == nullptr || !prototype->IsObject()) {
        return Result::Normal(Value::Bool(false));
      }
      // Bounded, like every prototype walk: the chain can be a cycle.
      const Object* current = a.object->Prototype();
      for (int depth = 0; current != nullptr && depth < 1000; ++depth) {
        if (current == prototype->object) {
          return Result::Normal(Value::Bool(true));
        }
        current = current->Prototype();
      }
      return Result::Normal(Value::Bool(false));
    }
  }
  return Throw("SyntaxError", "unsupported operator");
}

}  // namespace microbrowser::js

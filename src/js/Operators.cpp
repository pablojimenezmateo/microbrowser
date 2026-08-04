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

Result Interpreter::ApplyBinary(BinaryOp op, const Value& a, const Value& b) {
  switch (op) {
    case BinaryOp::Add:
      // The one operator that is not arithmetic-only: if either side is a
      // string after conversion, it concatenates. This asymmetry is the source
      // of most surprise in the language and is written out rather than folded
      // in.
      if (a.IsString() || b.IsString()) {
        return Result::Normal(Value::String(ToString(a) + ToString(b)));
      }
      return Result::Normal(Value::Number(ToNumber(a) + ToNumber(b)));
    case BinaryOp::Subtract:
      return Result::Normal(Value::Number(ToNumber(a) - ToNumber(b)));
    case BinaryOp::Multiply:
      return Result::Normal(Value::Number(ToNumber(a) * ToNumber(b)));
    case BinaryOp::Divide:
      return Result::Normal(Value::Number(ToNumber(a) / ToNumber(b)));
    case BinaryOp::Remainder:
      return Result::Normal(Value::Number(std::fmod(ToNumber(a), ToNumber(b))));
    case BinaryOp::Exponent:
      return Result::Normal(Value::Number(std::pow(ToNumber(a), ToNumber(b))));

    case BinaryOp::LooseEqual:
      return Result::Normal(Value::Bool(LooseEquals(a, b)));
    case BinaryOp::LooseNotEqual:
      return Result::Normal(Value::Bool(!LooseEquals(a, b)));
    case BinaryOp::StrictEqual:
      return Result::Normal(Value::Bool(StrictEquals(a, b)));
    case BinaryOp::StrictNotEqual:
      return Result::Normal(Value::Bool(!StrictEquals(a, b)));

    case BinaryOp::Less:
    case BinaryOp::Greater:
    case BinaryOp::LessEqual:
    case BinaryOp::GreaterEqual: {
      // Two strings compare lexicographically; anything else compares as
      // numbers, and a NaN on either side makes every comparison false.
      if (a.IsString() && b.IsString()) {
        const std::string& x = a.AsString();
        const std::string& y = b.AsString();
        const bool answer = op == BinaryOp::Less       ? x < y
                            : op == BinaryOp::Greater  ? x > y
                            : op == BinaryOp::LessEqual ? x <= y
                                                        : x >= y;
        return Result::Normal(Value::Bool(answer));
      }
      const double x = ToNumber(a);
      const double y = ToNumber(b);
      const bool answer = op == BinaryOp::Less       ? x < y
                          : op == BinaryOp::Greater  ? x > y
                          : op == BinaryOp::LessEqual ? x <= y
                                                      : x >= y;
      return Result::Normal(Value::Bool(answer));
    }

    case BinaryOp::BitAnd:
      return Result::Normal(Value::Number(ToInt32(ToNumber(a)) & ToInt32(ToNumber(b))));
    case BinaryOp::BitOr:
      return Result::Normal(Value::Number(ToInt32(ToNumber(a)) | ToInt32(ToNumber(b))));
    case BinaryOp::BitXor:
      return Result::Normal(Value::Number(ToInt32(ToNumber(a)) ^ ToInt32(ToNumber(b))));
    case BinaryOp::ShiftLeft:
      return Result::Normal(
          Value::Number(ToInt32(ToNumber(a)) << (ToUint32(ToNumber(b)) & 31)));
    case BinaryOp::ShiftRight:
      return Result::Normal(
          Value::Number(ToInt32(ToNumber(a)) >> (ToUint32(ToNumber(b)) & 31)));
    case BinaryOp::ShiftRightUnsigned:
      return Result::Normal(
          Value::Number(ToUint32(ToNumber(a)) >> (ToUint32(ToNumber(b)) & 31)));

    case BinaryOp::In: {
      if (!b.IsObject()) {
        return Throw("TypeError", "'in' requires an object");
      }
      if (b.object->GetKind() == Object::Kind::Proxy) {
        Value target;
        if (Object* trap = ProxyTrap(b, "has", target)) {
          const Result asked = CallFunction(Value::Obj(trap), Value::Undefined(), {target, a});
          return asked.IsAbrupt() ? asked : Result::Normal(Value::Bool(ToBoolean(asked.value)));
        }
        // No `has` trap: the question goes straight to the target, which is
        // what makes a handler that defines nothing transparent.
        return Result::Normal(
            Value::Bool(target.IsObject() && target.object->GetProperty(KeyFrom(a)) != nullptr));
      }
      const std::string key = ToString(a);
      if (b.object->GetKind() == Object::Kind::Array) {
        if (key == "length") {
          return Result::Normal(Value::Bool(true));
        }
        if (const std::optional<std::size_t> index = ParseArrayIndex(key)) {
          return Result::Normal(Value::Bool(b.object->HasElement(*index)));
        }
      }
      return Result::Normal(Value::Bool(b.object->GetProperty(key) != nullptr));
    }

    case BinaryOp::InstanceOf: {
      if (!b.IsObject() || !b.object->IsCallable()) {
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

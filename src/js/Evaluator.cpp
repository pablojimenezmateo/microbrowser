#include <cmath>
#include <optional>
#include <utility>

#include "js/Interpreter.h"

namespace microbrowser::js {


void Interpreter::HoistDeclarations(const Node& list, Environment& scope) {
  // Function declarations are visible before the line that writes them, which
  // is what makes mutually recursive functions work without forward
  // declarations. `var` hoisting is deliberately absent: it is a source of
  // bugs the language itself moved away from, and nothing here needs it.
  for (const NodePtr& statement : list.children) {
    if (statement != nullptr && statement->kind == NodeKind::FunctionDeclaration) {
      scope.Declare(statement->string, NewFunction(*statement, scope, false), false);
    }
  }
}

Result Interpreter::BindPattern(const Node& target, const Value& value, Environment& scope,
                                bool declare, bool is_const) {
  switch (target.kind) {
    case NodeKind::Identifier:
      if (declare) {
        scope.Declare(target.string, value, is_const);
        return Result::Normal(value);
      }
      if (!scope.Assign(target.string, value)) {
        if (scope.Lookup(target.string) != nullptr) {
          return Throw("TypeError", "assignment to constant variable '" + target.string + "'");
        }
        // An assignment to an undeclared name creates a global. Sloppy mode,
        // and the web depends on it. On the global *object*, so that
        // `globalThis.x` and `x` name the same thing.
        global_->Set(target.string, value);
      }
      return Result::Normal(value);

    case NodeKind::ArrayLiteral: {
      // Array destructuring. A hole skips an element rather than binding it.
      for (std::size_t i = 0; i < target.children.size(); ++i) {
        const Node* element = target.Child(i);
        if (element == nullptr) {
          continue;
        }
        if (element->kind == NodeKind::Spread || element->kind == NodeKind::RestElement) {
          std::vector<Value> rest;
          if (value.IsObject() && value.object->GetKind() == Object::Kind::Array) {
            for (std::size_t j = i; j < value.object->ElementCount(); ++j) {
              rest.push_back(value.object->GetElement(j));
            }
          }
          const Node* inner = element->Child(0);
          if (inner != nullptr) {
            Object* rest_array = NewArray(std::move(rest));
            if (rest_array == nullptr) {
              return Throw("RangeError", "out of memory");
            }
            const Result bound =
                BindPattern(*inner, Value::Obj(rest_array), scope, declare, is_const);
            if (bound.IsAbrupt()) {
              return bound;
            }
          }
          break;
        }
        const Value item = GetProperty(value, std::to_string(i));
        const Result bound = BindPattern(*element, item, scope, declare, is_const);
        if (bound.IsAbrupt()) {
          return bound;
        }
      }
      return Result::Normal(value);
    }

    case NodeKind::ObjectLiteral: {
      for (const NodePtr& property : target.children) {
        if (property == nullptr || property->kind != NodeKind::Property) {
          continue;
        }
        const Value item = GetProperty(value, property->string);
        const Node* binding = property->Child(0);
        if (binding != nullptr) {
          const Result bound = BindPattern(*binding, item, scope, declare, is_const);
          if (bound.IsAbrupt()) {
            return bound;
          }
        }
      }
      return Result::Normal(value);
    }

    case NodeKind::AssignmentPattern: {
      Value bound_value = value;
      if (bound_value.IsUndefined() && target.Child(1) != nullptr) {
        const Result fallback = Evaluate(*target.Child(1), scope);
        if (fallback.IsAbrupt()) {
          return fallback;
        }
        bound_value = fallback.value;
      }
      const Node* inner = target.Child(0);
      return inner == nullptr ? Result::Normal(bound_value)
                              : BindPattern(*inner, bound_value, scope, declare, is_const);
    }

    case NodeKind::Member: {
      // `obj.x = v` reaching here means a destructuring assignment wrote to a
      // property rather than to a binding.
      Value base;
      const Result member = EvaluateMember(target, scope, base);
      if (member.IsAbrupt()) {
        return member;
      }
      return SetProperty(base, ToString(member.value), value);
    }

    default:
      return Throw("SyntaxError", "invalid assignment target");
  }
}

Result Interpreter::EvaluateMember(const Node& node, Environment& scope, Value& base_out) {
  const Node* object_node = node.Child(0);
  const Node* property_node = node.Child(1);
  if (object_node == nullptr || property_node == nullptr) {
    return Throw("SyntaxError", "malformed member access");
  }

  if (object_node->kind == NodeKind::Super) {
    // `super.x` reads from the prototype of the object the *method* was
    // defined on, while `this` stays the receiver. Reading from the receiver's
    // prototype instead is what makes a three-level hierarchy call itself.
    Value* home = scope.Lookup("__home__");
    Value* self = scope.Lookup("this");
    base_out = self == nullptr ? Value::Undefined() : *self;
    if (home == nullptr || !home->IsObject() || home->object->Prototype() == nullptr) {
      return Throw("SyntaxError", "'super' is only valid inside a method");
    }
    super_base_ = Value::Obj(home->object->Prototype());
    if (node.number == 1.0) {
      const Result key = Evaluate(*property_node, scope);
      if (key.IsAbrupt()) {
        return key;
      }
      return Result::Normal(Value::String(ToString(key.value)));
    }
    return Result::Normal(Value::String(property_node->string));
  }
  super_base_ = Value::Undefined();

  const Result base = Evaluate(*object_node, scope);
  if (base.IsAbrupt()) {
    return base;
  }
  base_out = base.value;

  const bool computed = node.number == 1.0;
  if (!computed) {
    return Result::Normal(Value::String(property_node->string));
  }
  const Result key = Evaluate(*property_node, scope);
  if (key.IsAbrupt()) {
    return key;
  }
  return Result::Normal(Value::String(ToString(key.value)));
}

Result Interpreter::EvaluateBinary(const Node& node, Environment& scope) {
  const Node* left_node = node.Child(0);
  const Node* right_node = node.Child(1);
  if (left_node == nullptr || right_node == nullptr) {
    return Throw("SyntaxError", "malformed binary expression");
  }

  const Result left = Evaluate(*left_node, scope);
  if (left.IsAbrupt()) {
    return left;
  }
  const Result right = Evaluate(*right_node, scope);
  if (right.IsAbrupt()) {
    return right;
  }

  const std::string& op = node.string;
  const Value& a = left.value;
  const Value& b = right.value;

  if (op == "+") {
    // The one operator that is not arithmetic-only: if either side is a string
    // after conversion, it concatenates. This asymmetry is the source of most
    // surprise in the language and is written out rather than folded in.
    if (a.IsString() || b.IsString()) {
      return Result::Normal(Value::String(ToString(a) + ToString(b)));
    }
    return Result::Normal(Value::Number(ToNumber(a) + ToNumber(b)));
  }
  if (op == "-") return Result::Normal(Value::Number(ToNumber(a) - ToNumber(b)));
  if (op == "*") return Result::Normal(Value::Number(ToNumber(a) * ToNumber(b)));
  if (op == "/") return Result::Normal(Value::Number(ToNumber(a) / ToNumber(b)));
  if (op == "%") return Result::Normal(Value::Number(std::fmod(ToNumber(a), ToNumber(b))));
  if (op == "**") return Result::Normal(Value::Number(std::pow(ToNumber(a), ToNumber(b))));

  if (op == "==") return Result::Normal(Value::Bool(LooseEquals(a, b)));
  if (op == "!=") return Result::Normal(Value::Bool(!LooseEquals(a, b)));
  if (op == "===") return Result::Normal(Value::Bool(StrictEquals(a, b)));
  if (op == "!==") return Result::Normal(Value::Bool(!StrictEquals(a, b)));

  if (op == "<" || op == ">" || op == "<=" || op == ">=") {
    // Two strings compare lexicographically; anything else compares as
    // numbers, and a NaN on either side makes every comparison false.
    if (a.IsString() && b.IsString()) {
      const std::string& x = a.AsString();
      const std::string& y = b.AsString();
      if (op == "<") return Result::Normal(Value::Bool(x < y));
      if (op == ">") return Result::Normal(Value::Bool(x > y));
      if (op == "<=") return Result::Normal(Value::Bool(x <= y));
      return Result::Normal(Value::Bool(x >= y));
    }
    const double x = ToNumber(a);
    const double y = ToNumber(b);
    if (op == "<") return Result::Normal(Value::Bool(x < y));
    if (op == ">") return Result::Normal(Value::Bool(x > y));
    if (op == "<=") return Result::Normal(Value::Bool(x <= y));
    return Result::Normal(Value::Bool(x >= y));
  }

  if (op == "&") return Result::Normal(Value::Number(ToInt32(ToNumber(a)) & ToInt32(ToNumber(b))));
  if (op == "|") return Result::Normal(Value::Number(ToInt32(ToNumber(a)) | ToInt32(ToNumber(b))));
  if (op == "^") return Result::Normal(Value::Number(ToInt32(ToNumber(a)) ^ ToInt32(ToNumber(b))));
  if (op == "<<") {
    return Result::Normal(
        Value::Number(ToInt32(ToNumber(a)) << (ToUint32(ToNumber(b)) & 31)));
  }
  if (op == ">>") {
    return Result::Normal(
        Value::Number(ToInt32(ToNumber(a)) >> (ToUint32(ToNumber(b)) & 31)));
  }
  if (op == ">>>") {
    return Result::Normal(
        Value::Number(ToUint32(ToNumber(a)) >> (ToUint32(ToNumber(b)) & 31)));
  }

  if (op == "in") {
    if (!b.IsObject()) {
      return Throw("TypeError", "'in' requires an object");
    }
    const std::string key = ToString(a);
    if (b.object->GetKind() == Object::Kind::Array) {
      if (const std::optional<std::size_t> index = ParseArrayIndex(key)) {
        return Result::Normal(Value::Bool(b.object->HasElement(*index)));
      }
    }
    return Result::Normal(Value::Bool(b.object->Get(key) != nullptr));
  }
  if (op == "instanceof") {
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

  return Throw("SyntaxError", "unsupported operator '" + op + "'");
}

Result Interpreter::EvaluateAssignment(const Node& node, Environment& scope) {
  const Node* target = node.Child(0);
  const Node* value_node = node.Child(1);
  if (target == nullptr || value_node == nullptr) {
    return Throw("SyntaxError", "malformed assignment");
  }

  const std::string& op = node.string;
  if (op == "&&=" || op == "||=" || op == "?\?=") {
    // Short-circuiting assignment does not evaluate the right side, and does
    // not assign at all, when the test fails. Assigning the old value back
    // would be observable through a setter.
    const Result current = Evaluate(*target, scope);
    if (current.IsAbrupt()) {
      return current;
    }
    const bool should_assign = op == "&&=" ? ToBoolean(current.value)
                               : op == "||=" ? !ToBoolean(current.value)
                                             : current.value.IsNullish();
    if (!should_assign) {
      return current;
    }
    const Result value = Evaluate(*value_node, scope);
    if (value.IsAbrupt()) {
      return value;
    }
    return BindPattern(*target, value.value, scope, false, false);
  }

  Result value = Evaluate(*value_node, scope);
  if (value.IsAbrupt()) {
    return value;
  }

  if (op != "=") {
    const Result current = Evaluate(*target, scope);
    if (current.IsAbrupt()) {
      return current;
    }
    Node synthetic;
    synthetic.kind = NodeKind::Binary;
    synthetic.string = op.substr(0, op.size() - 1);
    // A compound assignment is defined as the binary operator applied to the
    // already-evaluated operands. Rebuilding a node to reuse EvaluateBinary
    // keeps one implementation of `+` rather than two that can disagree.
    auto left = std::make_unique<Node>();
    left->kind = NodeKind::NumberLiteral;
    auto right = std::make_unique<Node>();
    right->kind = NodeKind::NumberLiteral;
    synthetic.children.push_back(std::move(left));
    synthetic.children.push_back(std::move(right));

    Environment* temporary = heap_.AllocateEnvironment(&scope);

    if (temporary == nullptr) {

      return Throw("RangeError", "out of memory");

    }
    const ScopeGuard guard(*this, temporary);
    temporary->Declare("__lhs", current.value, false);
    temporary->Declare("__rhs", value.value, false);
    synthetic.children[0]->kind = NodeKind::Identifier;
    synthetic.children[0]->string = "__lhs";
    synthetic.children[1]->kind = NodeKind::Identifier;
    synthetic.children[1]->string = "__rhs";
    value = EvaluateBinary(synthetic, *temporary);
    if (value.IsAbrupt()) {
      return value;
    }
  }

  if (target->kind == NodeKind::Member) {
    Value base;
    const Result key = EvaluateMember(*target, scope, base);
    if (key.IsAbrupt()) {
      return key;
    }
    const Result stored = SetProperty(base, ToString(key.value), value.value);
    if (stored.IsAbrupt()) {
      return stored;
    }
    return Result::Normal(value.value);
  }
  return BindPattern(*target, value.value, scope, false, false);
}

Result Interpreter::EvaluateCall(const Node& node, Environment& scope) {
  const Node* callee_node = node.Child(0);
  if (callee_node == nullptr) {
    return Throw("SyntaxError", "malformed call");
  }

  if (callee_node->kind == NodeKind::Super) {
    // `super(...)` runs the parent constructor against the instance that is
    // already being built, rather than making a second one.
    Value* current_function = scope.Lookup("__function__");
    Value* self_binding = scope.Lookup("this");
    if (current_function == nullptr || !current_function->IsObject() ||
        current_function->object->SuperConstructor() == nullptr) {
      return Throw("SyntaxError", "'super' keyword unexpected here");
    }
    std::vector<Value> super_arguments;
    for (std::size_t i = 1; i < node.children.size(); ++i) {
      const Node* argument = node.Child(i);
      if (argument == nullptr) {
        continue;
      }
      const Result value = Evaluate(*argument, scope);
      if (value.IsAbrupt()) {
        return value;
      }
      super_arguments.push_back(value.value);
    }
    const Value instance = self_binding == nullptr ? Value::Undefined() : *self_binding;
    const Result constructed = CallFunction(
        Value::Obj(current_function->object->SuperConstructor()), instance, super_arguments);
    if (constructed.IsAbrupt()) {
      return constructed;
    }
    // Fields of *this* class initialize after the super call, which is the
    // ordering that makes a derived field see a base one.
    if (instance.IsObject()) {
      const Result fields = InitializeFields(instance.object, current_function->object);
      if (fields.IsAbrupt()) {
        return fields;
      }
    }
    return Result::Normal(Value::Undefined());
  }

  Value self;
  Result callee;
  if (callee_node->kind == NodeKind::Member) {
    // A method call's `this` is the object it was read from, and that object
    // must be evaluated once -- `f().g()` calls f once.
    Value base;
    const Result key = EvaluateMember(*callee_node, scope, base);
    if (key.IsAbrupt()) {
      return key;
    }
    if (callee_node->number == 2.0 && base.IsNullish()) {
      return Result::Normal(Value::Undefined());  // optional chaining
    }
    self = base;
    // For `super.m()`, the lookup happens on the parent prototype and `this`
    // stays the receiver -- which is the whole point of the two being separate.
    const Value lookup_base = super_base_.IsObject() ? super_base_ : base;
    super_base_ = Value::Undefined();
    callee = Result::Normal(GetProperty(lookup_base, ToString(key.value)));
  } else {
    callee = Evaluate(*callee_node, scope);
  }
  if (callee.IsAbrupt()) {
    return callee;
  }
  if (node.number == 1.0 && callee.value.IsNullish()) {
    return Result::Normal(Value::Undefined());  // `f?.()`
  }

  std::vector<Value> arguments;
  for (std::size_t i = 1; i < node.children.size(); ++i) {
    const Node* argument = node.Child(i);
    if (argument == nullptr) {
      continue;
    }
    if (argument->kind == NodeKind::Spread) {
      const Node* inner = argument->Child(0);
      if (inner == nullptr) {
        continue;
      }
      const Result spread = Evaluate(*inner, scope);
      if (spread.IsAbrupt()) {
        return spread;
      }
      if (spread.value.IsObject() && spread.value.object->GetKind() == Object::Kind::Array) {
        for (std::size_t j = 0; j < spread.value.object->ElementCount(); ++j) {
          arguments.push_back(spread.value.object->GetElement(j));
        }
      }
      continue;
    }
    const Result value = Evaluate(*argument, scope);
    if (value.IsAbrupt()) {
      return value;
    }
    arguments.push_back(value.value);
  }
  return CallFunction(callee.value, self, arguments);
}

}  // namespace microbrowser::js

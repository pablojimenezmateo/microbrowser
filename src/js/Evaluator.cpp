#include <cmath>
#include <optional>
#include <utility>

#include "js/BuiltinSupport.h"
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
      // Array destructuring, over the iteration protocol rather than by index.
      // `const [a, b] = new Set(xs)` is the same syntax as `const [a, b] = xs`
      // and reading indices would give undefined for one of them.
      //
      // One value taken per element, not a drained vector: the pattern is
      // allowed to be shorter than the iterable, and stepping only as far as
      // it needs is the difference between destructuring an endless iterator
      // and hanging on one.
      Iteration cursor;
      const Result opened = OpenIteration(value, cursor);
      if (opened.IsAbrupt()) {
        return opened;
      }
      bool exhausted = false;
      for (std::size_t i = 0; i < target.children.size(); ++i) {
        const Node* element = target.Child(i);
        if (element != nullptr &&
            (element->kind == NodeKind::Spread || element->kind == NodeKind::RestElement)) {
          std::vector<Value> rest;
          while (!exhausted) {
            Value item;
            const Result stepped = StepIteration(cursor, item, exhausted);
            if (stepped.IsAbrupt()) {
              return stepped;
            }
            if (exhausted) {
              break;
            }
            if (rest.size() >= kMaxAllocationLength) {
              return Throw("RangeError", "too many values to destructure");
            }
            rest.push_back(std::move(item));
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
        // A hole still consumes a value -- `const [, b] = xs` binds the
        // second, which it can only do by stepping past the first.
        Value item;
        if (!exhausted) {
          const Result stepped = StepIteration(cursor, item, exhausted);
          if (stepped.IsAbrupt()) {
            return stepped;
          }
          if (exhausted) {
            item = Value::Undefined();
          }
        }
        if (element == nullptr) {
          continue;
        }
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
      return SetProperty(base, KeyFrom(member.value), value);
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
      return key;  // a symbol key stays a symbol; see the note below
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
  // Returned as it was evaluated rather than as a string. Stringifying here is
  // what would make `o[Symbol.iterator]` the property named "Symbol(Symbol
  // .iterator)" -- a name a page can write out, which is exactly what a symbol
  // key must not be. Callers turn it into a key with KeyFrom.
  return key;
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

  BinaryOp op = BinaryOp::Add;
  if (!ParseBinaryOp(node.string, op)) {
    return Throw("SyntaxError", "unsupported operator '" + node.string + "'");
  }
  return ApplyBinary(op, left.value, right.value);
}

Result Interpreter::EvaluateAssignment(const Node& node, Environment& scope) {
  const Node* target = node.Child(0);
  const Node* value_node = node.Child(1);
  if (target == nullptr || value_node == nullptr) {
    return Throw("SyntaxError", "malformed assignment");
  }

  const std::string& op = node.string;
  const bool is_logical = op == "&&=" || op == "||=" || op == "?\?=";

  if (target->kind == NodeKind::Member) {
    // A member target's own operands are evaluated *first*, before the
    // right-hand side, and exactly once.
    //
    // Both halves of that were wrong here, and the compiler is what found it:
    // `a[i++] = i` read the right side first and stored under the wrong index,
    // and `a[i++] ||= 1` evaluated the subscript a second time to store
    // through it, so `i` moved twice. The spec's order is the one the machine
    // emits, and both engines answer the same now.
    Value base;
    const Result key = EvaluateMember(*target, scope, base);
    if (key.IsAbrupt()) {
      return key;
    }
    const PropertyKey property = KeyFrom(key.value);
    // The receiver has to outlive the right-hand side, which can run anything.
    if (base.IsObject()) {
      active_objects_.push_back(base.object);
    }
    struct BaseRoot {
      Interpreter& interpreter;
      bool held;
      ~BaseRoot() {
        if (held) {
          interpreter.active_objects_.pop_back();
        }
      }
    } root{*this, base.IsObject()};

    Value current;
    if (op != "=") {
      current = GetProperty(base, property);
      if (is_logical) {
        // Short-circuiting assignment does not evaluate the right side, and
        // does not assign at all, when the test fails. Assigning the old value
        // back would be observable through a setter.
        const bool should_assign = op == "&&=" ? ToBoolean(current)
                                   : op == "||=" ? !ToBoolean(current)
                                                 : current.IsNullish();
        if (!should_assign) {
          return Result::Normal(current);
        }
      }
    }
    Result value = Evaluate(*value_node, scope);
    if (value.IsAbrupt()) {
      return value;
    }
    if (op != "=" && !is_logical) {
      BinaryOp binary = BinaryOp::Add;
      if (!ParseBinaryOp(std::string_view(op).substr(0, op.size() - 1), binary)) {
        return Throw("SyntaxError", "unsupported operator '" + op + "'");
      }
      value = ApplyBinary(binary, current, value.value);
      if (value.IsAbrupt()) {
        return value;
      }
    }
    const Result stored = SetProperty(base, property, value.value);
    if (stored.IsAbrupt()) {
      return stored;
    }
    return Result::Normal(value.value);
  }

  if (is_logical) {
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
    // A compound assignment is defined as the binary operator applied to the
    // already-evaluated operands. This used to build a synthetic Binary node
    // over two temporary bindings so that one implementation of `+` served
    // both; ApplyBinary is that implementation, reachable directly, so the
    // scaffolding went with it.
    BinaryOp binary = BinaryOp::Add;
    if (!ParseBinaryOp(std::string_view(op).substr(0, op.size() - 1), binary)) {
      return Throw("SyntaxError", "unsupported operator '" + op + "'");
    }
    value = ApplyBinary(binary, current.value, value.value);
    if (value.IsAbrupt()) {
      return value;
    }
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
    callee = Result::Normal(GetProperty(lookup_base, KeyFrom(key.value)));
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
      // Any iterable, not only an array: `f(...new Set(xs))` and
      // `f(...'abc')` are both ordinary calls.
      const Result collected = CollectIterable(spread.value, arguments);
      if (collected.IsAbrupt()) {
        return collected;
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

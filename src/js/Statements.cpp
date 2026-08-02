#include <cmath>
#include <utility>

#include "js/Interpreter.h"

namespace microbrowser::js {

// Counts evaluation depth for as long as it is in scope, so every early return
// below decrements it exactly once.
namespace {

class DepthCounter {
 public:
  DepthCounter(int& depth, int limit) : depth_(depth), exceeded_(++depth > limit) {}
  ~DepthCounter() { --depth_; }
  DepthCounter(const DepthCounter&) = delete;
  DepthCounter& operator=(const DepthCounter&) = delete;
  bool Exceeded() const { return exceeded_; }

 private:
  int& depth_;
  bool exceeded_;
};

}  // namespace

Result Interpreter::Evaluate(const Node& node, Environment& scope) {
  const DepthCounter depth(eval_depth_, kMaxEvalDepth);
  if (depth.Exceeded()) {
    return Throw("RangeError", "maximum call stack size exceeded");
  }
  if (++steps_ > kMaxSteps) {
    // A page can write `while (true) {}`. A step budget makes that a thrown
    // error rather than a hung browser, which is the only difference a user
    // would notice between the two.
    return Throw("RangeError", "script ran too long");
  }

  switch (node.kind) {
    case NodeKind::NumberLiteral:
      return Result::Normal(Value::Number(node.number));
    case NodeKind::StringLiteral:
      return Result::Normal(Value::String(node.string));
    case NodeKind::BooleanLiteral:
      return Result::Normal(Value::Bool(node.number != 0.0));
    case NodeKind::NullLiteral:
      return Result::Normal(Value::Null());
    case NodeKind::RegExpLiteral:
      // No regular expression engine yet. A string keeps the program running
      // rather than failing at parse time on a feature that is not there.
      return Result::Normal(Value::String(node.string));

    case NodeKind::TemplateLiteral: {
      // The raw source with its substitutions replaced by their values. The
      // parser kept the substitution expressions in order, which is what makes
      // this a walk rather than a re-parse.
      std::string out;
      std::size_t child = 0;
      const std::string& raw = node.string;
      for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
          out.push_back(raw[i + 1]);
          ++i;
          continue;
        }
        if (raw[i] == '$' && raw[i + 1] == '{') {
          int braces = 1;
          std::size_t j = i + 2;
          for (; j < raw.size() && braces > 0; ++j) {
            if (raw[j] == '{') ++braces;
            else if (raw[j] == '}') --braces;
          }
          if (child < node.children.size() && node.Child(child) != nullptr) {
            const Node* expression = node.Child(child);
            // The parser wrapped each substitution in a statement.
            const Node* inner =
                expression->kind == NodeKind::ExpressionStatement ? expression->Child(0)
                                                                  : expression;
            if (inner != nullptr) {
              const Result value = Evaluate(*inner, scope);
              if (value.IsAbrupt()) {
                return value;
              }
              out += ToString(value.value);
            }
          }
          ++child;
          i = j - 1;
          continue;
        }
        out.push_back(raw[i]);
      }
      return Result::Normal(Value::String(std::move(out)));
    }

    case NodeKind::Identifier: {
      if (node.string == "undefined") {
        return Result::Normal(Value::Undefined());
      }
      Value* binding = scope.Lookup(node.string);
      if (binding != nullptr) {
        return Result::Normal(*binding);
      }
      // A property of the global object is also a global variable: `globalThis.x
      // = 1` makes `x` readable, and that is one namespace rather than two that
      // happen to overlap.
      if (const Value* property = global_->GetOwn(node.string)) {
        return Result::Normal(*property);
      }
      // Not undefined: a name that was never declared is a ReferenceError, and
      // the distinction is the language's way of catching a typo.
      return Throw("ReferenceError", node.string + " is not defined");
    }

    case NodeKind::ThisExpression: {
      Value* binding = scope.Lookup("this");
      return Result::Normal(binding == nullptr ? Value::Undefined() : *binding);
    }

    case NodeKind::ArrayLiteral: {
      std::vector<Value> elements;
      std::vector<bool> present;
      for (const NodePtr& element : node.children) {
        if (element == nullptr) {
          elements.push_back(Value::Undefined());  // a hole reads as undefined
          present.push_back(false);
          continue;
        }
        if (element->kind == NodeKind::Spread) {
          const Node* inner = element->Child(0);
          if (inner == nullptr) {
            continue;
          }
          const Result spread = Evaluate(*inner, scope);
          if (spread.IsAbrupt()) {
            return spread;
          }
          if (spread.value.IsObject() &&
              spread.value.object->GetKind() == Object::Kind::Array) {
            for (std::size_t i = 0; i < spread.value.object->ElementCount(); ++i) {
              elements.push_back(spread.value.object->GetElement(i));
              present.push_back(true);
            }
          }
          continue;
        }
        const Result value = Evaluate(*element, scope);
        if (value.IsAbrupt()) {
          return value;
        }
        elements.push_back(value.value);
        present.push_back(true);
      }
      Object* array = NewArray(std::move(elements), std::move(present));
      if (array == nullptr) {
        return Throw("RangeError", "out of memory");
      }
      return Result::Normal(Value::Obj(array));
    }

    case NodeKind::ObjectLiteral: {
      Object* object = NewObject();
      if (object == nullptr) {
        return Throw("RangeError", "out of memory");
      }
      const Value result = Value::Obj(object);
      for (const NodePtr& property : node.children) {
        if (property == nullptr) {
          continue;
        }
        if (property->kind == NodeKind::Spread) {
          const Node* inner = property->Child(0);
          if (inner == nullptr) {
            continue;
          }
          const Result spread = Evaluate(*inner, scope);
          if (spread.IsAbrupt()) {
            return spread;
          }
          if (spread.value.IsObject()) {
            for (const std::string& key : spread.value.object->Keys()) {
              object->Set(key, GetProperty(spread.value, key));
            }
          }
          continue;
        }
        std::string key = property->string;
        if (property->number == 1.0 && property->Child(1) != nullptr) {
          const Result computed = Evaluate(*property->Child(1), scope);
          if (computed.IsAbrupt()) {
            return computed;
          }
          key = ToString(computed.value);
        }
        const Node* value_node = property->Child(0);
        if (value_node == nullptr) {
          continue;
        }
        const Result value = Evaluate(*value_node, scope);
        if (value.IsAbrupt()) {
          return value;
        }
        object->Set(std::move(key), value.value);
      }
      return Result::Normal(result);
    }

    case NodeKind::FunctionExpression:
      return Result::Normal(NewFunction(node, scope, false));

    case NodeKind::ArrowFunction: {
      Value function = NewFunction(node, scope, true);
      Value* self = scope.Lookup("this");
      // Captured now, not at call time. That is the entire semantic difference
      // between an arrow and a function expression.
      function.object->SetBoundThis(self == nullptr ? Value::Undefined() : *self);
      return Result::Normal(function);
    }

    case NodeKind::Unary: {
      const Node* operand = node.Child(0);
      if (operand == nullptr) {
        return Throw("SyntaxError", "malformed unary expression");
      }
      if (node.string == "typeof" && operand->kind == NodeKind::Identifier &&
          scope.Lookup(operand->string) == nullptr &&
          global_->GetOwn(operand->string) == nullptr) {
        // `typeof undeclared` is "undefined" rather than a ReferenceError. The
        // one place the language lets a name be read without being declared,
        // and the reason feature detection works.
        return Result::Normal(Value::String("undefined"));
      }
      if (node.string == "delete") {
        if (operand->kind == NodeKind::Member) {
          Value base;
          const Result key = EvaluateMember(*operand, scope, base);
          if (key.IsAbrupt()) {
            return key;
          }
          if (base.IsObject()) {
            return Result::Normal(Value::Bool(base.object->Delete(ToString(key.value))));
          }
        }
        return Result::Normal(Value::Bool(true));
      }

      const Result value = Evaluate(*operand, scope);
      if (value.IsAbrupt()) {
        return value;
      }
      if (node.string == "!") return Result::Normal(Value::Bool(!ToBoolean(value.value)));
      if (node.string == "-") return Result::Normal(Value::Number(-ToNumber(value.value)));
      if (node.string == "+") return Result::Normal(Value::Number(ToNumber(value.value)));
      if (node.string == "~") {
        return Result::Normal(Value::Number(~ToInt32(ToNumber(value.value))));
      }
      if (node.string == "typeof") {
        return Result::Normal(Value::String(std::string(TypeOf(value.value))));
      }
      if (node.string == "void") return Result::Normal(Value::Undefined());
      return Throw("SyntaxError", "unsupported unary operator '" + node.string + "'");
    }

    case NodeKind::Update: {
      const Node* operand = node.Child(0);
      if (operand == nullptr) {
        return Throw("SyntaxError", "malformed update expression");
      }
      const Result current = Evaluate(*operand, scope);
      if (current.IsAbrupt()) {
        return current;
      }
      const double before = ToNumber(current.value);
      const double after = node.string == "++" ? before + 1 : before - 1;

      if (operand->kind == NodeKind::Member) {
        Value base;
        const Result key = EvaluateMember(*operand, scope, base);
        if (key.IsAbrupt()) {
          return key;
        }
        const Result stored = SetProperty(base, ToString(key.value), Value::Number(after));
        if (stored.IsAbrupt()) {
          return stored;
        }
      } else {
        const Result bound = BindPattern(*operand, Value::Number(after), scope, false, false);
        if (bound.IsAbrupt()) {
          return bound;
        }
      }
      // Prefix yields the new value, postfix the old one.
      return Result::Normal(Value::Number(node.number == 1.0 ? after : before));
    }

    case NodeKind::Binary:
      return EvaluateBinary(node, scope);

    case NodeKind::Logical: {
      const Node* left_node = node.Child(0);
      const Node* right_node = node.Child(1);
      if (left_node == nullptr || right_node == nullptr) {
        return Throw("SyntaxError", "malformed logical expression");
      }
      const Result left = Evaluate(*left_node, scope);
      if (left.IsAbrupt()) {
        return left;
      }
      // Short-circuiting, and the result is one of the operands rather than a
      // boolean -- which is what makes `a || 'default'` idiomatic.
      const bool take_right = node.string == "&&"   ? ToBoolean(left.value)
                              : node.string == "||" ? !ToBoolean(left.value)
                                                    : left.value.IsNullish();
      return take_right ? Evaluate(*right_node, scope) : left;
    }

    case NodeKind::Assignment:
      return EvaluateAssignment(node, scope);

    case NodeKind::Conditional: {
      const Node* test = node.Child(0);
      if (test == nullptr) {
        return Throw("SyntaxError", "malformed conditional");
      }
      const Result condition = Evaluate(*test, scope);
      if (condition.IsAbrupt()) {
        return condition;
      }
      const Node* branch = ToBoolean(condition.value) ? node.Child(1) : node.Child(2);
      return branch == nullptr ? Result::Normal() : Evaluate(*branch, scope);
    }

    case NodeKind::Call:
      return EvaluateCall(node, scope);

    case NodeKind::New: {
      const Node* callee_node = node.Child(0);
      if (callee_node == nullptr) {
        return Throw("SyntaxError", "malformed new expression");
      }
      const Result callee = Evaluate(*callee_node, scope);
      if (callee.IsAbrupt()) {
        return callee;
      }
      if (!callee.value.IsObject() || !callee.value.object->IsCallable()) {
        return Throw("TypeError", ToString(callee.value) + " is not a constructor");
      }
      std::vector<Value> arguments;
      for (std::size_t i = 1; i < node.children.size(); ++i) {
        const Node* argument = node.Child(i);
        if (argument == nullptr) {
          continue;
        }
        const Result value = Evaluate(*argument, scope);
        if (value.IsAbrupt()) {
          return value;
        }
        arguments.push_back(value.value);
      }

      Object* instance = NewObject();
      if (instance == nullptr) {
        return Throw("RangeError", "out of memory");
      }
      const Value* prototype = callee.value.object->Get("prototype");
      if (prototype != nullptr && prototype->IsObject()) {
        instance->SetPrototype(prototype->object);
      }
      const Value self = Value::Obj(instance);
      active_objects_.push_back(instance);
      // A base class initializes its fields before the constructor body runs.
      // A derived one does it after its super() call instead, which is the
      // ordering that lets a derived field read a base one.
      Object* parent = callee.value.object->SuperConstructor();
      if (parent == nullptr) {
        const Result fields = InitializeFields(instance, callee.value.object);
        if (fields.IsAbrupt()) {
          active_objects_.pop_back();
          return fields;
        }
      } else if (callee.value.object->Body() == nullptr) {
        // A derived class with no explicit constructor gets an implicit
        // `constructor(...args){ super(...args) }`. Without it, `class B
        // extends A { n = 5 }` runs no constructor at all and leaves both the
        // base's state and its own fields unset.
        const Result base = CallFunction(Value::Obj(parent), self, arguments);
        if (base.IsAbrupt()) {
          active_objects_.pop_back();
          return base;
        }
        const Result fields = InitializeFields(instance, callee.value.object);
        if (fields.IsAbrupt()) {
          active_objects_.pop_back();
          return fields;
        }
      }
      const Result constructed = CallFunction(callee.value, self, arguments);
      active_objects_.pop_back();
      if (constructed.IsAbrupt()) {
        return constructed;
      }
      // A constructor returning an object replaces the instance; returning a
      // primitive does not. The rule exists so a factory can be a constructor.
      return Result::Normal(constructed.value.IsObject() ? constructed.value : self);
    }

    case NodeKind::Member: {
      Value base;
      const Result key = EvaluateMember(node, scope, base);
      if (key.IsAbrupt()) {
        return key;
      }
      if (node.number == 2.0 && base.IsNullish()) {
        return Result::Normal(Value::Undefined());  // `a?.b`
      }
      if (base.IsNullish()) {
        return Throw("TypeError",
                     "cannot read property '" + ToString(key.value) + "' of " + ToString(base));
      }
      return Result::Normal(GetProperty(base, ToString(key.value)));
    }

    case NodeKind::Sequence: {
      Result last = Result::Normal();
      for (const NodePtr& element : node.children) {
        if (element == nullptr) {
          continue;
        }
        last = Evaluate(*element, scope);
        if (last.IsAbrupt()) {
          return last;
        }
      }
      return last;
    }

    case NodeKind::ClassExpression:
      return EvaluateClass(node, scope);

    case NodeKind::Super: {
      // `super` on its own is only reachable as `super(...)` or `super.x`, and
      // both are handled where the call and the member access are. Reaching
      // here means it was used as a value, which it is not.
      return Throw("SyntaxError", "'super' keyword unexpected here");
    }

    case NodeKind::TaggedTemplate:
    case NodeKind::Spread:
      return Throw("SyntaxError", "unsupported expression");

    default:
      return EvaluateStatement(node, scope);
  }
}

Result Interpreter::EvaluateBlock(const Node& node, Environment& scope) {
  // A block that declares nothing needs no scope of its own, and skipping it
  // is not only a speed matter: `while (true) {}` allocated one per iteration,
  // and since the collector cannot run mid-evaluation, an empty infinite loop
  // exhausted the heap before it reached the step budget. Found by the fuzzer.
  bool declares = false;
  for (const NodePtr& statement : node.children) {
    if (statement == nullptr) {
      continue;
    }
    declares = declares || statement->kind == NodeKind::VariableDeclaration ||
               statement->kind == NodeKind::FunctionDeclaration ||
               statement->kind == NodeKind::ClassDeclaration;
  }
  if (!declares) {
    Result last = Result::Normal();
    for (const NodePtr& statement : node.children) {
      if (statement == nullptr) {
        continue;
      }
      last = EvaluateStatement(*statement, scope);
      if (last.IsAbrupt()) {
        return last;
      }
    }
    return last;
  }

  Environment* block_scope = heap_.AllocateEnvironment(&scope);
  if (block_scope == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  const ScopeGuard guard(*this, block_scope);
  HoistDeclarations(node, *block_scope);

  Result last = Result::Normal();
  for (const NodePtr& statement : node.children) {
    if (statement == nullptr) {
      continue;
    }
    last = EvaluateStatement(*statement, *block_scope);
    if (last.IsAbrupt()) {
      return last;
    }
  }
  return last;
}

Result Interpreter::EvaluateForIn(const Node& node, Environment& scope) {
  const std::string my_label = std::move(pending_label_);
  pending_label_.clear();
  const Node* left = node.Child(0);
  const Node* right = node.Child(1);
  const Node* body = node.Child(2);
  if (left == nullptr || right == nullptr || body == nullptr) {
    return Throw("SyntaxError", "malformed for-in");
  }

  const Result iterable = Evaluate(*right, scope);
  if (iterable.IsAbrupt()) {
    return iterable;
  }

  std::vector<Value> items;
  const bool is_of = node.string == "of";
  if (iterable.value.IsObject()) {
    Object* object = iterable.value.object;
    if (object->GetKind() == Object::Kind::Array) {
      for (std::size_t i = 0; i < object->ElementCount(); ++i) {
        // `for...of` yields values, `for...in` yields keys -- and for an array
        // the keys are strings, which is the classic reason `for...in` over an
        // array gives "0", "1" rather than 0, 1.
        if (is_of) {
          items.push_back(object->GetElement(i));
        } else if (object->HasElement(i)) {
          items.push_back(Value::String(std::to_string(i)));
        }
      }
    }
    if (!is_of) {
      for (const std::string& key : object->Keys()) {
        items.push_back(Value::String(key));
      }
    }
  } else if (iterable.value.IsString() && is_of) {
    for (const char c : iterable.value.AsString()) {
      items.push_back(Value::String(std::string(1, c)));
    }
  } else if (!iterable.value.IsNullish() && is_of) {
    return Throw("TypeError", ToString(iterable.value) + " is not iterable");
  }

  for (const Value& item : items) {
    Environment* iteration = heap_.AllocateEnvironment(&scope);
    if (iteration == nullptr) {
      return Throw("RangeError", "out of memory");
    }
    const ScopeGuard guard(*this, iteration);

    Result bound = Result::Normal();
    if (left->kind == NodeKind::VariableDeclaration) {
      const Node* declarator = left->Child(0);
      const Node* target = declarator == nullptr ? nullptr : declarator->Child(0);
      if (target != nullptr) {
        bound = BindPattern(*target, item, *iteration, true, left->string == "const");
      }
    } else {
      bound = BindPattern(*left, item, *iteration, false, false);
    }
    if (bound.IsAbrupt()) {
      return bound;
    }

    Result result = EvaluateStatement(*body, *iteration);
    if (result.completion == Completion::Break) {
      if (result.label.empty() || result.label == my_label) {
        break;
      }
      return result;
    }
    if (result.completion == Completion::Continue) {
      if (!result.label.empty() && result.label != my_label) {
        return result;
      }
      continue;
    }
    if (result.IsAbrupt()) {
      return result;
    }
  }
  return Result::Normal();
}

Result Interpreter::EvaluateStatement(const Node& node, Environment& scope) {
  const DepthCounter depth(eval_depth_, kMaxEvalDepth);
  if (depth.Exceeded()) {
    return Throw("RangeError", "maximum call stack size exceeded");
  }
  if (++steps_ > kMaxSteps) {
    return Throw("RangeError", "script ran too long");
  }

  switch (node.kind) {
    case NodeKind::Program:
    case NodeKind::Block:
      return EvaluateBlock(node, scope);

    case NodeKind::Empty:
    case NodeKind::Debugger:
      return Result::Normal();

    case NodeKind::ExpressionStatement: {
      const Node* expression = node.Child(0);
      return expression == nullptr ? Result::Normal() : Evaluate(*expression, scope);
    }

    case NodeKind::VariableDeclaration: {
      const bool is_const = node.string == "const";
      for (const NodePtr& declarator : node.children) {
        if (declarator == nullptr) {
          continue;
        }
        const Node* target = declarator->Child(0);
        if (target == nullptr) {
          continue;
        }
        Value value;
        if (declarator->Child(1) != nullptr) {
          const Result initializer = Evaluate(*declarator->Child(1), scope);
          if (initializer.IsAbrupt()) {
            return initializer;
          }
          value = initializer.value;
        } else if (is_const) {
          return Throw("SyntaxError", "a const declaration needs an initializer");
        }
        const Result bound = BindPattern(*target, value, scope, true, is_const);
        if (bound.IsAbrupt()) {
          return bound;
        }
      }
      return Result::Normal();
    }

    case NodeKind::FunctionDeclaration:
      // Already hoisted into the scope; re-declaring here would rebind it to a
      // closure over the same scope, which is the same function.
      if (scope.Lookup(node.string) == nullptr) {
        scope.Declare(node.string, NewFunction(node, scope, false), false);
      }
      return Result::Normal();

    case NodeKind::If: {
      const Node* test = node.Child(0);
      if (test == nullptr) {
        return Throw("SyntaxError", "malformed if");
      }
      const Result condition = Evaluate(*test, scope);
      if (condition.IsAbrupt()) {
        return condition;
      }
      const Node* branch = ToBoolean(condition.value) ? node.Child(1) : node.Child(2);
      return branch == nullptr ? Result::Normal() : EvaluateStatement(*branch, scope);
    }

    case NodeKind::While:
    case NodeKind::DoWhile: {
      const std::string my_label = std::move(pending_label_);
      pending_label_.clear();
      const bool is_do = node.kind == NodeKind::DoWhile;
      const Node* test = node.Child(is_do ? 1 : 0);
      const Node* body = node.Child(is_do ? 0 : 1);
      if (test == nullptr || body == nullptr) {
        return Throw("SyntaxError", "malformed loop");
      }
      bool first = true;
      while (true) {
        if (!(is_do && first)) {
          const Result condition = Evaluate(*test, scope);
          if (condition.IsAbrupt()) {
            return condition;
          }
          if (!ToBoolean(condition.value)) {
            break;
          }
        }
        first = false;

        Result result = EvaluateStatement(*body, scope);
        if (result.completion == Completion::Break) {
          if (result.label.empty() || result.label == my_label) {
            break;
          }
          return result;
        }
        if (result.completion == Completion::Continue) {
          if (!result.label.empty() && result.label != my_label) {
            return result;
          }
          continue;
        }
        if (result.IsAbrupt()) {
          return result;
        }
      }
      return Result::Normal();
    }

    case NodeKind::For: {
      const std::string my_label = std::move(pending_label_);
      pending_label_.clear();
      Environment* loop_scope = heap_.AllocateEnvironment(&scope);
      if (loop_scope == nullptr) {
        return Throw("RangeError", "out of memory");
      }
      const ScopeGuard guard(*this, loop_scope);

      if (node.Child(0) != nullptr) {
        const Result init = EvaluateStatement(*node.Child(0), *loop_scope);
        if (init.IsAbrupt()) {
          return init;
        }
      }
      while (true) {
        if (node.Child(1) != nullptr) {
          const Result condition = Evaluate(*node.Child(1), *loop_scope);
          if (condition.IsAbrupt()) {
            return condition;
          }
          if (!ToBoolean(condition.value)) {
            break;
          }
        }
        if (node.Child(3) != nullptr) {
          Result result = EvaluateStatement(*node.Child(3), *loop_scope);
          if (result.completion == Completion::Break) {
            if (result.label.empty() || result.label == my_label) {
              break;
            }
            return result;
          }
          if (result.completion == Completion::Continue && !result.label.empty() &&
              result.label != my_label) {
            return result;
          }
          if (result.IsAbrupt() && result.completion != Completion::Continue) {
            return result;
          }
        }
        if (node.Child(2) != nullptr) {
          const Result update = Evaluate(*node.Child(2), *loop_scope);
          if (update.IsAbrupt()) {
            return update;
          }
        }
      }
      return Result::Normal();
    }

    case NodeKind::ForIn:
      return EvaluateForIn(node, scope);

    case NodeKind::Return: {
      Value value;
      if (node.Child(0) != nullptr) {
        const Result argument = Evaluate(*node.Child(0), scope);
        if (argument.IsAbrupt()) {
          return argument;
        }
        value = argument.value;
      }
      return Result{Completion::Return, std::move(value), {}};
    }

    case NodeKind::Break:
      return Result{Completion::Break, Value::Undefined(), node.string};
    case NodeKind::Continue:
      return Result{Completion::Continue, Value::Undefined(), node.string};

    case NodeKind::Throw: {
      if (node.Child(0) == nullptr) {
        return Throw("SyntaxError", "malformed throw");
      }
      const Result argument = Evaluate(*node.Child(0), scope);
      if (argument.IsAbrupt()) {
        return argument;
      }
      return Result{Completion::Throw, argument.value, {}};
    }

    case NodeKind::Try: {
      const Node* block = node.Child(0);
      Result result = block == nullptr ? Result::Normal() : EvaluateStatement(*block, scope);

      if (result.completion == Completion::Throw && node.Child(2) != nullptr) {
        Environment* catch_scope = heap_.AllocateEnvironment(&scope);
        if (catch_scope == nullptr) {
          return Throw("RangeError", "out of memory");
        }
        const ScopeGuard guard(*this, catch_scope);
        if (node.Child(1) != nullptr) {
          const Result bound =
              BindPattern(*node.Child(1), result.value, *catch_scope, true, false);
          if (bound.IsAbrupt()) {
            return bound;
          }
        }
        result = EvaluateStatement(*node.Child(2), *catch_scope);
      }

      if (node.Child(3) != nullptr) {
        // finally runs whatever the block did, and an abrupt completion of its
        // own replaces that -- which is why `try { return 1 } finally
        // { return 2 }` returns 2. Written out because it is the case that
        // makes exceptions the wrong tool for this.
        const Result finalizer = EvaluateStatement(*node.Child(3), scope);
        if (finalizer.IsAbrupt()) {
          return finalizer;
        }
      }
      return result;
    }

    case NodeKind::Switch: {
      const Node* discriminant = node.Child(0);
      if (discriminant == nullptr) {
        return Throw("SyntaxError", "malformed switch");
      }
      const Result subject = Evaluate(*discriminant, scope);
      if (subject.IsAbrupt()) {
        return subject;
      }

      Environment* switch_scope = heap_.AllocateEnvironment(&scope);

      if (switch_scope == nullptr) {

        return Throw("RangeError", "out of memory");

      }
      const ScopeGuard guard(*this, switch_scope);

      // Two passes: find where to enter, then run from there to the end or a
      // break. `default` is only taken when nothing matched, however early it
      // appears, and execution still falls through into what follows it.
      std::size_t start = node.children.size();
      std::size_t default_clause = node.children.size();
      for (std::size_t i = 1; i < node.children.size(); ++i) {
        const Node* clause = node.Child(i);
        if (clause == nullptr) {
          continue;
        }
        if (clause->Child(0) == nullptr) {
          default_clause = i;
          continue;
        }
        const Result test = Evaluate(*clause->Child(0), *switch_scope);
        if (test.IsAbrupt()) {
          return test;
        }
        if (StrictEquals(subject.value, test.value)) {
          start = i;
          break;
        }
      }
      if (start == node.children.size()) {
        start = default_clause;
      }

      for (std::size_t i = start; i < node.children.size(); ++i) {
        const Node* clause = node.Child(i);
        if (clause == nullptr) {
          continue;
        }
        for (std::size_t j = 1; j < clause->children.size(); ++j) {
          const Node* statement = clause->Child(j);
          if (statement == nullptr) {
            continue;
          }
          Result result = EvaluateStatement(*statement, *switch_scope);
          if (result.completion == Completion::Break && result.label.empty()) {
            return Result::Normal();
          }
          if (result.IsAbrupt()) {
            return result;
          }
        }
      }
      return Result::Normal();
    }

    case NodeKind::Labeled: {
      const Node* body = node.Child(0);
      if (body == nullptr) {
        return Result::Normal();
      }
      // Handed to the statement below, which takes it if it is a loop. A
      // labelled `continue` names a loop, not a label, and the loop is the only
      // thing that can act on it.
      pending_label_ = node.string;
      Result result = EvaluateStatement(*body, scope);
      pending_label_.clear();
      if ((result.completion == Completion::Break ||
           result.completion == Completion::Continue) &&
          result.label == node.string) {
        return Result::Normal();
      }
      return result;
    }

    case NodeKind::ClassDeclaration: {
      const Result value = EvaluateClass(node, scope);
      if (value.IsAbrupt()) {
        return value;
      }
      scope.Declare(node.string, value.value, false);
      return Result::Normal();
    }

    default:
      // Does *not* fall back to Evaluate. Evaluate's own default sends
      // unrecognised kinds here, so two defaults that each deferred to the
      // other bounced until the stack ran out -- found by the fuzzer as a
      // stack overflow on a node kind that is neither an expression nor a
      // statement (a Declarator or a Parameters list reached directly). A kind
      // arriving here is a gap between the parser and the evaluator, and
      // saying so is the only useful answer.
      return Throw("SyntaxError", "this construct cannot be evaluated");
  }
}

}  // namespace microbrowser::js

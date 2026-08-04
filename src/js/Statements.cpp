#include <cmath>
#include <utility>

#include "js/Interpreter.h"
#include "js/TemplateParts.h"

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
      return EvaluateRegExpLiteral(node);

    case NodeKind::TemplateLiteral: {
      // The literal chunks with each substitution's value between them. The
      // split is the same function the parser used, so the children line up
      // with the gaps by construction rather than by two walks agreeing.
      const TemplateParts parts = SplitTemplate(node.string);
      std::string out;
      for (std::size_t i = 0; i < parts.literals.size(); ++i) {
        out += parts.literals[i];
        if (i >= node.children.size()) {
          continue;  // the last chunk has no substitution after it
        }
        // Each child is the substitution's expression -- the parser parses one
        // per `${}`, so no unwrapping and no re-scan of the raw text.
        const Node* expression = node.Child(i);
        if (expression == nullptr) {
          continue;  // an empty `${}` contributes nothing
        }
        const Result value = Evaluate(*expression, scope);
        if (value.IsAbrupt()) {
          return value;
        }
        // ToString with its own hint, not whatever `+` would have picked: a
        // substitution tries `toString` before `valueOf`.
        std::string text;
        const Result converted = ToStringOf(value.value, text);
        if (converted.IsAbrupt()) {
          return converted;
        }
        out += text;
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

    case NodeKind::NewTarget: {
      // An ordinary name lookup, so an arrow -- which declares none of its own
      // -- finds the enclosing function's, the way it finds `this`.
      Value* binding = scope.Lookup("__newtarget__");
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
          // Any iterable, which is what makes `[...'abc']` three characters
          // and `[...someSet]` its members.
          std::vector<Value> spread_values;
          const Result collected = CollectIterable(spread.value, spread_values);
          if (collected.IsAbrupt()) {
            return collected;
          }
          for (Value& item : spread_values) {
            elements.push_back(std::move(item));
            present.push_back(true);
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
        // The parser packs three independent facts into `number`: whether the
        // key was computed, and whether this is a getter or a setter.
        const auto flags = static_cast<int>(property->number);
        const bool computed = (flags & 1) != 0;
        const bool is_getter = (flags & 2) != 0;
        const bool is_setter = (flags & 4) != 0;
        // A computed key keeps whatever it evaluated to. Stringifying here is
        // what would file `{ [Symbol.iterator]() {} }` under a name a page can
        // write out, which would make the protocol hook forgeable.
        PropertyKey key = property->string;
        if (computed && property->Child(1) != nullptr) {
          const Result evaluated_key = Evaluate(*property->Child(1), scope);
          if (evaluated_key.IsAbrupt()) {
            return evaluated_key;
          }
          const Result converted = ToKeyOf(evaluated_key.value, key);
          if (converted.IsAbrupt()) {
            return converted;
          }
        }
        const Node* value_node = property->Child(0);
        if (value_node == nullptr) {
          continue;
        }
        const Result value = Evaluate(*value_node, scope);
        if (value.IsAbrupt()) {
          return value;
        }
        if (is_getter || is_setter) {
          if (!value.value.IsObject()) {
            return Throw("TypeError", "an accessor must be a function");
          }
          // Defined rather than set, so that `{ get x(){}, set x(v){} }`
          // fills in the two halves of one property rather than the second
          // replacing the first.
          object->DefineAccessor(std::move(key), is_getter ? value.value.object : nullptr,
                                 is_setter ? value.value.object : nullptr);
          continue;
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
            PropertyKey property;
            const Result converted = ToKeyOf(key.value, property);
            if (converted.IsAbrupt()) {
              return converted;
            }
            return Result::Normal(Value::Bool(base.object->Delete(property)));
          }
        }
        return Result::Normal(Value::Bool(true));
      }

      const Result value = Evaluate(*operand, scope);
      if (value.IsAbrupt()) {
        return value;
      }
      if (node.string == "!") return Result::Normal(Value::Bool(!ToBoolean(value.value)));
      if (node.string == "-" || node.string == "+" || node.string == "~") {
        // Through ToNumberOf rather than ToNumber: `-obj` runs the object's
        // `valueOf`, and `+[]` is 0 rather than NaN because of it.
        double number = 0;
        const Result converted = ToNumberOf(value.value, number);
        if (converted.IsAbrupt()) {
          return converted;
        }
        if (node.string == "~") {
          return Result::Normal(Value::Number(~ToInt32(number)));
        }
        return Result::Normal(Value::Number(node.string == "-" ? -number : number));
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
      double before = 0;
      const Result converted = ToNumberOf(current.value, before);
      if (converted.IsAbrupt()) {
        return converted;
      }
      const double after = node.string == "++" ? before + 1 : before - 1;

      if (operand->kind == NodeKind::Member) {
        Value base;
        const Result key = EvaluateMember(*operand, scope, base);
        if (key.IsAbrupt()) {
          return key;
        }
        PropertyKey property;
        const Result converted_key = ToKeyOf(key.value, property);
        if (converted_key.IsAbrupt()) {
          return converted_key;
        }
        const Result stored = SetProperty(base, property, Value::Number(after));
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
      std::vector<Value> arguments;
      for (std::size_t i = 1; i < node.children.size(); ++i) {
        const Node* argument = node.Child(i);
        if (argument == nullptr) {
          continue;
        }
        // `new C(...args)`. The parser already accepts it; evaluating the
        // spread node as an ordinary expression is what used to make it a
        // "SyntaxError: unsupported expression" at run time.
        if (argument->kind == NodeKind::Spread) {
          const Node* inner = argument->Child(0);
          if (inner == nullptr) {
            continue;
          }
          const Result spread = Evaluate(*inner, scope);
          if (spread.IsAbrupt()) {
            return spread;
          }
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
      return Construct(callee.value, arguments);
    }

    case NodeKind::Member: {
      const auto flags = static_cast<std::uint8_t>(node.number);
      Value base;
      const Result key = EvaluateMember(node, scope, base);
      if (key.IsAbrupt()) {
        return key;
      }
      // A link further in already gave up. Nothing here runs, and the mark on
      // the root is what turns this back into undefined.
      if (IsChainSignal(base)) {
        return Result::Normal((flags & kMemberChainRoot) != 0 ? Value::Undefined() : base);
      }
      if ((flags & kMemberOptional) != 0 && base.IsNullish()) {
        // `a?.b`. The whole chain gives up, not just this access -- so unless
        // this *is* the whole chain, the marker goes out rather than undefined.
        return Result::Normal((flags & kMemberChainRoot) != 0 ? Value::Undefined()
                                                              : ChainSignal());
      }
      if (base.IsNullish()) {
        return Throw("TypeError",
                     "cannot read property '" + ToString(key.value) + "' of " + ToString(base));
      }
      PropertyKey property;
      const Result converted = ToKeyOf(key.value, property);
      if (converted.IsAbrupt()) {
        return converted;
      }
      return Result::Normal(GetProperty(base, property));
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
      return EvaluateTaggedTemplate(node, scope);

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

  // `for...in` enumerates keys and has no protocol behind it, so its items are
  // collected up front. `for...of` runs the iteration protocol, which is
  // observable: an iterator whose `next` has side effects must not be stepped
  // past a `break`, so it is driven one value at a time rather than drained
  // into a vector first.
  const bool is_of = node.string == "of";
  std::vector<Value> keys;
  Iteration cursor;
  if (is_of) {
    const Result opened = OpenIteration(iterable.value, cursor);
    if (opened.IsAbrupt()) {
      return opened;
    }
  } else if (iterable.value.IsObject()) {
    Object* object = iterable.value.object;
    if (object->GetKind() == Object::Kind::Array) {
      for (std::size_t i = 0; i < object->ElementCount(); ++i) {
        // For an array the keys are strings, which is the classic reason
        // `for...in` over one gives "0", "1" rather than 0, 1.
        if (object->HasElement(i)) {
          keys.push_back(Value::String(std::to_string(i)));
        }
      }
    }
    for (const std::string& key : object->Keys()) {
      keys.push_back(Value::String(key));
    }
  }

  // The iterator object is held on the shadow stack for as long as the loop
  // runs: it lives in a C++ local, which the collector cannot see.
  if (cursor.iterator.IsObject()) {
    active_objects_.push_back(cursor.iterator.object);
  } else if (cursor.array != nullptr) {
    active_objects_.push_back(cursor.array);
  }
  struct IteratorRoot {
    Interpreter& interpreter;
    bool held;
    ~IteratorRoot() {
      if (held) {
        interpreter.active_objects_.pop_back();
      }
    }
  } root{*this, cursor.iterator.IsObject() || cursor.array != nullptr};

  // What every way out of the loop goes through, so that an iterator this walks
  // away from is told so. Called while `root` above is still holding the
  // iterator, which is what makes it safe to run the page's `return` here.
  //
  // A throw is the one completion that does not close, which is a deviation
  // and a deliberate one: the machine does not close there either -- its
  // UnwindToHandler truncates the cursor stack without running anything -- and
  // two engines disagreeing is worse than one shared gap that is written down.
  const auto leave = [&](Result result) {
    if (!is_of || result.completion == Completion::Throw) {
      return result;
    }
    Result closed = CloseIterationCursor(cursor);
    return closed.IsAbrupt() ? closed : result;
  };

  for (std::size_t step = 0;; ++step) {
    Value item;
    if (is_of) {
      bool done = false;
      const Result advanced = StepIteration(cursor, item, done);
      if (advanced.IsAbrupt()) {
        // The iterator itself threw, so it is finished and is not asked to
        // close -- asking a `next` that failed to also `return` is not what
        // the protocol says and is one more call into code that just broke.
        cursor.done = true;
        return advanced;
      }
      // Written back, not just read: `leave` below asks the cursor whether it
      // finished, and an exhausted iterator must not be asked to close.
      cursor.done = done;
      if (done) {
        break;
      }
    } else {
      if (step >= keys.size()) {
        break;
      }
      item = keys[step];
    }

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
      return leave(std::move(bound));
    }

    Result result = EvaluateStatement(*body, *iteration);
    if (result.completion == Completion::Break) {
      if (result.label.empty() || result.label == my_label) {
        break;
      }
      return leave(std::move(result));
    }
    if (result.completion == Completion::Continue) {
      if (!result.label.empty() && result.label != my_label) {
        return leave(std::move(result));
      }
      continue;
    }
    if (result.IsAbrupt()) {
      return leave(std::move(result));
    }
  }
  // Off the end of the loop: exhausted, or a `break` that belonged here. The
  // first closes nothing because the cursor is already done, and the second is
  // the case this exists for.
  return leave(Result::Normal());
}

Result Interpreter::EvaluateStatement(const Node& node, Environment& scope) {
  const DepthCounter depth(eval_depth_, kMaxEvalDepth);
  if (depth.Exceeded()) {
    return Throw("RangeError", "maximum call stack size exceeded");
  }
  if (++steps_ > kMaxSteps) {
    return Throw("RangeError", "script ran too long");
  }

  // A label reaches the statement it was written on and no further.
  //
  // It used to reach everything inside that statement, because only the loops
  // cleared it: `found: { for (const a of xs) { break found } }` left the label
  // set when the `for` started, so the loop believed it was the one named and
  // `break found` left the loop instead of the block. Found by running the same
  // program on both engines -- the compiler resolves a label to a construct
  // while compiling, so it could not make this mistake.
  switch (node.kind) {
    case NodeKind::While:
    case NodeKind::DoWhile:
    case NodeKind::For:
    case NodeKind::ForIn:
    case NodeKind::Labeled:
      break;  // these three read it; Labeled is what sets it
    default:
      pending_label_.clear();
      break;
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
      ScopeGuard guard(*this, loop_scope);

      if (node.Child(0) != nullptr) {
        const Result init = EvaluateStatement(*node.Child(0), *loop_scope);
        if (init.IsAbrupt()) {
          return init;
        }
      }
      // A `let` or `const` head is one binding *per iteration*: each pass gets
      // a fresh copy of the loop scope, so the three closures a
      // `for (let i = 0; i < 3; i++)` makes see 0, 1 and 2. `var` is the
      // contrast and keeps one binding for the whole loop.
      const Node* init_node = node.Child(0);
      const bool per_iteration = init_node != nullptr &&
                                 init_node->kind == NodeKind::VariableDeclaration &&
                                 (init_node->string == "let" || init_node->string == "const");
      const auto next_iteration_scope = [&]() -> bool {
        if (!per_iteration) {
          return true;
        }
        Environment* fresh = heap_.AllocateEnvironment(loop_scope->Parent());
        if (fresh == nullptr) {
          return false;
        }
        fresh->CopyBindingsFrom(*loop_scope);
        loop_scope = fresh;
        // Rooted from here on. The old one stays alive through whatever
        // closure the body made, which is the binding it is meant to keep.
        guard.Retarget(loop_scope);
        return true;
      };
      if (!next_iteration_scope()) {
        return Throw("RangeError", "out of memory");
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
        // The copy sits before the increment, so the increment writes to the
        // new binding and the closure the body just made keeps the old value.
        if (!next_iteration_scope()) {
          return Throw("RangeError", "out of memory");
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

Result Interpreter::EvaluateRegExpLiteral(const Node& node) {

      // The literal arrives as it was written, delimiters and all, because the
      // lexer's job was to find its extent rather than to take it apart. The
      // last `/` separates the pattern from the flags: the first one cannot,
      // since `/[/]/` has one in the middle.
      const std::size_t close = node.string.rfind('/');
      if (node.string.size() < 2 || node.string.front() != '/' || close == 0) {
        return Throw("SyntaxError", "malformed regular expression literal");
      }
      const std::string source = node.string.substr(1, close - 1);
      const std::string flag_text = node.string.substr(close + 1);
      const std::optional<RegExpFlags> flags = RegExpFlags::Parse(flag_text);
      if (!flags.has_value()) {
        return Throw("SyntaxError", "invalid regular expression flags: " + flag_text);
      }
      std::string error;
      RegExp pattern = RegExp::Compile(source, *flags, error);
      if (!pattern.IsValid()) {
        return Throw("SyntaxError", "invalid regular expression: " + error);
      }
      // A fresh object per evaluation, which is what makes `lastIndex` on a
      // literal inside a loop start over each time round.
      const Value value = NewRegExpValue(std::move(pattern));
      if (value.IsUndefined()) {
        return Throw("RangeError", "out of memory");
      }
      return Result::Normal(value);
    }

Result Interpreter::EvaluateTaggedTemplate(const Node& node, Environment& scope) {

      // ``tag`a${x}b` `` is `tag(["a", "b"], x)`. The tag receives the literal
      // chunks as an array and the substitution *values* as the arguments
      // after it -- which is the whole point of the form: the tag sees the two
      // apart and can decide what to do with each, which is how a library
      // escapes an interpolation it did not write.
      const Node* tag_node = node.Child(0);
      const Node* template_node = node.Child(1);
      if (tag_node == nullptr || template_node == nullptr) {
        return Throw("SyntaxError", "malformed tagged template");
      }
      Value self;
      Result tag = Result::Normal();
      if (tag_node->kind == NodeKind::Member) {
        // A method tag keeps its receiver: ``obj.tag`x` `` calls it on `obj`.
        const Result key = EvaluateMember(*tag_node, scope, self);
        if (key.IsAbrupt()) {
          return key;
        }
        PropertyKey property;
        const Result converted = ToKeyOf(key.value, property);
        if (converted.IsAbrupt()) {
          return converted;
        }
        tag = Result::Normal(GetProperty(self, property));
      } else {
        tag = Evaluate(*tag_node, scope);
        if (tag.IsAbrupt()) {
          return tag;
        }
      }
      if (!tag.value.IsObject() || !tag.value.object->IsCallable()) {
        return Throw("TypeError", "the tag of a tagged template is not a function");
      }

      const TemplateParts parts = SplitTemplate(template_node->string);
      std::vector<Value> chunks;
      chunks.reserve(parts.literals.size());
      for (const std::string& literal : parts.literals) {
        chunks.push_back(Value::String(literal));
      }
      const Value strings = NewArrayValue(chunks);
      if (!strings.IsObject()) {
        return Throw("RangeError", "out of memory");
      }
      // `raw` is the text *before* escape processing, which is the whole
      // reason a tag exists: it can see the backslash the cooked form ate.
      std::vector<Value> raw_chunks;
      for (const std::string& literal : parts.raws) {
        raw_chunks.push_back(Value::String(literal));
      }
      strings.object->Set("raw", NewArrayValue(std::move(raw_chunks)));

      std::vector<Value> arguments{strings};
      for (const NodePtr& child : template_node->children) {
        if (child == nullptr) {
          arguments.push_back(Value::Undefined());
          continue;
        }
        const Result value = Evaluate(*child, scope);
        if (value.IsAbrupt()) {
          return value;
        }
        arguments.push_back(value.value);
      }
      return CallFunction(tag.value, self, arguments);
    }

}  // namespace microbrowser::js

// `for...in` and `for...of` for the tree-walker.
//
// Its own translation unit because Statements.cpp reached the module's line
// cap, and the lint's advice on that is right: a file over its cap means a
// missing module rather than a bigger file. This is the natural seam -- one
// self-contained loop whose whole difficulty is the iterator protocol and
// closing a cursor on every way out, which nothing else in Statements.cpp
// shares.

#include <string>
#include <utility>

#include "js/Interpreter.h"

namespace microbrowser::js {

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
    // Up the chain, and each name once. See the note on the machine's
    // ForInKeys; the two have to agree about what a `for...in` reports.
    std::vector<std::string> seen;
    for (Object* walk = object; walk != nullptr;) {
      for (const std::string& key : OwnKeys(Value::Obj(walk), true)) {
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
          seen.push_back(key);
        }
      }
      walk = walk->Prototype();
      if (walk == object) {
        break;
      }
    }
    for (const std::string& key : seen) {
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
        // `for (var k in o)` assigns to the function-scope binding hoisted at
        // entry, the same as any other `var`. Only `let` and `const` get the
        // fresh per-iteration binding this scope exists for.
        const bool is_var = left->string == "var";
        bound = BindPattern(*target, item, *iteration, !is_var, left->string == "const");
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

}  // namespace microbrowser::js

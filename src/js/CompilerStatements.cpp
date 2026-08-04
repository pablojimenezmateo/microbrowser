#include <string>
#include <utility>

#include "js/CompilerImpl.h"

// The compiler, statement half. Control flow becomes jumps, and the four abrupt
// completions the tree-walker returns as values become four different shapes:
//
//   break / continue -- a jump, with the scopes and cursors between here and
//                       the target unwound in front of it
//   return           -- an opcode, after the enclosing finalizers have run
//   throw            -- the handler table, which is the only one the machine
//                       has to search for at run time
//
// That asymmetry is the point. Three of the four are decided while compiling,
// so the running loop pays nothing for them.

namespace microbrowser::js {

bool Compiler::Declares(const Node& list) {
  for (const NodePtr& statement : list.children) {
    if (statement == nullptr) {
      continue;
    }
    if (statement->kind == NodeKind::VariableDeclaration ||
        statement->kind == NodeKind::FunctionDeclaration ||
        statement->kind == NodeKind::ClassDeclaration) {
      return true;
    }
  }
  return false;
}

void Compiler::Hoist(const Node& list) {
  // Function declarations are visible before the line that writes them, which
  // is what makes mutually recursive functions work without forward
  // declarations. `var` hoisting is deliberately absent, as in the tree-walker.
  for (const NodePtr& statement : list.children) {
    if (statement == nullptr || statement->kind != NodeKind::FunctionDeclaration) {
      continue;
    }
    FunctionValue(*statement, false);
    EmitDeclare(statement->string, false);
  }
}

void Compiler::Program(const Node& program) {
  is_program_ = true;
  // Slot zero of the frame is the script's completion value.
  Emit(Op::PushUndefined, 0, 1);
  Hoist(program);
  for (const NodePtr& statement : program.children) {
    if (statement == nullptr) {
      continue;
    }
    // Cleared per top-level statement, so that `1; if (false) 2` is undefined
    // rather than 1 -- a statement that completes without a value replaces the
    // one before it.
    Emit(Op::ClearCompletion);
    Statement(*statement);
  }
  Emit(Op::LoadCompletion, 0, 1);
  Emit(Op::Return, 0, -1);
}

void Compiler::StatementList(const Node& list) {
  Hoist(list);
  for (const NodePtr& statement : list.children) {
    if (statement == nullptr) {
      continue;
    }
    Statement(*statement);
  }
}

void Compiler::Block(const Node& node) {
  // A block that declares nothing needs no scope of its own, and skipping it is
  // not only a speed matter: `while (true) {}` allocated one per iteration, and
  // an empty infinite loop exhausted the heap before it reached the step
  // budget. Found by the fuzzer, and still true.
  if (!Declares(node)) {
    StatementList(node);
    return;
  }
  // Every name the block declares gets its slot before any of the block runs.
  // Two passes rather than one because control flow can skip a declaration --
  // `switch (n) { case 1: let a; case 2: let b }` entered at the second case
  // must still find `b` where the compiler put it.
  scopes_.emplace_back();
  ReserveDeclarations(node);
  Emit(Op::PushScope, scopes_.back().count, 0);
  ++scope_depth_;
  StatementList(node);
  Emit(Op::PopScope, 1, 0);
  --scope_depth_;
  scopes_.pop_back();
}

void Compiler::Statement(const Node& node) {
  const CompileDepth depth(state_, kMaxCompileDepth);
  if (depth.Exceeded() || state_.failed) {
    Fail();
    return;
  }

  switch (node.kind) {
    case NodeKind::Program:
    case NodeKind::Block:
      Block(node);
      return;

    case NodeKind::Empty:
    case NodeKind::Debugger:
      return;

    case NodeKind::ExpressionStatement: {
      const Node* expression = node.Child(0);
      if (expression == nullptr) {
        return;
      }
      Expression(*expression);
      // At the top level the value is the script's; anywhere else it is
      // discarded, which is what a statement does with it.
      Emit(is_program_ ? Op::SetCompletion : Op::Pop, 0, -1);
      return;
    }

    case NodeKind::VariableDeclaration:
      VariableDeclaration(node);
      return;

    case NodeKind::FunctionDeclaration:
      // Already hoisted into this scope by Hoist.
      return;

    case NodeKind::ClassDeclaration:
      // Same two halves as the expression form: the method bodies compiled
      // here, the class itself built by EvaluateClass.
      ClassMethods(node);
      Emit(Op::ClassLiteral, NodeIndex(node), 1);
      EmitDeclare(node.string, false);
      return;

    case NodeKind::If:
      IfStatement(node);
      return;
    case NodeKind::While:
      WhileStatement(node, false);
      return;
    case NodeKind::DoWhile:
      WhileStatement(node, true);
      return;
    case NodeKind::For:
      ForStatement(node);
      return;
    case NodeKind::ForIn:
      ForInStatement(node);
      return;
    case NodeKind::Try:
      TryStatement(node);
      return;
    case NodeKind::Switch:
      SwitchStatement(node);
      return;
    case NodeKind::Labeled:
      LabeledStatement(node);
      return;

    case NodeKind::Return:
      ReturnStatement(node);
      return;
    case NodeKind::Break:
      BreakOrContinue(node, true);
      return;
    case NodeKind::Continue:
      BreakOrContinue(node, false);
      return;

    case NodeKind::Throw: {
      const Node* argument = node.Child(0);
      if (argument == nullptr) {
        ThrowSyntax("malformed throw");
        Emit(Op::Pop, 0, -1);
        return;
      }
      Expression(*argument);
      Emit(Op::ThrowOp, 0, -1);
      return;
    }

    default:
      // Not a statement, so it is an expression used as one. Anything neither
      // is a gap between the parser and this, and the tree-walker answers it.
      if (node.kind == NodeKind::Declarator || node.kind == NodeKind::Parameters ||
          node.kind == NodeKind::Property || node.kind == NodeKind::SwitchCase ||
          node.kind == NodeKind::MethodDefinition) {
        Fail();
        return;
      }
      Expression(node);
      Emit(is_program_ ? Op::SetCompletion : Op::Pop, 0, -1);
      return;
  }
}

void Compiler::VariableDeclaration(const Node& node) {
  const bool is_const = node.string == "const";
  for (const NodePtr& declarator : node.children) {
    if (declarator == nullptr) {
      continue;
    }
    const Node* target = declarator->Child(0);
    if (target == nullptr) {
      continue;
    }
    const Node* initializer = declarator->Child(1);
    if (initializer != nullptr) {
      Expression(*initializer);
    } else if (is_const) {
      ThrowSyntax("a const declaration needs an initializer");
    } else {
      Emit(Op::PushUndefined, 0, 1);
    }
    BindTarget(*target, true, is_const);
  }
}

void Compiler::IfStatement(const Node& node) {
  const Node* test = node.Child(0);
  if (test == nullptr) {
    ThrowSyntax("malformed if");
    Emit(Op::Pop, 0, -1);
    return;
  }
  Expression(*test);
  const std::uint32_t to_else = Emit(Op::JumpIfFalse, 0, -1);
  if (node.Child(1) != nullptr) {
    Statement(*node.Child(1));
  }
  if (node.Child(2) == nullptr) {
    Patch(to_else, Here());
    return;
  }
  const std::uint32_t to_end = Emit(Op::Jump, 0, 0);
  Patch(to_else, Here());
  Statement(*node.Child(2));
  Patch(to_end, Here());
}

// --- Loops ------------------------------------------------------------------

void Compiler::WhileStatement(const Node& node, bool is_do) {
  const Node* test = node.Child(is_do ? 1 : 0);
  const Node* body = node.Child(is_do ? 0 : 1);
  if (test == nullptr || body == nullptr) {
    ThrowSyntax("malformed loop");
    Emit(Op::Pop, 0, -1);
    return;
  }

  LoopContext loop;
  loop.label = std::move(pending_label_);
  pending_label_.clear();
  loop.break_stack = loop.continue_stack = stack_depth_;
  loop.break_scopes = loop.continue_scopes = scope_depth_;
  loop.break_iterations = loop.continue_iterations = iteration_depth_;
  loop.finally_depth = finallys_.size();
  loops_.push_back(std::move(loop));

  const std::uint32_t top = Here();
  std::uint32_t to_end = 0;
  if (!is_do) {
    Expression(*test);
    to_end = Emit(Op::JumpIfFalse, 0, -1);
  }
  Statement(*body);
  const std::uint32_t continue_target = Here();
  if (is_do) {
    Expression(*test);
    const std::uint32_t again = Emit(Op::JumpIfTrue, 0, -1);
    Patch(again, top);
  } else {
    Patch(Emit(Op::Jump, 0, 0), top);
    Patch(to_end, Here());
  }

  const LoopContext done = std::move(loops_.back());
  loops_.pop_back();
  PatchAll(done.continue_jumps, continue_target);
  PatchAll(done.break_jumps, Here());
}

void Compiler::ForStatement(const Node& node) {
  // The loop's own scope holds the initializer's bindings, so `for (let i=0;;)`
  // has one `i` for the whole loop -- which is what the tree-walker does, and
  // is why a closure made in the body sees the last value.
  std::string label = std::move(pending_label_);
  pending_label_.clear();

  scopes_.emplace_back();
  if (node.Child(0) != nullptr && node.Child(0)->kind == NodeKind::VariableDeclaration) {
    for (const NodePtr& declarator : node.Child(0)->children) {
      if (declarator != nullptr && declarator->Child(0) != nullptr) {
        ReservePattern(*declarator->Child(0));
      }
    }
  }
  Emit(Op::PushScope, scopes_.back().count, 0);
  ++scope_depth_;
  if (node.Child(0) != nullptr) {
    Statement(*node.Child(0));
  }

  LoopContext loop;
  loop.label = std::move(label);
  loop.break_stack = loop.continue_stack = stack_depth_;
  loop.break_scopes = loop.continue_scopes = scope_depth_;
  loop.break_iterations = loop.continue_iterations = iteration_depth_;
  loop.finally_depth = finallys_.size();
  loops_.push_back(std::move(loop));

  const std::uint32_t top = Here();
  std::uint32_t to_end = 0;
  bool has_test = false;
  if (node.Child(1) != nullptr) {
    Expression(*node.Child(1));
    to_end = Emit(Op::JumpIfFalse, 0, -1);
    has_test = true;
  }
  if (node.Child(3) != nullptr) {
    Statement(*node.Child(3));
  }
  const std::uint32_t continue_target = Here();
  if (node.Child(2) != nullptr) {
    Expression(*node.Child(2));
    Emit(Op::Pop, 0, -1);
  }
  Patch(Emit(Op::Jump, 0, 0), top);
  if (has_test) {
    Patch(to_end, Here());
  }

  const LoopContext done = std::move(loops_.back());
  loops_.pop_back();
  PatchAll(done.continue_jumps, continue_target);
  PatchAll(done.break_jumps, Here());

  Emit(Op::PopScope, 1, 0);
  --scope_depth_;
  scopes_.pop_back();
}

void Compiler::ForInStatement(const Node& node) {
  const Node* left = node.Child(0);
  const Node* right = node.Child(1);
  const Node* body = node.Child(2);
  if (left == nullptr || right == nullptr || body == nullptr) {
    ThrowSyntax("malformed for-in");
    Emit(Op::Pop, 0, -1);
    return;
  }
  std::string label = std::move(pending_label_);
  pending_label_.clear();

  Expression(*right);
  if (node.string != "of") {
    // `for...in` enumerates keys and has no protocol behind it, so the keys are
    // collected up front and then walked as an array. `for...of` runs the
    // iteration protocol, which is observable -- an iterator whose `next` has
    // side effects must not be stepped past a `break`.
    Emit(Op::ForInKeys);
  }
  Emit(Op::IterateOpen, 0, -1);
  ++iteration_depth_;

  LoopContext loop;
  loop.label = std::move(label);
  loop.break_stack = loop.continue_stack = stack_depth_;
  loop.break_scopes = scope_depth_;
  loop.break_iterations = iteration_depth_ - 1;  // a break closes the cursor
  loop.continue_scopes = scope_depth_ + 1;       // continue is inside the iteration scope
  loop.continue_iterations = iteration_depth_;
  loop.finally_depth = finallys_.size();
  loops_.push_back(std::move(loop));

  scopes_.emplace_back();
  if (left->kind == NodeKind::VariableDeclaration) {
    const Node* declarator = left->Child(0);
    if (declarator != nullptr && declarator->Child(0) != nullptr) {
      ReservePattern(*declarator->Child(0));
    }
  }
  const std::uint32_t top = Here();
  const std::uint32_t to_end = Emit(Op::IterateNext, 0, 1);
  Emit(Op::PushScope, scopes_.back().count, 0);
  ++scope_depth_;
  if (left->kind == NodeKind::VariableDeclaration) {
    const Node* declarator = left->Child(0);
    const Node* target = declarator == nullptr ? nullptr : declarator->Child(0);
    if (target == nullptr) {
      Emit(Op::Pop, 0, -1);
    } else {
      BindTarget(*target, true, left->string == "const");
    }
  } else {
    BindTarget(*left, false, false);
  }
  Statement(*body);
  const std::uint32_t continue_target = Here();
  Emit(Op::PopScope, 1, 0);
  --scope_depth_;
  scopes_.pop_back();
  Patch(Emit(Op::Jump, 0, 0), top);

  Patch(to_end, Here());
  const LoopContext done = std::move(loops_.back());
  loops_.pop_back();
  PatchAll(done.continue_jumps, continue_target);
  Emit(Op::IterateClose, 1, 0);
  --iteration_depth_;
  PatchAll(done.break_jumps, Here());
}

// --- Leaving --------------------------------------------------------------

void Compiler::BreakOrContinue(const Node& node, bool is_break) {
  std::size_t found = loops_.size();
  for (std::size_t i = loops_.size(); i > 0; --i) {
    const LoopContext& loop = loops_[i - 1];
    if (!is_break && !loop.is_loop) {
      continue;  // a switch is breakable, not continuable
    }
    if (node.string.empty() || node.string == loop.label) {
      found = i - 1;
      break;
    }
  }
  if (found == loops_.size()) {
    // A label that names nothing enclosing. The tree-walker lets the completion
    // escape to the top, where it is silently dropped; saying so is better.
    ThrowSyntax(std::string(is_break ? "break" : "continue") + " is not inside a loop");
    Emit(Op::Pop, 0, -1);
    return;
  }

  const std::uint32_t stack_before = stack_depth_;
  const std::uint32_t scopes_before = scope_depth_;
  const std::uint32_t iterations_before = iteration_depth_;
  RunFinalizers(loops_[found].finally_depth);
  const LoopContext& loop = loops_[found];
  UnwindTo(is_break ? loop.break_stack : loop.continue_stack,
           is_break ? loop.break_scopes : loop.continue_scopes,
           is_break ? loop.break_iterations : loop.continue_iterations);
  const std::uint32_t jump = Emit(Op::Jump, 0, 0);
  if (is_break) {
    loops_[found].break_jumps.push_back(jump);
  } else {
    loops_[found].continue_jumps.push_back(jump);
  }
  // Everything after this is reached by a path that did not jump.
  stack_depth_ = stack_before;
  scope_depth_ = scopes_before;
  iteration_depth_ = iterations_before;
}

void Compiler::ReturnStatement(const Node& node) {
  const std::uint32_t stack_before = stack_depth_;
  const std::uint32_t scopes_before = scope_depth_;
  const std::uint32_t iterations_before = iteration_depth_;
  if (node.Child(0) != nullptr) {
    Expression(*node.Child(0));
  } else {
    Emit(Op::PushUndefined, 0, 1);
  }
  // Every enclosing finalizer runs before the frame goes. The return value sits
  // under whatever they push and pop, which is why the depth is tracked rather
  // than assumed.
  RunFinalizers(0);
  Emit(Op::Return, 0, -1);
  // What follows is reached by a path that did not return.
  stack_depth_ = stack_before;
  scope_depth_ = scopes_before;
  iteration_depth_ = iterations_before;
}

// --- try / catch / finally --------------------------------------------------

void Compiler::TryStatement(const Node& node) {
  const Node* block = node.Child(0);
  const Node* catch_param = node.Child(1);
  const Node* catch_body = node.Child(2);
  const Node* finally_body = node.Child(3);

  const std::uint32_t entry_stack = stack_depth_;
  const std::uint32_t entry_scopes = scope_depth_;
  const std::uint32_t entry_iterations = iteration_depth_;

  if (finally_body != nullptr) {
    finallys_.push_back(FinallyContext{finally_body, entry_scopes, entry_iterations});
  }

  const std::uint32_t try_begin = Here();
  if (block != nullptr) {
    Statement(*block);
  }
  const std::uint32_t try_end = Here();

  // The normal path runs the finalizer and leaves.
  if (finally_body != nullptr) {
    const std::size_t depth = finallys_.size() - 1;
    RunFinalizers(depth);
  }
  std::vector<std::uint32_t> to_end{Emit(Op::Jump, 0, 0)};

  std::uint32_t catch_begin = 0;
  std::uint32_t catch_end = 0;
  if (catch_body != nullptr) {
    catch_begin = Here();
    // The thrown value arrives on the stack, put there by the unwinder.
    stack_depth_ = entry_stack + 1;
    scope_depth_ = entry_scopes;
    iteration_depth_ = entry_iterations;
    scopes_.emplace_back();
    if (catch_param != nullptr) {
      ReservePattern(*catch_param);
    }
    Emit(Op::PushScope, scopes_.back().count, 0);
    ++scope_depth_;
    if (catch_param != nullptr) {
      BindTarget(*catch_param, true, false);
    } else {
      Emit(Op::Pop, 0, -1);
    }
    Statement(*catch_body);
    Emit(Op::PopScope, 1, 0);
    --scope_depth_;
    scopes_.pop_back();
    catch_end = Here();
    if (finally_body != nullptr) {
      RunFinalizers(finallys_.size() - 1);
    }
    to_end.push_back(Emit(Op::Jump, 0, 0));
  }

  std::uint32_t rethrow = 0;
  if (finally_body != nullptr) {
    // Where a throw that nothing caught goes: run the finalizer, then throw the
    // same value on. `try { throw x } finally { return 1 }` returns 1 because
    // the finalizer's own return is simply reached first.
    rethrow = Here();
    stack_depth_ = entry_stack + 1;
    scope_depth_ = entry_scopes;
    iteration_depth_ = entry_iterations;
    RunFinalizers(finallys_.size() - 1);
    Emit(Op::ThrowOp, 0, -1);
    finallys_.pop_back();
  }

  PatchAll(to_end, Here());
  stack_depth_ = entry_stack;
  scope_depth_ = entry_scopes;
  iteration_depth_ = entry_iterations;

  // Order matters: the first handler that covers the instruction wins, so the
  // catch clause is registered before the finalizer that surrounds it.
  if (catch_body != nullptr && try_end > try_begin) {
    function_.handlers.push_back(
        Handler{try_begin, try_end, catch_begin, entry_stack, entry_scopes, entry_iterations});
  }
  if (finally_body != nullptr) {
    if (catch_body == nullptr && try_end > try_begin) {
      function_.handlers.push_back(
          Handler{try_begin, try_end, rethrow, entry_stack, entry_scopes, entry_iterations});
    }
    if (catch_body != nullptr && catch_end > catch_begin) {
      // A throw from inside the catch clause still owes the finalizer.
      function_.handlers.push_back(
          Handler{catch_begin, catch_end, rethrow, entry_stack, entry_scopes, entry_iterations});
    }
  }
}

// --- switch -----------------------------------------------------------------

void Compiler::SwitchStatement(const Node& node) {
  const Node* discriminant = node.Child(0);
  if (discriminant == nullptr) {
    ThrowSyntax("malformed switch");
    Emit(Op::Pop, 0, -1);
    return;
  }
  Expression(*discriminant);
  // One scope for the whole switch, holding what every clause declares --
  // which is exactly the case two passes exist for, since entering at the
  // second clause skips the first one's `let`.
  scopes_.emplace_back();
  for (std::size_t i = 1; i < node.children.size(); ++i) {
    const Node* clause = node.Child(i);
    if (clause != nullptr) {
      ReserveDeclarations(*clause);
    }
  }
  Emit(Op::PushScope, scopes_.back().count, 0);
  ++scope_depth_;

  LoopContext context;
  context.is_loop = false;  // breakable, not continuable
  context.break_stack = stack_depth_ - 1;  // the subject goes too
  context.break_scopes = scope_depth_ - 1;
  context.break_iterations = iteration_depth_;
  context.finally_depth = finallys_.size();
  loops_.push_back(std::move(context));

  // Two passes, the way the tree-walker does it: find where to enter, then run
  // from there to the end or a break. `default` is only taken when nothing
  // matched, however early it appears, and execution still falls through into
  // what follows it.
  std::vector<std::uint32_t> entries;
  std::vector<std::size_t> clauses;
  std::size_t default_clause = node.children.size();
  for (std::size_t i = 1; i < node.children.size(); ++i) {
    const Node* clause = node.Child(i);
    if (clause == nullptr) {
      continue;
    }
    if (clause->Child(0) == nullptr) {
      default_clause = i;
      clauses.push_back(i);
      entries.push_back(0);
      continue;
    }
    Emit(Op::Dup, 0, 1);
    Expression(*clause->Child(0));
    Emit(Op::Binary, static_cast<std::uint32_t>(BinaryOp::StrictEqual), -1);
    entries.push_back(Emit(Op::JumpIfTrue, 0, -1));
    clauses.push_back(i);
  }
  const std::uint32_t to_default = Emit(Op::Jump, 0, 0);

  std::vector<std::uint32_t> bodies(clauses.size(), 0);
  for (std::size_t k = 0; k < clauses.size(); ++k) {
    bodies[k] = Here();
    const Node* clause = node.Child(clauses[k]);
    if (clause == nullptr) {
      continue;
    }
    for (std::size_t j = 1; j < clause->children.size(); ++j) {
      const Node* statement = clause->Child(j);
      if (statement != nullptr) {
        Statement(*statement);
      }
    }
  }
  const std::uint32_t after_bodies = Here();
  for (std::size_t k = 0; k < clauses.size(); ++k) {
    if (clauses[k] != default_clause) {
      Patch(entries[k], bodies[k]);
    }
  }
  std::uint32_t default_target = after_bodies;
  for (std::size_t k = 0; k < clauses.size(); ++k) {
    if (clauses[k] == default_clause) {
      default_target = bodies[k];
    }
  }
  Patch(to_default, default_target);

  Emit(Op::PopScope, 1, 0);
  --scope_depth_;
  scopes_.pop_back();
  Emit(Op::Pop, 0, -1);  // the subject

  const LoopContext done = std::move(loops_.back());
  loops_.pop_back();
  PatchAll(done.break_jumps, Here());
}

void Compiler::LabeledStatement(const Node& node) {
  const Node* body = node.Child(0);
  if (body == nullptr) {
    return;
  }
  const bool is_loop = body->kind == NodeKind::While || body->kind == NodeKind::DoWhile ||
                       body->kind == NodeKind::For || body->kind == NodeKind::ForIn;
  if (is_loop) {
    // Handed to the loop below, which takes it as its own. A labelled
    // `continue` names a loop, not a label, and the loop is the only thing that
    // can act on it.
    pending_label_ = node.string;
    Statement(*body);
    pending_label_.clear();
    return;
  }
  LoopContext context;
  context.label = node.string;
  context.is_loop = false;
  context.break_stack = stack_depth_;
  context.break_scopes = scope_depth_;
  context.break_iterations = iteration_depth_;
  context.finally_depth = finallys_.size();
  loops_.push_back(std::move(context));
  Statement(*body);
  const LoopContext done = std::move(loops_.back());
  loops_.pop_back();
  PatchAll(done.break_jumps, Here());
}

// --- Patterns ---------------------------------------------------------------

void Compiler::StoreToMember(const Node& target) {
  // The value is already on top; the member's own operands go under it. Two
  // rotations rather than one op that reorders three slots, because this is the
  // only place that needs it and a third rotate opcode would be a permanently
  // present cost for a rarely written form.
  bool key_on_stack = false;
  std::uint32_t name = 0;
  MemberOperands(target, key_on_stack, name);
  if (key_on_stack) {
    Emit(Op::RotateDown, 2, 0);
    Emit(Op::RotateDown, 2, 0);
    Emit(Op::SetProperty, 0, -2);
  } else {
    Emit(Op::Swap);
    Emit(Op::SetPropertyName, name, -1);
  }
  Emit(Op::Pop, 0, -1);
}

void Compiler::BindTarget(const Node& target, bool declare, bool is_const) {
  const CompileDepth depth(state_, kMaxCompileDepth);
  if (depth.Exceeded()) {
    Fail();
    return;
  }
  switch (target.kind) {
    case NodeKind::Identifier:
      if (declare) {
        EmitDeclare(target.string, is_const);
      } else {
        EmitStore(target.string);
        Emit(Op::Pop, 0, -1);
      }
      return;

    case NodeKind::ArrayLiteral:
      BindArrayPattern(target, declare, is_const);
      return;

    case NodeKind::ObjectLiteral:
      BindObjectPattern(target, declare, is_const);
      return;

    case NodeKind::AssignmentPattern: {
      const Node* fallback = target.Child(1);
      if (fallback != nullptr) {
        const std::uint32_t skip = Emit(Op::JumpIfNotUndefined, 0, 0);
        Emit(Op::Pop, 0, -1);
        Expression(*fallback);
        Patch(skip, Here());
      }
      const Node* inner = target.Child(0);
      if (inner == nullptr) {
        Emit(Op::Pop, 0, -1);
      } else {
        BindTarget(*inner, declare, is_const);
      }
      return;
    }

    case NodeKind::Member:
      // A destructuring assignment that wrote to a property rather than to a
      // binding.
      StoreToMember(target);
      return;

    default:
      ThrowSyntax("invalid assignment target");
      Emit(Op::Pop, 0, -1);
      Emit(Op::Pop, 0, -1);
      return;
  }
}

void Compiler::BindArrayPattern(const Node& target, bool declare, bool is_const) {
  // Over the iteration protocol rather than by index. `const [a, b] = new
  // Set(xs)` is the same syntax as `const [a, b] = xs`, and reading indices
  // would give undefined for one of them.
  Emit(Op::IterateOpen, 0, -1);
  ++iteration_depth_;
  for (std::size_t i = 0; i < target.children.size(); ++i) {
    const Node* element = target.Child(i);
    if (element != nullptr &&
        (element->kind == NodeKind::Spread || element->kind == NodeKind::RestElement)) {
      Emit(Op::IterateRest, 0, 1);
      const Node* inner = element->Child(0);
      if (inner == nullptr) {
        Emit(Op::Pop, 0, -1);
      } else {
        BindTarget(*inner, declare, is_const);
      }
      break;
    }
    // A hole still consumes a value -- `const [, b] = xs` binds the second,
    // which it can only do by stepping past the first.
    Emit(Op::IterateStep, 0, 1);
    if (element == nullptr) {
      Emit(Op::Pop, 0, -1);
      continue;
    }
    BindTarget(*element, declare, is_const);
  }
  Emit(Op::IterateClose, 1, 0);
  --iteration_depth_;
}

void Compiler::BindObjectPattern(const Node& target, bool declare, bool is_const) {
  for (const NodePtr& property : target.children) {
    if (property == nullptr || property->kind != NodeKind::Property) {
      continue;
    }
    const Node* binding = property->Child(0);
    if (binding == nullptr) {
      continue;
    }
    Emit(Op::Dup, 0, 1);
    Emit(Op::GetPropertyName, Name(property->string), 0);
    BindTarget(*binding, declare, is_const);
  }
  Emit(Op::Pop, 0, -1);  // the object the pattern was read from
}

}  // namespace microbrowser::js

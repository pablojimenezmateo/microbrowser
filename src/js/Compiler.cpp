#include <memory>
#include <string>
#include <utility>

#include "js/CompilerImpl.h"
#include "js/TemplateParts.h"

// The compiler's emitter, and its expressions.
//
// Three translation units, split by what they compile rather than by size:
// statements and patterns in CompilerStatements.cpp, scopes and slot
// resolution in CompilerScopes.cpp, and how a function body becomes a chunk in
// CompilerFunctions.cpp. What is left here is the emitter every one of them
// calls, and the expressions. It is the same split the parser has and for the
// same reason.
//
// Two invariants hold everywhere below, and every bug found while writing this
// was a violation of one of them:
//
//   * An expression leaves exactly one value on the stack. A statement leaves
//     none. That is what makes `stack_depth_` a number the compiler can know
//     at every instruction, and knowing it is what lets a handler say where to
//     truncate and a `break` say how much to drop.
//
//   * Nothing is popped before the operation that could throw. The VM reads
//     operands in place and pops after, so a getter that runs a collection
//     mid-access finds its receiver still on the stack and therefore still a
//     root.

namespace microbrowser::js {

Compiler::Compiler(CompileState& state, CompiledFunction& function, Compiler* parent)
    : state_(state), function_(function), parent_(parent) {}

// --- Emitting ---------------------------------------------------------------

std::uint32_t Compiler::Emit(Op op, std::uint32_t a, int delta) {
  if (state_.failed) {
    return 0;
  }
  if (++state_.emitted > kMaxEmittedInstructions) {
    Fail();
    return 0;
  }
  const auto at = static_cast<std::uint32_t>(function_.code.size());
  function_.code.push_back(Instruction{op, a});
  const int depth = static_cast<int>(stack_depth_) + delta;
  if (depth < 0) {
    // The compiler and its own arithmetic disagreeing. Not reachable from a
    // page -- it is a bug in an emitter above -- but running the program would
    // read a slot belonging to someone else, so it stops here.
    Fail();
    return at;
  }
  stack_depth_ = static_cast<std::uint32_t>(depth);
  return at;
}

std::uint32_t Compiler::Here() const { return static_cast<std::uint32_t>(function_.code.size()); }

void Compiler::Patch(std::uint32_t at, std::uint32_t target) {
  if (at < function_.code.size()) {
    function_.code[at].a = target;
  }
}

void Compiler::PatchAll(const std::vector<std::uint32_t>& jumps, std::uint32_t target) {
  for (const std::uint32_t at : jumps) {
    Patch(at, target);
  }
}

std::uint32_t Compiler::Constant(Value value) {
  function_.constants.push_back(std::move(value));
  return static_cast<std::uint32_t>(function_.constants.size() - 1);
}

std::uint32_t Compiler::Name(std::string_view text) {
  const std::string key(text);
  const auto found = names_.find(key);
  if (found != names_.end()) {
    return found->second;
  }
  function_.names.push_back(key);
  function_.keys.emplace_back(key);
  const auto index = static_cast<std::uint32_t>(function_.names.size() - 1);
  names_.emplace(key, index);
  return index;
}

std::uint32_t Compiler::NodeIndex(const Node& node) {
  function_.nodes.push_back(&node);
  return static_cast<std::uint32_t>(function_.nodes.size() - 1);
}

void Compiler::Fail() { state_.failed = true; }

void Compiler::ThrowSyntax(std::string message) {
  Emit(Op::ThrowSyntaxError, Constant(Value::String(std::move(message))), 0);
  // It never returns, but the code after it is compiled as if a value arrived:
  // an expression's contract is one value, and the emitter above this one is
  // counting.
  Emit(Op::PushUndefined, 0, 1);
}

void Compiler::UnwindTo(std::uint32_t stack, std::uint32_t scopes, std::uint32_t iterations) {
  if (iteration_depth_ > iterations) {
    Emit(Op::IterateClose, iteration_depth_ - iterations, 0);
  }
  if (scope_depth_ > scopes && !function_.frame_locals) {
    // A flattened function has no scopes to pop. Leaving a block means nothing
    // at run time there: its slots stay where they are and are put back in
    // their undeclared state by the ClearLocals that re-enters it.
    Emit(Op::PopScope, scope_depth_ - scopes, 0);
  }
  for (std::uint32_t i = stack_depth_; i > stack; --i) {
    Emit(Op::Pop, 0, -1);
  }
  // The emitted code has unwound; the compiler's own idea of where it is has
  // not, because the instructions after this are reached by the path that did
  // *not* jump. Restoring is the caller's business and every caller either
  // jumps away immediately or restores by hand.
}

void Compiler::RunFinalizers(std::size_t depth) {
  // Leaves the compiler's depths where the emitted code has left them, rather
  // than restoring them. The caller is about to unwind further and would
  // otherwise emit a second set of pops for scopes this already popped -- which
  // is exactly the bug that made a `break` out of a `try/finally` inside two
  // blocks pop a scope belonging to someone else.
  if (finallys_.size() <= depth) {
    return;
  }
  // Innermost first: each one runs in the scope its own `try` was written in.
  for (std::size_t i = finallys_.size(); i > depth; --i) {
    const FinallyContext context = finallys_[i - 1];
    if (iteration_depth_ > context.iteration_depth) {
      Emit(Op::IterateClose, iteration_depth_ - context.iteration_depth, 0);
      iteration_depth_ = context.iteration_depth;
    }
    if (scope_depth_ > context.scope_depth) {
      if (!function_.frame_locals) {
        Emit(Op::PopScope, scope_depth_ - context.scope_depth, 0);
      }
      scope_depth_ = context.scope_depth;
    }
    if (context.body != nullptr) {
      // The one place code is compiled at a scope depth lower than the one it
      // is lexically written at: the finalizer runs after the blocks between it
      // and the jump have been popped. The model has to say so, or a name
      // inside the finalizer resolves two scopes further out than it lives.
      const std::size_t live = scope_floor_ + context.scope_depth;
      std::vector<CompiledScope> hidden;
      if (scopes_.size() > live) {
        hidden.assign(scopes_.begin() + static_cast<std::ptrdiff_t>(live), scopes_.end());
        scopes_.resize(live);
      }
      // Compiled with the finalizers above it removed, so a `return` inside a
      // finalizer does not emit that same finalizer again for ever.
      std::vector<FinallyContext> saved(finallys_.begin() + static_cast<std::ptrdiff_t>(i - 1),
                                        finallys_.end());
      finallys_.resize(i - 1);
      Statement(*context.body);
      finallys_.insert(finallys_.end(), saved.begin(), saved.end());
      scopes_.insert(scopes_.end(), hidden.begin(), hidden.end());
    }
  }
}

// --- Expressions ------------------------------------------------------------

void Compiler::Expression(const Node& node) {
  const CompileDepth depth(state_, kMaxCompileDepth);
  if (depth.Exceeded() || state_.failed) {
    Fail();
    return;
  }

  switch (node.kind) {
    case NodeKind::NumberLiteral:
      Emit(Op::PushConstant, Constant(Value::Number(node.number)), 1);
      return;
    case NodeKind::StringLiteral:
      Emit(Op::PushConstant, Constant(Value::String(node.string)), 1);
      return;
    case NodeKind::BooleanLiteral:
      Emit(node.number != 0.0 ? Op::PushTrue : Op::PushFalse, 0, 1);
      return;
    case NodeKind::NullLiteral:
      Emit(Op::PushNull, 0, 1);
      return;
    case NodeKind::RegExpLiteral:
      // A fresh object per evaluation, which is what makes `lastIndex` on a
      // literal inside a loop start over each time round.
      Emit(Op::RegExpLiteral, NodeIndex(node), 1);
      return;

    case NodeKind::Identifier:
      if (node.string == "undefined") {
        Emit(Op::PushUndefined, 0, 1);
        return;
      }
      EmitLoad(node.string);
      return;

    case NodeKind::ThisExpression: {
      // `this` is slot zero of a function's own scope, so inside compiled code
      // it resolves like any other name. At the top level of a program there is
      // no such scope, and the forgiving form is right there: an unbound `this`
      // is undefined rather than a ReferenceError.
      std::uint32_t packed = 0;
      if (ResolveSlot("this", packed)) {
        Emit(Op::LoadSlot, packed, 1);
      } else {
        Emit(Op::LoadThis, 0, 1);
      }
      return;
    }

    case NodeKind::TemplateLiteral:
      TemplateLiteral(node);
      return;
    case NodeKind::TaggedTemplate:
      TaggedTemplate(node);
      return;

    case NodeKind::ArrayLiteral:
      ArrayLiteral(node);
      return;
    case NodeKind::ObjectLiteral:
      ObjectLiteral(node);
      return;

    case NodeKind::FunctionExpression:
      FunctionValue(node, false);
      return;
    case NodeKind::ArrowFunction:
      FunctionValue(node, true);
      return;
    case NodeKind::ClassExpression:
      // The class is *built* by the tree-walking EvaluateClass, which holds the
      // ordering a class needs -- fields after a super() call and before the
      // constructor body, a home object per method, static members on the
      // constructor. What used to be handed over with it was the method bodies,
      // and those are compiled here now: the builder asks for each one by node.
      ClassMethods(node);
      Emit(Op::ClassLiteral, NodeIndex(node), 1);
      return;

    case NodeKind::Unary:
      Unary(node);
      return;
    case NodeKind::Update:
      Update(node);
      return;
    case NodeKind::Assignment:
      Assignment(node);
      return;

    case NodeKind::Binary: {
      const Node* left = node.Child(0);
      const Node* right = node.Child(1);
      BinaryOp op = BinaryOp::Add;
      if (left == nullptr || right == nullptr || !ParseBinaryOp(node.string, op)) {
        ThrowSyntax("malformed binary expression");
        return;
      }
      Expression(*left);
      Expression(*right);
      Emit(Op::Binary, static_cast<std::uint32_t>(op), -1);
      return;
    }

    case NodeKind::Logical: {
      const Node* left = node.Child(0);
      const Node* right = node.Child(1);
      if (left == nullptr || right == nullptr) {
        ThrowSyntax("malformed logical expression");
        return;
      }
      Expression(*left);
      // Short-circuiting, and the result is one of the operands rather than a
      // boolean -- which is why the jump peeks instead of popping.
      const Op test = node.string == "&&"   ? Op::JumpIfFalsePeek
                      : node.string == "||" ? Op::JumpIfTruePeek
                                            : Op::JumpIfNotNullish;
      const std::uint32_t skip = Emit(test, 0, 0);
      Emit(Op::Pop, 0, -1);
      Expression(*right);
      Patch(skip, Here());
      return;
    }

    case NodeKind::Conditional: {
      const Node* test = node.Child(0);
      if (test == nullptr) {
        ThrowSyntax("malformed conditional");
        return;
      }
      Expression(*test);
      const std::uint32_t to_else = Emit(Op::JumpIfFalse, 0, -1);
      const std::uint32_t before = stack_depth_;
      if (node.Child(1) != nullptr) {
        Expression(*node.Child(1));
      } else {
        Emit(Op::PushUndefined, 0, 1);
      }
      const std::uint32_t to_end = Emit(Op::Jump, 0, 0);
      Patch(to_else, Here());
      stack_depth_ = before;
      if (node.Child(2) != nullptr) {
        Expression(*node.Child(2));
      } else {
        Emit(Op::PushUndefined, 0, 1);
      }
      Patch(to_end, Here());
      return;
    }

    case NodeKind::Call:
      OptionalChain(node);
      return;
    case NodeKind::New:
      NewExpression(node);
      return;
    case NodeKind::Member:
      // Through OptionalChain whether or not this one is optional: it is what
      // knows whether a chain is already open, and a plain member inside an
      // optional chain is still part of it.
      OptionalChain(node);
      return;

    case NodeKind::Sequence: {
      bool first = true;
      for (const NodePtr& element : node.children) {
        if (element == nullptr) {
          continue;
        }
        if (!first) {
          Emit(Op::Pop, 0, -1);
        }
        Expression(*element);
        first = false;
      }
      if (first) {
        Emit(Op::PushUndefined, 0, 1);
      }
      return;
    }

    case NodeKind::NewTarget:
      Emit(Op::LoadNewTarget, 0, 1);
      return;

    case NodeKind::ImportMeta:
      // The linker bound it in the module's scope under a name no source can
      // write, so this is an ordinary load. Outside a module the name is not
      // bound, which is the ReferenceError the language gives there.
      EmitLoad("*meta*");
      return;

    case NodeKind::ImportCall:
      ImportCall(node);
      return;

    case NodeKind::Super:
      // Only reachable as `super(...)` or `super.x`, and both are handled where
      // the call and the member access are. Reaching here means it was used as
      // a value, which it is not.
      ThrowSyntax("'super' keyword unexpected here");
      return;

    case NodeKind::Yield:
      Yield(node);
      return;

    case NodeKind::Spread:
      ThrowSyntax("unsupported expression");
      return;

    default:
      // A node kind that is neither an expression nor anything this knows how
      // to reject. The tree-walker answers these, so the program goes there.
      Fail();
      return;
  }
}

void Compiler::Yield(const Node& node) {
  if (!function_.is_generator) {
    // `yield` is an expression everywhere the parser is concerned and a keyword
    // only inside a generator. Rejected here rather than there, because whether
    // it is one is a scope question -- the same arrangement `await` has.
    ThrowSyntax("yield is only valid inside a generator");
    return;
  }
  const Node* argument = node.Child(0);

  if (node.number == 0.0) {
    // `yield` and `yield x`. One instruction: the value goes out, and what the
    // next `next(v)` sends in arrives in its place.
    if (argument == nullptr) {
      Emit(Op::PushUndefined, 0, 1);
    } else {
      Expression(*argument);
    }
    if (function_.is_async) {
      // `yield` in an async generator awaits its operand before handing it
      // over, which is what makes `yield fetch(x)` give the page the response
      // rather than the promise. Two instructions rather than a third kind of
      // suspend: the Await files the frame and a microtask puts it back, and
      // the Yield that follows then has a value to settle the request with.
      Emit(Op::Await, 0, 0);
    }
    Emit(Op::Yield, 0, 0);
    return;
  }

  // `yield* xs`: a loop over the delegate's iterator, with the three ways a
  // resume can arrive all forwarded to it.
  //
  // The loop body is one instruction and one suspend. What makes it a
  // *relationship* rather than a plain loop is the handler registered over the
  // `yield`: a `g.throw(e)` or a `g.return(v)` that lands there is caught and
  // handed to the delegate's own `throw` or `return`, so an inner generator's
  // `catch` sees the throw and its `finally` runs on the return. Without it
  // both stopped at the outer generator, which is a difference a page writing
  // a pipeline of generators notices immediately.
  if (argument == nullptr) {
    Emit(Op::PushUndefined, 0, 1);
  } else {
    Expression(*argument);
  }
  Emit(Op::IterateOpen, 0, -1);
  ++iteration_depth_;
  const std::uint32_t before = stack_depth_;

  // What the first step sends in. `next(undefined)`, and thereafter whatever
  // the last resume handed over -- which is the value the delegate's own
  // `yield` expression evaluates to, and is why it is threaded rather than
  // dropped.
  Emit(Op::PushUndefined, 0, 1);
  const std::uint32_t top = Here();
  const std::uint32_t to_end = Emit(Op::IterateDelegate, 0, 0);
  const std::uint32_t protected_begin = Here();
  Emit(Op::Yield, 0, 0);
  const std::uint32_t protected_end = Here();
  Patch(Emit(Op::Jump, 0, 0), top);

  // The forwarding path, reached only by the handler below.
  const std::uint32_t forward = Here();
  stack_depth_ = before + 1;  // the unwinder pushed the thrown value
  const std::uint32_t forward_to_end = Emit(Op::IterateForward, 0, 0);
  Patch(Emit(Op::Jump, 0, 0), protected_begin);

  Patch(to_end, Here());
  Patch(forward_to_end, Here());
  // The delegate's return value is on the stack at either jump target, which
  // the fall-through path never reaches -- so the depth is set, not tracked.
  stack_depth_ = before + 1;
  Emit(Op::IterateClose, 1, 0);
  --iteration_depth_;

  // `is_finally`, because it has to catch a forced return as well as a throw:
  // those are the two things `g.return()` and `g.throw()` deliver here, and a
  // `catch`-shaped handler sees only one of them. The recorded iteration depth
  // includes the delegate's own cursor, which is the whole point -- the
  // handler must not close the thing it is about to forward to.
  const std::uint32_t live_scopes = function_.frame_locals ? 0 : scope_depth_;
  if (protected_end > protected_begin) {
    function_.handlers.push_back(Handler{protected_begin, protected_end, forward, before,
                                         live_scopes, iteration_depth_ + 1, true});
  }
}

void Compiler::Unary(const Node& node) {
  const Node* operand = node.Child(0);
  if (operand == nullptr) {
    ThrowSyntax("malformed unary expression");
    return;
  }
  if (node.string == "typeof" && operand->kind == NodeKind::Identifier) {
    // `typeof undeclared` is "undefined" rather than a ReferenceError. The one
    // place the language lets a name be read without being declared, and the
    // reason feature detection works. A name the compiler placed *is* declared,
    // so it reads normally -- and reading it before its own `let` is a
    // ReferenceError, which is what the language says too.
    std::uint32_t packed = 0;
    if (ResolveSlot(operand->string, packed)) {
      Emit(Op::LoadSlot, packed, 1);
      Emit(Op::TypeofValue);
    } else {
      Emit(Op::TypeofName, Name(operand->string), 1);
    }
    return;
  }
  if (node.string == "delete") {
    if (operand->kind == NodeKind::Member) {
      bool key_on_stack = false;
      std::uint32_t name = 0;
      MemberOperands(*operand, key_on_stack, name);
      if (!key_on_stack) {
        Emit(Op::PushConstant, Constant(Value::String(function_.names[name])), 1);
      }
      Emit(Op::DeleteProperty, 0, -1);
      return;
    }
    Emit(Op::PushTrue, 0, 1);
    return;
  }

  Expression(*operand);
  if (node.string == "!") {
    Emit(Op::Not);
  } else if (node.string == "-") {
    Emit(Op::Negate);
  } else if (node.string == "+") {
    Emit(Op::UnaryPlus);
  } else if (node.string == "~") {
    Emit(Op::BitNot);
  } else if (node.string == "typeof") {
    Emit(Op::TypeofValue);
  } else if (node.string == "void") {
    Emit(Op::Discard);
  } else if (node.string == "await") {
    if (!function_.is_async) {
      // `await` is a unary operator everywhere the parser is concerned and a
      // keyword only inside an async function. Rejected here rather than
      // there, because whether it is one is a scope question -- the same
      // arrangement `super` has.
      Emit(Op::Pop, 0, -1);
      ThrowSyntax("await is only valid inside an async function");
      return;
    }
    Emit(Op::Await);
  } else {
    Emit(Op::Pop, 0, -1);
    ThrowSyntax("unsupported unary operator '" + node.string + "'");
  }
}

void Compiler::Update(const Node& node) {
  const Node* operand = node.Child(0);
  if (operand == nullptr) {
    ThrowSyntax("malformed update expression");
    return;
  }
  const bool prefix = node.number == 1.0;
  const BinaryOp step = node.string == "++" ? BinaryOp::Add : BinaryOp::Subtract;

  if (operand->kind == NodeKind::Member) {
    bool key_on_stack = false;
    std::uint32_t name = 0;
    MemberOperands(*operand, key_on_stack, name);
    if (key_on_stack) {
      Emit(Op::Dup2, 0, 2);
      Emit(Op::GetProperty, 0, -1);
    } else {
      Emit(Op::Dup, 0, 1);
      Emit(Op::GetPropertyName, name, 0);
    }
    Emit(Op::ToNumberOp);
    if (!prefix) {
      // The old value has to survive the store, so it goes underneath the
      // store's operands rather than being recomputed.
      Emit(Op::Dup, 0, 1);
      Emit(Op::RotateDown, key_on_stack ? 3 : 2, 0);
    }
    Emit(Op::PushConstant, Constant(Value::Number(1.0)), 1);
    Emit(Op::Binary, static_cast<std::uint32_t>(step), -1);
    Emit(key_on_stack ? Op::SetProperty : Op::SetPropertyName, name, key_on_stack ? -2 : -1);
    if (!prefix) {
      Emit(Op::Pop, 0, -1);
    }
    return;
  }

  if (operand->kind != NodeKind::Identifier) {
    // `++f(n)` is not an assignment target -- but the operand is evaluated
    // before anyone finds out, which for `++++...++f(n-3)` is the difference
    // between a RangeError from the recursion and a SyntaxError from the
    // sixtieth operator. The tree-walker evaluates first because it has to;
    // this has to be told to.
    Expression(*operand);
    Emit(Op::Pop, 0, -1);
    ThrowSyntax("invalid assignment target");
    return;
  }
  EmitLoad(operand->string);
  Emit(Op::ToNumberOp);
  if (!prefix) {
    Emit(Op::Dup, 0, 1);
  }
  Emit(Op::PushConstant, Constant(Value::Number(1.0)), 1);
  Emit(Op::Binary, static_cast<std::uint32_t>(step), -1);
  EmitStore(operand->string);
  if (!prefix) {
    Emit(Op::Pop, 0, -1);
  }
}

void Compiler::MemberOperands(const Node& node, bool& key_on_stack, std::uint32_t& name) {
  const Node* object = node.Child(0);
  const Node* property = node.Child(1);
  if (object == nullptr || property == nullptr) {
    ThrowSyntax("malformed member access");
    key_on_stack = false;
    name = Name("");
    return;
  }
  if (object->kind == NodeKind::Super) {
    // `super.x` reads from the prototype of the object the *method* was defined
    // on, while `this` stays the receiver. Reading from the receiver's
    // prototype instead is what makes a three-level hierarchy call itself.
    Emit(Op::LoadSuperBase, 0, 1);
    key_on_stack = (static_cast<std::uint8_t>(node.number) & kMemberComputed) != 0;
    if (key_on_stack) {
      Expression(*property);
      name = 0;
    } else {
      name = Name(property->string);
    }
    return;
  }
  Expression(*object);
  key_on_stack = (static_cast<std::uint8_t>(node.number) & kMemberComputed) != 0;
  if (key_on_stack) {
    // Left as it evaluated rather than stringified: `o[Symbol.iterator]` must
    // stay a symbol key, which is a key no page can write out.
    //
    // The chain is suspended across it: `a?.b[c?.d]` has two chains, and the
    // inner one's short-circuit belongs to the key rather than to `a`.
    std::vector<ChainExit>* outer = chain_;
    chain_ = nullptr;
    Expression(*property);
    chain_ = outer;
    name = 0;
    return;
  }
  name = Name(property->string);
}

void Compiler::OptionalChain(const Node& node) {
  if (chain_ != nullptr) {
    // Already inside one. This node is a link rather than a root -- the mark
    // says which, and a nested chain would only arise from a key or an
    // argument, both of which suspend the outer one.
    if (node.kind == NodeKind::Member) {
      MemberExpression(node);
    } else {
      CallExpression(node);
    }
    return;
  }
  std::vector<ChainExit> exits;
  chain_ = &exits;
  if (node.kind == NodeKind::Member) {
    MemberExpression(node);
  } else {
    CallExpression(node);
  }
  chain_ = nullptr;
  if (exits.empty()) {
    return;
  }
  // Every exit unwinds to the same place: whatever the chain would have left
  // behind, replaced by undefined. `merged` is the depth the chain actually
  // finished at, and each exit pops down to one below it.
  const std::uint32_t merged = stack_depth_;
  std::vector<std::uint32_t> to_end{Emit(Op::Jump, 0, 0)};
  for (const ChainExit& exit : exits) {
    Patch(exit.jump, Here());
    stack_depth_ = exit.depth;
    while (stack_depth_ + 1 > merged) {
      Emit(Op::Pop, 0, -1);
    }
    Emit(Op::PushUndefined, 0, 1);
    to_end.push_back(Emit(Op::Jump, 0, 0));
  }
  PatchAll(to_end, Here());
  stack_depth_ = merged;
}

void Compiler::MemberExpression(const Node& node) {
  bool key_on_stack = false;
  std::uint32_t name = 0;
  MemberOperands(node, key_on_stack, name);
  const auto flags = static_cast<std::uint8_t>(node.number);
  const bool optional = (flags & kMemberOptional) != 0;
  if (optional && chain_ != nullptr) {
    // `a?.b`. The jump goes to the end of the *chain*, not to the end of this
    // access -- so `a?.b.c` gives up on the whole expression rather than
    // reading `.c` off undefined.
    if (key_on_stack) {
      Emit(Op::Swap);
      chain_->push_back(ChainExit{Emit(Op::JumpIfNullish, 0, 0), stack_depth_});
      Emit(Op::Swap);
    } else {
      chain_->push_back(ChainExit{Emit(Op::JumpIfNullish, 0, 0), stack_depth_});
    }
  } else if (key_on_stack) {
    Emit(Op::ThrowIfNullishKey);
  } else {
    Emit(Op::ThrowIfNullishName, name, 0);
  }
  if (key_on_stack) {
    Emit(Op::GetProperty, 0, -1);
  } else {
    Emit(Op::GetPropertyName, name, 0);
  }
}

bool Compiler::CallArguments(const Node& node, std::size_t first, std::uint32_t& count) {
  // An argument is its own expression: an optional chain written inside one
  // short-circuits to the end of *that* chain, not to the end of the call it
  // is an argument to.
  std::vector<ChainExit>* outer = chain_;
  chain_ = nullptr;
  struct Restore {
    std::vector<ChainExit>*& slot;
    std::vector<ChainExit>* value;
    ~Restore() { slot = value; }
  } restore{chain_, outer};
  bool spread = false;
  for (std::size_t i = first; i < node.children.size(); ++i) {
    const Node* argument = node.Child(i);
    if (argument != nullptr && argument->kind == NodeKind::Spread) {
      spread = true;
      break;
    }
  }
  if (!spread) {
    count = 0;
    for (std::size_t i = first; i < node.children.size(); ++i) {
      const Node* argument = node.Child(i);
      if (argument == nullptr) {
        continue;
      }
      Expression(*argument);
      ++count;
    }
    return false;
  }
  // One array, built the way an array literal is -- which is what makes
  // `f(...new Set(xs))` and `f(...'abc')` ordinary calls rather than a special
  // case of one.
  Emit(Op::NewArray, 0, 1);
  for (std::size_t i = first; i < node.children.size(); ++i) {
    const Node* argument = node.Child(i);
    if (argument == nullptr) {
      continue;
    }
    if (argument->kind == NodeKind::Spread) {
      const Node* inner = argument->Child(0);
      if (inner == nullptr) {
        continue;
      }
      Expression(*inner);
      Emit(Op::ArraySpread, 0, -1);
      continue;
    }
    Expression(*argument);
    Emit(Op::ArrayPush, 0, -1);
  }
  count = 0;
  return true;
}

void Compiler::CallExpression(const Node& node) {
  const Node* callee = node.Child(0);
  if (callee == nullptr) {
    ThrowSyntax("malformed call");
    return;
  }
  if (callee->kind == NodeKind::Super) {
    // `super(...)` runs the parent constructor against the instance already
    // being built rather than making a second one, and then initializes this
    // class's own fields -- which is the ordering that lets a derived field
    // read a base one.
    std::uint32_t count = 0;
    if (CallArguments(node, 1, count)) {
      ThrowSyntax("a spread argument to super() is not supported");
      return;
    }
    Emit(Op::SuperCall, count, -static_cast<int>(count));
    Emit(Op::PushUndefined, 0, 1);
    return;
  }

  if (callee->kind == NodeKind::Member && callee->Child(0) != nullptr &&
      callee->Child(0)->kind == NodeKind::Super) {
    // `super.m()` is the one call whose lookup and whose receiver come from
    // different places: the method is found on the prototype of the object this
    // method was *defined* on, and it runs with the instance as `this`. Those
    // being two things is the whole reason `super` exists -- binding the
    // receiver to the base instead would make an override invisible to itself.
    const Node* property = callee->Child(1);
    if (property == nullptr) {
      ThrowSyntax("malformed member access");
      return;
    }
    Emit(Op::LoadSuperBase, 0, 1);
    if (callee->number == 1.0) {
      Expression(*property);
      Emit(Op::GetProperty, 0, -1);
    } else {
      Emit(Op::GetPropertyName, Name(property->string), 0);
    }
    std::uint32_t packed = 0;
    if (ResolveSlot("this", packed)) {
      Emit(Op::LoadSlot, packed, 1);
    } else {
      Emit(Op::LoadThis, 0, 1);
    }
    std::uint32_t count = 0;
    if (CallArguments(node, 1, count)) {
      Emit(Op::CallApply, 0, -2);
    } else {
      Emit(Op::Call, count, -static_cast<int>(count) - 1);
    }
    return;
  }

  if (callee->kind == NodeKind::Member) {
    // A method call's receiver is the object it was read from, and that object
    // must be evaluated once -- `f().g()` calls f once.
    bool key_on_stack = false;
    std::uint32_t name = 0;
    MemberOperands(*callee, key_on_stack, name);
    if ((static_cast<std::uint8_t>(callee->number) & kMemberOptional) != 0 &&
        chain_ != nullptr) {
      if (key_on_stack) {
        Emit(Op::Swap);
        chain_->push_back(ChainExit{Emit(Op::JumpIfNullish, 0, 0), stack_depth_});
        Emit(Op::Swap);
      } else {
        chain_->push_back(ChainExit{Emit(Op::JumpIfNullish, 0, 0), stack_depth_});
      }
    } else if (key_on_stack) {
      Emit(Op::ThrowIfNullishKey);
    } else {
      Emit(Op::ThrowIfNullishName, name, 0);
    }
    if (key_on_stack) {
      Emit(Op::Dup2, 0, 2);
      Emit(Op::GetProperty, 0, -1);  // [base key callee]
      Emit(Op::RotateDown, 2, 0);    // [callee base key]
      Emit(Op::Pop, 0, -1);          // [callee base]
    } else {
      Emit(Op::Dup, 0, 1);
      Emit(Op::GetPropertyName, name, 0);  // [base callee]
      Emit(Op::Swap);                      // [callee base]
    }
  } else {
    Expression(*callee);
    Emit(Op::PushUndefined, 0, 1);  // called plainly: no receiver
  }

  if ((static_cast<std::uint8_t>(node.number) & kCallOptional) != 0 && chain_ != nullptr) {
    // `f?.()`. The receiver is on top by now, so the callee is brought up to be
    // tested and put back.
    Emit(Op::Swap);
    chain_->push_back(ChainExit{Emit(Op::JumpIfNullish, 0, 0), stack_depth_});
    Emit(Op::Swap);
  }

  std::uint32_t count = 0;
  const bool spread = CallArguments(node, 1, count);
  if (spread) {
    Emit(Op::CallApply, 0, -2);
  } else {
    Emit(Op::Call, count, -static_cast<int>(count) - 1);
  }

}

void Compiler::NewExpression(const Node& node) {
  const Node* callee = node.Child(0);
  if (callee == nullptr) {
    ThrowSyntax("malformed new expression");
    return;
  }
  Expression(*callee);
  std::uint32_t count = 0;
  const bool spread = CallArguments(node, 1, count);
  if (spread) {
    Emit(Op::NewApply, 0, -1);
  } else {
    Emit(Op::New, count, -static_cast<int>(count));
  }
}

void Compiler::ArrayLiteral(const Node& node) {
  Emit(Op::NewArray, 0, 1);
  for (const NodePtr& element : node.children) {
    if (element == nullptr) {
      Emit(Op::ArrayHole, 0, 0);  // a hole reads as undefined but is not present
      continue;
    }
    if (element->kind == NodeKind::Spread) {
      const Node* inner = element->Child(0);
      if (inner == nullptr) {
        continue;
      }
      Expression(*inner);
      Emit(Op::ArraySpread, 0, -1);
      continue;
    }
    Expression(*element);
    Emit(Op::ArrayPush, 0, -1);
  }
}

void Compiler::ObjectLiteral(const Node& node) {
  Emit(Op::NewObject, 0, 1);
  for (const NodePtr& property : node.children) {
    if (property == nullptr) {
      continue;
    }
    if (property->kind == NodeKind::Spread) {
      const Node* inner = property->Child(0);
      if (inner == nullptr) {
        continue;
      }
      Expression(*inner);
      Emit(Op::ObjectSpread, 0, -1);
      continue;
    }
    // The parser packs three independent facts into `number`: whether the key
    // was computed, and whether this is a getter or a setter.
    const auto flags = static_cast<int>(property->number);
    const bool computed = (flags & 1) != 0;
    const bool is_getter = (flags & 2) != 0;
    const bool is_setter = (flags & 4) != 0;
    const Node* value = property->Child(0);
    if (value == nullptr) {
      continue;
    }
    if (computed) {
      const Node* key = property->Child(1);
      if (key == nullptr) {
        continue;
      }
      // Whatever it evaluated to, not its text: stringifying here would file
      // `{ [Symbol.iterator]() {} }` under a name a page can write out, and
      // not colliding is the entire reason symbols exist.
      Expression(*key);
      Expression(*value);
      if (is_getter) {
        Emit(Op::ObjectGetter, 0, -2);
      } else if (is_setter) {
        Emit(Op::ObjectSetter, 0, -2);
      } else {
        Emit(Op::ObjectSet, 0, -2);
      }
      continue;
    }
    if (is_getter || is_setter) {
      Emit(Op::PushConstant, Constant(Value::String(property->string)), 1);
      Expression(*value);
      Emit(is_getter ? Op::ObjectGetter : Op::ObjectSetter, 0, -2);
      continue;
    }
    Expression(*value);
    Emit(Op::ObjectSetName, Name(property->string), -1);
  }
}

void Compiler::ImportCall(const Node& node) {
  // `import(spec)`. Compiled as a call to the host hook the linker installed,
  // which is what keeps a *dynamic* import -- whose specifier is not known
  // until it runs -- from needing anything of the machine that a static one
  // does not.
  EmitLoad("*import*");
  Emit(Op::PushUndefined, 0, 1);  // no receiver
  if (node.Child(0) == nullptr) {
    Emit(Op::PushUndefined, 0, 1);
  } else {
    Expression(*node.Child(0));
  }
  Emit(Op::Call, 1, -2);
}

void Compiler::TemplateLiteral(const Node& node) {
  // Compiled to concatenation, with the literal chunks split once here rather
  // than re-split at every evaluation. The left operand of every `+` below is a
  // string by construction -- the first chunk, then the running result -- so
  // the operator's string case is the one that always applies, and each
  // substitution goes through the one ToString the language defines.
  const TemplateParts parts = SplitTemplate(node.string);
  Emit(Op::PushConstant,
       Constant(Value::String(parts.literals.empty() ? std::string() : parts.literals[0])), 1);
  for (std::size_t i = 0; i < parts.literals.size(); ++i) {
    if (i > 0) {
      Emit(Op::PushConstant, Constant(Value::String(parts.literals[i])), 1);
      Emit(Op::Binary, static_cast<std::uint32_t>(BinaryOp::Add), -1);
    }
    if (i >= node.children.size()) {
      continue;  // the last chunk has no substitution after it
    }
    const Node* substitution = node.Child(i);
    if (substitution == nullptr) {
      continue;  // an empty `${}` contributes nothing
    }
    Expression(*substitution);
    // ToString before the concatenation rather than through it. `+` would
    // convert with the default hint and try `valueOf` first; a substitution is
    // ToString and tries `toString` first, and an object with both is written
    // that way on purpose.
    Emit(Op::ToStringOp, 0, 0);
    Emit(Op::Binary, static_cast<std::uint32_t>(BinaryOp::Add), -1);
  }
}

void Compiler::TaggedTemplate(const Node& node) {
  // ``tag`a${x}b` `` is `tag(["a", "b"], x)`. The tag sees the literal chunks
  // and the substitution values apart, which is the whole point of the form:
  // it can escape an interpolation it did not write.
  const Node* tag = node.Child(0);
  const Node* literal = node.Child(1);
  if (tag == nullptr || literal == nullptr) {
    ThrowSyntax("malformed tagged template");
    return;
  }
  if (tag->kind == NodeKind::Member) {
    bool key_on_stack = false;
    std::uint32_t name = 0;
    MemberOperands(*tag, key_on_stack, name);
    if (key_on_stack) {
      Emit(Op::Dup2, 0, 2);
      Emit(Op::GetProperty, 0, -1);  // [base key callee]
      Emit(Op::RotateDown, 2, 0);    // [callee base key]
      Emit(Op::Pop, 0, -1);          // [callee base]
    } else {
      Emit(Op::Dup, 0, 1);
      Emit(Op::GetPropertyName, name, 0);
      Emit(Op::Swap);
    }
  } else {
    Expression(*tag);
    Emit(Op::PushUndefined, 0, 1);
  }
  Emit(Op::TemplateStrings, NodeIndex(*literal), 1);
  std::uint32_t count = 1;
  for (const NodePtr& child : literal->children) {
    if (child == nullptr) {
      Emit(Op::PushUndefined, 0, 1);
    } else {
      Expression(*child);
    }
    ++count;
  }
  Emit(Op::Call, count, -static_cast<int>(count) - 1);
}

void Compiler::Assignment(const Node& node) {
  const Node* target = node.Child(0);
  const Node* value = node.Child(1);
  if (target == nullptr || value == nullptr) {
    ThrowSyntax("malformed assignment");
    return;
  }
  const std::string& op = node.string;
  const bool is_member = target->kind == NodeKind::Member;

  // The operands a member target needs, read once. `o[f()].x += 1` must call f
  // once, which is the whole reason a compound assignment is not a load and a
  // store written separately.
  bool key_on_stack = false;
  std::uint32_t name = 0;
  if (is_member) {
    MemberOperands(*target, key_on_stack, name);
  }

  if (op == "&&=" || op == "||=" || op == "?\?=") {
    // Short-circuiting assignment does not evaluate the right side, and does
    // not assign at all, when the test fails. Assigning the old value back
    // would be observable through a setter.
    const Op test = op == "&&=" ? Op::JumpIfFalsePeek
                    : op == "||=" ? Op::JumpIfTruePeek
                                  : Op::JumpIfNotNullish;
    if (is_member) {
      if (key_on_stack) {
        Emit(Op::Dup2, 0, 2);
        Emit(Op::GetProperty, 0, -1);
      } else {
        Emit(Op::Dup, 0, 1);
        Emit(Op::GetPropertyName, name, 0);
      }
    } else if (target->kind == NodeKind::Identifier) {
      EmitLoad(target->string);
    } else {
      ThrowSyntax("invalid assignment target");
      return;
    }
    const std::uint32_t kept = stack_depth_;
    const std::uint32_t skip = Emit(test, 0, 0);
    Emit(Op::Pop, 0, -1);
    Expression(*value);
    if (is_member) {
      Emit(key_on_stack ? Op::SetProperty : Op::SetPropertyName, name, key_on_stack ? -2 : -1);
    } else {
      EmitStore(target->string);
    }
    const std::uint32_t merged = stack_depth_;
    const std::uint32_t to_end = Emit(Op::Jump, 0, 0);
    Patch(skip, Here());
    stack_depth_ = kept;
    // The value that was already there is the result; whatever was under it
    // was only there to reach it.
    while (stack_depth_ > merged) {
      Emit(Op::PopUnder, 0, -1);
    }
    Patch(to_end, Here());
    stack_depth_ = merged;
    return;
  }

  if (op != "=") {
    BinaryOp binary = BinaryOp::Add;
    if (!ParseBinaryOp(std::string_view(op).substr(0, op.size() - 1), binary)) {
      ThrowSyntax("unsupported operator '" + op + "'");
      return;
    }
    if (is_member) {
      if (key_on_stack) {
        Emit(Op::Dup2, 0, 2);
        Emit(Op::GetProperty, 0, -1);
      } else {
        Emit(Op::Dup, 0, 1);
        Emit(Op::GetPropertyName, name, 0);
      }
    } else if (target->kind == NodeKind::Identifier) {
      EmitLoad(target->string);
    } else {
      ThrowSyntax("invalid assignment target");
      return;
    }
    Expression(*value);
    Emit(Op::Binary, static_cast<std::uint32_t>(binary), -1);
    if (is_member) {
      Emit(key_on_stack ? Op::SetProperty : Op::SetPropertyName, name, key_on_stack ? -2 : -1);
    } else {
      EmitStore(target->string);
    }
    return;
  }

  Expression(*value);
  if (is_member) {
    Emit(key_on_stack ? Op::SetProperty : Op::SetPropertyName, name, key_on_stack ? -2 : -1);
    return;
  }
  if (target->kind == NodeKind::Identifier) {
    EmitStore(target->string);
    return;
  }
  // A destructuring assignment. The value is the expression's result, so it is
  // duplicated and the pattern consumes the copy.
  Emit(Op::Dup, 0, 1);
  BindTarget(*target, false, false);
}

std::unique_ptr<CompiledFunction> Compile(const Node& program) {
  CompileState state;
  auto compiled = std::make_unique<CompiledFunction>();
  Compiler compiler(state, *compiled);
  compiler.Program(program);
  if (state.failed) {
    return nullptr;
  }
  return compiled;
}

}  // namespace microbrowser::js

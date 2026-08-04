#include <memory>
#include <string>
#include <utility>

#include "js/CompilerImpl.h"

// How a function body becomes a chunk.
//
// Split out of Compiler.cpp, which is the emitter and the expressions: a
// function is neither. What happens here is the decisions a body needs made
// before a single instruction of it is emitted, and all of them are decisions
// about the *shape* of the call rather than about what it computes --
//
//   * whether its bindings live in the frame or on the heap, which is
//     `frame_locals` and which every name in the body then resolves against;
//   * whether it builds an `arguments` array, which is one walk of the body;
//   * what calling it hands back -- a value, a promise, or a generator.
//
// The last of those is why the prologue below is not simply "bind the
// parameters and compile the body". A generator binds its parameters at the
// call and then suspends before the body, so the GeneratorEntry sits between
// the two -- which is what makes `g(bad())` throw at the call and `g()` run
// nothing until the first `next`.

namespace microbrowser::js {

namespace {

// Whether a subtree names `arguments`, so the array is built only where
// something can observe it. The walk stops at a nested ordinary function --
// which has an `arguments` of its own -- and does not stop at an arrow, which
// does not.
bool NamesArguments(const Node& node) {
  if (node.kind == NodeKind::Identifier && node.string == "arguments") {
    return true;
  }
  for (const NodePtr& child : node.children) {
    if (child == nullptr) {
      continue;
    }
    if (child->kind == NodeKind::FunctionExpression ||
        child->kind == NodeKind::FunctionDeclaration ||
        child->kind == NodeKind::ClassExpression || child->kind == NodeKind::ClassDeclaration) {
      continue;
    }
    if (NamesArguments(*child)) {
      return true;
    }
  }
  return false;
}

// Whether a subtree can make a function object, which is the only way a scope
// created inside a call can outlive it.
//
// The walk stops at the first one it finds rather than descending: a function
// nested three deep is still a function this one contains, and what matters is
// that there is one at all. A class counts because its methods are functions
// and because its body is built by the tree-walking evaluator, which captures
// the scope it is handed.
bool CreatesClosure(const Node& node) {
  switch (node.kind) {
    case NodeKind::FunctionExpression:
    case NodeKind::FunctionDeclaration:
    case NodeKind::ArrowFunction:
    case NodeKind::ClassExpression:
    case NodeKind::ClassDeclaration:
    case NodeKind::MethodDefinition:
      return true;
    default:
      break;
  }
  for (const NodePtr& child : node.children) {
    if (child != nullptr && CreatesClosure(*child)) {
      return true;
    }
  }
  return false;
}

}  // namespace

void Compiler::Function(const Node& node, bool arrow) {
  const CompileDepth depth(state_, kMaxCompileDepth);
  if (depth.Exceeded()) {
    Fail();
    return;
  }
  const Node* parameters = node.Child(0);
  const Node* body = node.Child(1);
  function_.name = node.string;
  function_.is_arrow = arrow;
  // An async function returns a promise and its body can suspend; a generator
  // returns an iterator and its body can suspend. Carried on the node as
  // `number` by the parser, because both are modifiers on a function and not
  // different kinds of one -- everything else about compiling the body is
  // identical, which is why they are flags here rather than three code paths.
  const auto flags = static_cast<std::uint8_t>(node.number);
  function_.is_async = (flags & kFunctionAsync) != 0;
  function_.is_generator = (flags & kFunctionGenerator) != 0;
  // What `fn.length` reports, which stops at the first default and at a rest
  // element -- not how many bindings the prologue makes, which is all of them.
  function_.parameter_count = DeclaredArity(parameters);
  function_.needs_arguments =
      !arrow && ((body != nullptr && NamesArguments(*body)) ||
                 (parameters != nullptr && NamesArguments(*parameters)));
  // Nothing this call makes can outlive it unless it makes a function, so a
  // body with none in it keeps its bindings in the frame and allocates no
  // scope at all. Decided here, before a single instruction is emitted,
  // because every name in the body resolves differently depending on it.
  function_.frame_locals = !(body != nullptr && CreatesClosure(*body)) &&
                           !(parameters != nullptr && CreatesClosure(*parameters));

  // The function's own scope, whose first five slots PushFrame fills. Reserved
  // in the same order there and here; see kSlotThis in Bytecode.h. The two
  // lists agreeing is what makes a parameter's index a compile-time constant,
  // so a slot added to one and not the other is not a slowdown -- it is every
  // parameter reading the wrong binding.
  scopes_.emplace_back();
  scope_floor_ = 1;
  Reserve("this");
  Reserve("__home__");
  Reserve("__function__");
  Reserve("arguments");
  Reserve("__newtarget__");
  if (parameters != nullptr) {
    for (const NodePtr& parameter : parameters->children) {
      if (parameter != nullptr) {
        ReservePattern(*parameter);
      }
    }
  }
  // Every `var` the body declares, at any depth inside it, belongs to *this*
  // scope rather than to the block it is written in. Reserved here, after the
  // parameters, so a `var` that names a parameter shares its slot instead of
  // taking a second one.
  std::vector<std::string> var_names;
  if (body != nullptr) {
    CollectVarNames(*body, var_names);
  }
  for (const std::string& name : var_names) {
    Reserve(name);
  }
  // How many slots the frame reserves. In a scoped function that is this one
  // scope; in a flattened one it is every scope in the body as well, so it is
  // not known until the body has been compiled and is set again at the end.
  function_.scope_slots = scopes_.back().count;

  // Slot zero of every frame is the completion value. Reserved here so that a
  // handler's recorded depth and a frame's working base mean the same thing in
  // a function as they do in a program.
  Emit(Op::PushUndefined, 0, 1);

  // The `var` bindings exist from here, holding undefined, however far down
  // the body their declarations are written. Before the parameters bind, not
  // after: `function f(a) { var a }` must leave `a` holding the argument, and
  // declaring in the other order would overwrite it with undefined.
  for (const std::string& name : var_names) {
    Emit(Op::PushUndefined, 0, 1);
    EmitDeclare(name, false);
  }

  if (parameters != nullptr) {
    for (std::size_t i = 0; i < parameters->children.size(); ++i) {
      const Node* parameter = parameters->Child(i);
      if (parameter == nullptr) {
        continue;
      }
      if (parameter->kind == NodeKind::RestElement) {
        const Node* target = parameter->Child(0);
        Emit(Op::RestArguments, static_cast<std::uint32_t>(i), 1);
        if (target == nullptr) {
          Emit(Op::Pop, 0, -1);
        } else {
          BindTarget(*target, true, false);
        }
        break;
      }
      Emit(Op::LoadArgument, static_cast<std::uint32_t>(i), 1);
      const Node* target = parameter;
      if (parameter->kind == NodeKind::AssignmentPattern) {
        target = parameter->Child(0);
        const Node* fallback = parameter->Child(1);
        if (fallback != nullptr) {
          // A default applies to a missing argument *and* to an explicit
          // undefined, which is the rule and is not the same as "no argument".
          const std::uint32_t skip = Emit(Op::JumpIfNotUndefined, 0, 0);
          Emit(Op::Pop, 0, -1);
          Expression(*fallback);
          Patch(skip, Here());
        }
      }
      if (target == nullptr) {
        Emit(Op::Pop, 0, -1);
      } else {
        BindTarget(*target, true, false);
      }
    }
  }

  if (function_.is_generator) {
    // After the parameters and before the body: calling a generator function
    // binds its parameters -- so a default that throws throws at the call --
    // and then runs not one line of the body. This is the instruction that
    // hands the caller the generator and files the frame; the Pop discards
    // what the first `next` sends in, which by definition nothing is waiting
    // for.
    Emit(Op::GeneratorEntry, 0, 1);
    Emit(Op::Pop, 0, -1);
  }

  if (body == nullptr) {
    Emit(Op::PushUndefined, 0, 1);
    Emit(Op::Return, 0, -1);
  } else if (body->kind == NodeKind::Block) {
    Block(*body);
    Emit(Op::PushUndefined, 0, 1);
    Emit(Op::Return, 0, -1);
  } else {
    // An expression-bodied arrow returns its expression.
    Expression(*body);
    Emit(Op::Return, 0, -1);
  }

  if (function_.frame_locals) {
    function_.scope_slots = frame_slots_;
  }
}

std::uint32_t Compiler::CompileNested(const Node& node, bool arrow) {
  auto nested = std::make_unique<CompiledFunction>();
  nested->source = &node;
  // The nested compiler knows this one, because the run-time chain does: a
  // frame's scope has the defining scope as its parent, so a name the inner
  // function does not declare is found by counting scopes out through here.
  Compiler inner(state_, *nested, this);
  inner.Function(node, arrow);
  if (state_.failed) {
    return 0;
  }
  function_.functions.push_back(std::move(nested));
  return static_cast<std::uint32_t>(function_.functions.size() - 1);
}

void Compiler::FunctionValue(const Node& node, bool arrow) {
  const std::uint32_t index = CompileNested(node, arrow);
  if (state_.failed) {
    return;
  }
  Emit(arrow ? Op::ClosureArrow : Op::Closure, index, 1);
}

void Compiler::ClassMethods(const Node& node) {
  // One scope, standing for the one EvaluateClass makes at run time so that a
  // method can name its own class. It holds exactly one binding -- the class
  // name -- and nothing when the class is anonymous, in which case the scope
  // still exists and still counts as a hop.
  scopes_.emplace_back();
  Reserve(node.string);
  for (const NodePtr& member : node.children) {
    if (member == nullptr || member->kind != NodeKind::MethodDefinition ||
        member->children.empty()) {
      continue;
    }
    const Node* body = member->children.back().get();
    if (body != nullptr && body->kind == NodeKind::FunctionExpression) {
      CompileNested(*body, false);
    }
  }
  scopes_.pop_back();
}

}  // namespace microbrowser::js

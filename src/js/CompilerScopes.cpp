#include <string>
#include <utility>

#include "js/CompilerImpl.h"

// Where names go, and how a reference finds one again.
//
// Its own translation unit because it is its own question. Compiler.cpp and
// CompilerStatements.cpp are about turning syntax into instructions; this is
// about the model of the run-time scope chain that lets those instructions say
// "two scopes out, slot three" instead of naming anything. The model has to
// stay in lockstep with the PushScope instructions the other two emit -- that
// correspondence is the whole basis of the scheme, and keeping the code that
// depends on it together is what makes it reviewable.
//
// A reference the model cannot place is not an error. The top level of a
// program declares into the global scope, which other scripts and every builtin
// also write to and which therefore has no layout to know; and the packing has
// limits. Both fall back to the name form, which is slower and not wrong.

namespace microbrowser::js {

void Compiler::Reserve(std::string_view name) {
  if (scopes_.empty() || name.empty()) {
    // The top level of a program. Its scope is the global one, which other
    // scripts and every builtin also write to, so there is no layout to know
    // and the name form is the right answer rather than a missed one.
    return;
  }
  CompiledScope& scope = scopes_.back();
  if (scope.slots.count(std::string(name)) != 0) {
    return;
  }
  if (!function_.frame_locals) {
    if (scope.count > kMaxSlotIndex) {
      return;  // too wide to pack; the name form still finds it
    }
    scope.slots.emplace(std::string(name), scope.count);
    ++scope.count;
    return;
  }
  // Flattened: one run of slots per scope, all of them in the frame's slice.
  // Falling back to the name form is not available here -- a name lookup walks
  // Environments and there is no Environment holding this -- so running out of
  // indices abandons the compile and the tree-walker takes the program. A
  // function with four thousand block-scoped names is the only way to get
  // there.
  if (frame_slots_ >= kMaxSlotIndex) {
    Fail();
    return;
  }
  if (scope.count == 0) {
    scope.base = frame_slots_;
  }
  scope.slots.emplace(std::string(name), frame_slots_);
  ++frame_slots_;
  ++scope.count;
}

void Compiler::OpenScope() { scopes_.emplace_back(); }

void Compiler::EnterScope() {
  const CompiledScope& scope = scopes_.back();
  if (!function_.frame_locals) {
    Emit(Op::PushScope, scope.count, 0);
  } else if (scope.count > 0) {
    Emit(Op::ClearLocals, PackLocals(scope.base, scope.count), 0);
  }
  ++scope_depth_;
}

void Compiler::LeaveScope() {
  if (!function_.frame_locals) {
    Emit(Op::PopScope, 1, 0);
  }
  --scope_depth_;
  scopes_.pop_back();
}

void Compiler::ReservePattern(const Node& target) {
  switch (target.kind) {
    case NodeKind::Identifier:
      Reserve(target.string);
      return;
    case NodeKind::ArrayLiteral:
      for (const NodePtr& element : target.children) {
        if (element != nullptr) {
          ReservePattern(*element);
        }
      }
      return;
    case NodeKind::ObjectLiteral:
      for (const NodePtr& property : target.children) {
        if (property != nullptr && property->kind == NodeKind::Property &&
            property->Child(0) != nullptr) {
          ReservePattern(*property->Child(0));
        }
      }
      return;
    case NodeKind::AssignmentPattern:
    case NodeKind::RestElement:
    case NodeKind::Spread:
      if (target.Child(0) != nullptr) {
        ReservePattern(*target.Child(0));
      }
      return;
    default:
      return;  // a member target assigns through something that already exists
  }
}

void Compiler::ReserveDeclarations(const Node& list) {
  for (const NodePtr& statement : list.children) {
    if (statement == nullptr) {
      continue;
    }
    switch (statement->kind) {
      case NodeKind::FunctionDeclaration:
      case NodeKind::ClassDeclaration:
        Reserve(statement->string);
        break;
      case NodeKind::VariableDeclaration:
        for (const NodePtr& declarator : statement->children) {
          if (declarator != nullptr && declarator->Child(0) != nullptr) {
            ReservePattern(*declarator->Child(0));
          }
        }
        break;
      default:
        break;
    }
  }
}

bool Compiler::ResolveSlot(std::string_view name, std::uint32_t& packed) {
  std::uint32_t hops = 0;
  for (const Compiler* compiler = this; compiler != nullptr; compiler = compiler->parent_) {
    // Every scope of a flattened function is the frame's one slice, so all of
    // them are zero hops out and getting past them costs a single hop -- to
    // the scope the function was defined in, where the chain is Environments
    // again. Only `this` can be flattened: a function containing another
    // creates a closure, which is exactly what disqualifies it.
    const bool flat = compiler->function_.frame_locals;
    for (std::size_t i = compiler->scopes_.size(); i > 0; --i) {
      const CompiledScope& scope = compiler->scopes_[i - 1];
      const auto found = scope.slots.find(std::string(name));
      if (found != scope.slots.end()) {
        const std::uint32_t index = Name(name);
        if (hops > kMaxSlotHops || found->second > kMaxSlotIndex || index > kMaxSlotName) {
          // Too deep or too wide to pack. The name form still finds a binding
          // that lives in an Environment, and finds nothing at all when the
          // binding is in a frame -- so where that is the answer, the compile
          // is abandoned and the tree-walker takes the program instead.
          if (flat) {
            Fail();
          }
          return false;
        }
        packed = PackSlot(hops, found->second, index);
        return true;
      }
      if (!flat) {
        ++hops;
        if (hops > kMaxSlotHops) {
          return false;
        }
      }
    }
    if (flat) {
      ++hops;
      if (hops > kMaxSlotHops) {
        return false;
      }
    }
  }
  return false;
}

void Compiler::EmitLoad(std::string_view name) {
  std::uint32_t packed = 0;
  if (ResolveSlot(name, packed)) {
    Emit(Op::LoadSlot, packed, 1);
    return;
  }
  Emit(Op::LoadName, Name(name), 1);
}

void Compiler::EmitStore(std::string_view name) {
  std::uint32_t packed = 0;
  if (ResolveSlot(name, packed)) {
    Emit(Op::StoreSlot, packed, 0);
    return;
  }
  Emit(Op::StoreName, Name(name), 0);
}

void Compiler::EmitDeclare(std::string_view name, bool is_const) {
  if (!scopes_.empty()) {
    const CompiledScope& scope = scopes_.back();
    const auto found = scope.slots.find(std::string(name));
    if (found != scope.slots.end()) {
      function_.declarations.push_back(SlotDeclaration{found->second, Name(name), is_const});
      Emit(Op::DeclareSlot,
           static_cast<std::uint32_t>(function_.declarations.size() - 1), -1);
      return;
    }
  }
  if (function_.frame_locals) {
    // The name form declares into an Environment, and a flattened function has
    // none of its own -- the binding would land in the enclosing scope, where
    // nothing in this function would ever find it again. Every declaration
    // here is supposed to have been reserved when its scope opened, so this is
    // a gap in the reserving rather than a program the language allows; the
    // tree-walker takes the program and the difference is speed.
    Fail();
    return;
  }
  Emit(is_const ? Op::DeclareConst : Op::DeclareLet, Name(name), -1);
}


}  // namespace microbrowser::js

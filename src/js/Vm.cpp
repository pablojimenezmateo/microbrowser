#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "js/TemplateParts.h"

// The machine.
//
// One loop over one instruction stream, with two stacks that are members rather
// than C++ frames. That is the whole change, and everything the project wanted
// from it follows from those two stacks being *data*:
//
//   * The collector can run while script is running. Every live value is on
//     vm_.stack or reachable from a frame, and GatherVmRoots hands both to
//     Heap::Collect. A tree-walker cannot do this, which is why collection used
//     to happen only between top-level statements and why a script that
//     allocated while recursing ran out of heap instead of collecting.
//
//   * A call is a push. `f()` calling `g()` calling `h()` is three frames on a
//     vector, not three hundred bytes of C++ stack each, so the recursion limit
//     is a number this file chooses rather than one the platform imposes.
//
//   * A frame can be *saved*. Nothing does yet, and `await` is what will:
//     suspending a call means copying a frame and its stack slice somewhere,
//     which is a thing you can only write down about a machine whose state is
//     already written down.
//
// The one rule that runs through every case below: **read operands in place and
// pop after**. A property read can run a getter, a getter can allocate, and
// allocation can collect -- so a receiver moved into a C++ local and popped
// first is a receiver the collector cannot see. Values are copied out for
// convenience, but the stack is only truncated once the operation that could
// call has finished.

namespace microbrowser::js {

Environment* Interpreter::CurrentScope() {
  if (vm_.frames.empty()) {
    return global_scope_;
  }
  const Frame& frame = vm_.frames.back();
  return vm_.scopes.size() > frame.scope_base ? vm_.scopes.back() : frame.scope;
}

Binding* Interpreter::SlotBinding(const Frame& frame, std::uint32_t packed) {
  const std::uint32_t hops = SlotHops(packed);
  const std::uint32_t index = SlotIndex(packed);
  if (frame.code->frame_locals) {
    if (hops == 0) {
      // The frame's own bindings, and every block's inside it: a flattened
      // function has one slice and the compiler numbered it function-wide.
      const std::size_t at = frame.locals_base + index;
      return at < vm_.locals.size() ? &vm_.locals[at] : nullptr;
    }
    // Past the frame, the chain is ordinary scopes again, starting at the one
    // the function was defined in -- so one hop of the compiler's count has
    // already been spent getting out of the frame.
    Environment* scope = frame.scope == nullptr ? nullptr : frame.scope->Ancestor(hops - 1);
    return scope == nullptr ? nullptr : scope->Slot(index);
  }
  Environment* scope = CurrentScope()->Ancestor(hops);
  return scope == nullptr ? nullptr : scope->Slot(index);
}

Value* Interpreter::FrameName(std::string_view name, std::uint32_t slot) {
  if (!vm_.frames.empty()) {
    const Frame& frame = vm_.frames.back();
    if (frame.code->frame_locals) {
      const std::size_t at = frame.locals_base + slot;
      if (at < vm_.locals.size() && vm_.locals[at].live) {
        return &vm_.locals[at].value;
      }
      return frame.scope == nullptr ? nullptr : frame.scope->Lookup(name);
    }
  }
  return CurrentScope()->Lookup(name);
}

void Interpreter::GatherVmRoots(std::vector<Object*>& objects,
                                std::vector<Environment*>& scopes) const {
  for (const Value& value : vm_.stack) {
    if (value.IsObject() || value.IsSymbol()) {
      objects.push_back(value.object);
    }
  }
  // A flattened frame's bindings are reachable from nothing else -- there is no
  // Environment holding them -- so this is the only thing keeping them alive.
  for (const Binding& binding : vm_.locals) {
    if (binding.live && (binding.value.IsObject() || binding.value.IsSymbol())) {
      objects.push_back(binding.value.object);
    }
  }
  for (const Frame& frame : vm_.frames) {
    for (Object* held : {frame.function, frame.promise, frame.generator}) {
      if (held != nullptr) {
        objects.push_back(held);
      }
    }
    if (frame.scope != nullptr) {
      scopes.push_back(frame.scope);
    }
  }
  for (Environment* scope : vm_.scopes) {
    if (scope != nullptr) {
      scopes.push_back(scope);
    }
  }
  // An open cursor is the only thing keeping what it is walking alive.
  for (const Iteration& cursor : vm_.iterations) {
    if (cursor.array != nullptr) {
      objects.push_back(cursor.array);
    }
    for (const Value* held : {&cursor.iterator, &cursor.next}) {
      if (held->IsObject() || held->IsSymbol()) {
        objects.push_back(held->object);
      }
    }
  }
}

Result Interpreter::PushFrame(Object* function, std::size_t callee_slot,
                              std::uint32_t argument_count) {
  if (call_depth_ + static_cast<int>(vm_.frames.size()) >= kMaxCallDepth ||
      vm_.frames.size() >= kFrameCapacity) {
    // A page can write unbounded recursion. The frames are on the heap now, so
    // this bound is a policy rather than a property of the C++ stack -- but a
    // page still has to get a RangeError rather than an allocator failure.
    return Throw("RangeError", "maximum call stack size exceeded");
  }
  const CompiledFunction* code = function->Code();
  Environment* scope = nullptr;
  const std::size_t locals_base = vm_.locals.size();
  if (code->frame_locals) {
    // Nothing made inside this call can outlive it, so its bindings go on the
    // locals stack and the call allocates nothing at all. The chain continues
    // at the scope the function was defined in.
    if (locals_base + code->scope_slots > kLocalsCapacity) {
      return Throw("RangeError", "maximum call stack size exceeded");
    }
    // Reserved on the first call that needs it rather than in the constructor.
    // The rule is the value stack's -- fixed capacity, because an instruction
    // holds a Binding* into this while it runs -- but the cost is three
    // megabytes, and a page that runs no script should not pay it. Measured:
    // the benchmark harness holds twenty-six interpreters and only one of them
    // ever pushes a frame.
    if (vm_.locals.capacity() < kLocalsCapacity) {
      vm_.locals.reserve(kLocalsCapacity);
    }
    vm_.locals.resize(locals_base + code->scope_slots);
    scope = function->Closure();
  } else {
    scope = heap_.AllocateEnvironment(function->Closure());
    if (scope == nullptr) {
      return Throw("RangeError", "out of memory");
    }
    // The prologue, at the four fixed slots Compiler::Function reserved. The
    // two agreeing is what lets a parameter's index be a compile-time
    // constant; see kSlotThis in Bytecode.h.
    scope->Reserve(code->scope_slots);
  }

  // An arrow function has no `this` of its own: it uses the one captured where
  // it was written. That is the whole difference between the two forms.
  const Value self = vm_.stack[callee_slot + 1];
  const auto declare = [&](std::uint32_t index, const char* name, Value value, bool is_const) {
    if (code->frame_locals) {
      vm_.locals[locals_base + index] = Binding{std::move(value), is_const, true};
      return;
    }
    scope->DeclareSlot(index, name, std::move(value), is_const);
  };
  declare(kSlotThis, "this", function->IsArrow() ? function->BoundThis() : self, true);
  if (function->HomeObject() != nullptr) {
    declare(kSlotHome, "__home__", Value::Obj(function->HomeObject()), true);
  }
  declare(kSlotFunction, "__function__", Value::Obj(function), true);
  if (code->needs_arguments) {
    std::vector<Value> arguments(vm_.stack.begin() + static_cast<std::ptrdiff_t>(callee_slot) + 2,
                                 vm_.stack.end());
    Object* list = NewArray(std::move(arguments));
    if (list == nullptr) {
      vm_.locals.resize(locals_base);
      return Throw("RangeError", "out of memory");
    }
    declare(kSlotArguments, "arguments", Value::Obj(list), false);
  }

  Frame frame;
  frame.code = code;
  frame.function = function;
  frame.scope = scope;
  frame.locals_base = locals_base;
  if (code->is_async) {
    // Made before a line of the body runs, because the body can suspend on its
    // first instruction and the caller has to be handed this either way.
    const Value promise = NewPromiseValue();
    if (!promise.IsObject()) {
      vm_.locals.resize(locals_base);
      return Throw("RangeError", "out of memory");
    }
    frame.promise = promise.object;
  }
  if (code->is_generator) {
    // Made before a line of the body runs, for the reason the promise above is:
    // the GeneratorEntry that follows the parameter prologue hands this to the
    // caller, and the caller has to be handed it whatever the body does next.
    Object* generator = NewGenerator();
    if (generator == nullptr) {
      vm_.locals.resize(locals_base);
      return Throw("RangeError", "out of memory");
    }
    frame.generator = generator;
  }
  frame.stack_base = callee_slot;
  frame.argument_base = callee_slot + 2;
  frame.argument_count = argument_count;
  frame.scope_base = vm_.scopes.size();
  frame.iteration_base = vm_.iterations.size();
  vm_.frames.push_back(frame);
  return Result::Normal();
}

bool Interpreter::UnwindToHandler(const Value& thrown, std::size_t entry_depth) {
  while (vm_.frames.size() > entry_depth) {
    Frame& frame = vm_.frames.back();
    // The instruction that threw, not the one after it: ip has already moved on
    // by the time anything can fail.
    const std::uint32_t at = frame.ip == 0 ? 0 : frame.ip - 1;
    const std::size_t working_base = frame.stack_base + 2 + frame.argument_count;
    for (const Handler& handler : frame.code->handlers) {
      if (at < handler.begin || at >= handler.end) {
        continue;
      }
      // Back to the state the `try` started from: the cursors it had open, the
      // scopes it was inside, and the stack as deep as it was. A throw can
      // happen half way through an expression, which is why this is a
      // truncation and not a pop.
      vm_.iterations.resize(frame.iteration_base + handler.iteration_depth);
      vm_.scopes.resize(frame.scope_base + handler.scope_depth);
      vm_.stack.resize(working_base + handler.stack_depth);
      vm_.stack.push_back(thrown);
      frame.ip = handler.target;
      return true;
    }
    // Nothing here catches it. The frame goes, and the search continues in the
    // caller -- which is what makes a throw cross a call boundary.
    const Frame done = frame;
    vm_.frames.pop_back();
    vm_.iterations.resize(done.iteration_base);
    vm_.scopes.resize(done.scope_base);
    vm_.locals.resize(done.locals_base);
    vm_.stack.resize(done.stack_base);
    if (done.promise != nullptr) {
      // The throw stops here. An async function does not throw at its caller,
      // it returns a rejected promise -- which its caller already has, since
      // the promise was handed over when the call was made.
      SettleAsyncResult(done.promise, thrown, true);
      vm_.stack.push_back(Value::Obj(done.promise));
      return true;
    }
  }
  return false;
}

Result Interpreter::CallCompiled(Object* function, const Value& self,
                                 const std::vector<Value>& arguments) {
  if (vm_.stack.size() + arguments.size() + 2 > kValueStackCapacity) {
    return Throw("RangeError", "maximum call stack size exceeded");
  }
  const std::size_t entry_depth = vm_.frames.size();
  const std::size_t callee_slot = vm_.stack.size();
  vm_.stack.push_back(Value::Obj(function));
  vm_.stack.push_back(self);
  for (const Value& argument : arguments) {
    vm_.stack.push_back(argument);
  }
  const Result pushed =
      PushFrame(function, callee_slot, static_cast<std::uint32_t>(arguments.size()));
  if (pushed.IsAbrupt()) {
    vm_.stack.resize(callee_slot);
    return pushed;
  }
  Result result = RunFrames(entry_depth);
  vm_.stack.resize(callee_slot);
  return result;
}

Result Interpreter::RunFrames(std::size_t entry_depth) {
  while (vm_.frames.size() > entry_depth) {
    if (++steps_ > kMaxSteps) {
      // A page can write `while (true) {}`. A step budget makes that a thrown
      // error rather than a hung browser, which is the only difference a user
      // would notice between the two.
      const Result out = Throw("RangeError", "script ran too long");
      if (!UnwindToHandler(out.value, entry_depth)) {
        return out;
      }
      continue;
    }
    if (vm_.stack.size() + 8 > kValueStackCapacity) {
      const Result out = Throw("RangeError", "maximum call stack size exceeded");
      if (!UnwindToHandler(out.value, entry_depth)) {
        return out;
      }
      continue;
    }

    Frame* frame = &vm_.frames.back();
    if (frame->ip >= frame->code->code.size()) {
      // A body that fell off its own end. Every compiled function ends in a
      // Return, so this is a compiler bug rather than a program one -- and
      // undefined is the answer that keeps it from being a memory error too.
      vm_.stack.resize(frame->stack_base);
      vm_.stack.push_back(Value::Undefined());
      vm_.scopes.resize(frame->scope_base);
      vm_.locals.resize(frame->locals_base);
      vm_.iterations.resize(frame->iteration_base);
      vm_.frames.pop_back();
      if (vm_.frames.size() == entry_depth) {
        Value out = vm_.stack.back();
        vm_.stack.pop_back();
        return Result::Normal(std::move(out));
      }
      continue;
    }

    const Instruction instruction = frame->code->code[frame->ip++];
    const CompiledFunction& code = *frame->code;
    Result pending;
    bool threw = false;

    switch (instruction.op) {
      case Op::Nop:
        break;

      // --- Stack shuffling ---------------------------------------------------
      case Op::Pop:
        vm_.stack.pop_back();
        break;
      case Op::PopUnder:
        vm_.stack[vm_.stack.size() - 2] = vm_.stack.back();
        vm_.stack.pop_back();
        break;
      case Op::Dup:
        vm_.stack.push_back(vm_.stack.back());
        break;
      case Op::Dup2: {
        const Value under = vm_.stack[vm_.stack.size() - 2];
        const Value over = vm_.stack.back();
        vm_.stack.push_back(under);
        vm_.stack.push_back(over);
        break;
      }
      case Op::Swap:
        std::swap(vm_.stack[vm_.stack.size() - 1], vm_.stack[vm_.stack.size() - 2]);
        break;
      case Op::RotateDown: {
        Value top = vm_.stack.back();
        const std::size_t at = vm_.stack.size() - 1 - instruction.a;
        for (std::size_t i = vm_.stack.size() - 1; i > at; --i) {
          vm_.stack[i] = vm_.stack[i - 1];
        }
        vm_.stack[at] = std::move(top);
        break;
      }

      // --- Constants ---------------------------------------------------------
      case Op::PushConstant:
        vm_.stack.push_back(code.constants[instruction.a]);
        break;
      case Op::PushUndefined:
        vm_.stack.push_back(Value::Undefined());
        break;
      case Op::PushNull:
        vm_.stack.push_back(Value::Null());
        break;
      case Op::PushTrue:
        vm_.stack.push_back(Value::Bool(true));
        break;
      case Op::PushFalse:
        vm_.stack.push_back(Value::Bool(false));
        break;

      // --- Names -------------------------------------------------------------
      case Op::LoadName: {
        const std::string& name = code.names[instruction.a];
        if (Value* binding = CurrentScope()->Lookup(name)) {
          vm_.stack.push_back(*binding);
          break;
        }
        // A property of the global object is also a global variable, so that
        // `globalThis.x = 1` makes `x` readable: one namespace rather than two
        // that happen to overlap.
        if (const Value* property = global_->GetOwn(name)) {
          vm_.stack.push_back(*property);
          break;
        }
        // Not undefined: a name that was never declared is a ReferenceError,
        // which is the language's way of catching a typo.
        pending = Throw("ReferenceError", name + " is not defined");
        threw = true;
        break;
      }
      case Op::StoreName: {
        const std::string& name = code.names[instruction.a];
        Environment* scope = CurrentScope();
        if (!scope->Assign(name, vm_.stack.back())) {
          if (scope->Lookup(name) != nullptr) {
            pending = Throw("TypeError", "assignment to constant variable '" + name + "'");
            threw = true;
            break;
          }
          // An assignment to an undeclared name creates a global. Sloppy mode,
          // and the web depends on it. On the global *object*, so that
          // `globalThis.x` and `x` name the same thing.
          global_->Set(name, vm_.stack.back());
        }
        break;
      }
      case Op::DeclareLet:
        CurrentScope()->Declare(code.names[instruction.a], vm_.stack.back(), false);
        vm_.stack.pop_back();
        break;
      case Op::DeclareConst:
        CurrentScope()->Declare(code.names[instruction.a], vm_.stack.back(), true);
        vm_.stack.pop_back();
        break;
      case Op::TypeofName: {
        const std::string& name = code.names[instruction.a];
        Value* binding = CurrentScope()->Lookup(name);
        const Value* property = binding == nullptr ? global_->GetOwn(name) : nullptr;
        if (binding == nullptr && property == nullptr) {
          vm_.stack.push_back(Value::String("undefined"));
          break;
        }
        vm_.stack.push_back(Value::String(std::string(TypeOf(binding != nullptr ? *binding
                                                                                : *property))));
        break;
      }
      case Op::LoadSlot: {
        const Binding* binding = SlotBinding(*frame, instruction.a);
        if (binding == nullptr || !binding->live) {
          // The slot is reserved but its declaration has not run. Reading a
          // binding before its own `let` is a ReferenceError, and reserving the
          // slot up front must not quietly turn that into undefined.
          pending = Throw("ReferenceError",
                          "cannot access '" + code.names[SlotName(instruction.a)] +
                              "' before it is declared");
          threw = true;
          break;
        }
        vm_.stack.push_back(binding->value);
        break;
      }
      case Op::StoreSlot: {
        Binding* binding = SlotBinding(*frame, instruction.a);
        if (binding == nullptr || !binding->live) {
          pending = Throw("ReferenceError",
                          "cannot access '" + code.names[SlotName(instruction.a)] +
                              "' before it is declared");
          threw = true;
          break;
        }
        if (binding->is_const) {
          pending = Throw("TypeError", "assignment to constant variable '" +
                                           code.names[SlotName(instruction.a)] + "'");
          threw = true;
          break;
        }
        binding->value = vm_.stack.back();
        break;
      }
      case Op::DeclareSlot: {
        const SlotDeclaration& declaration = code.declarations[instruction.a];
        if (code.frame_locals) {
          // Bounded rather than trusted: the index comes from the compiler and
          // not from a page, but a write past the slice would be someone
          // else's binding, and this is the only place that writes by index.
          const std::size_t at = frame->locals_base + declaration.slot;
          if (at < vm_.locals.size()) {
            vm_.locals[at] = Binding{vm_.stack.back(), declaration.is_const, true};
          }
        } else {
          CurrentScope()->DeclareSlot(declaration.slot, code.names[declaration.name],
                                      vm_.stack.back(), declaration.is_const);
        }
        vm_.stack.pop_back();
        break;
      }
      case Op::LoadThis: {
        Value* binding = FrameName("this", kSlotThis);
        vm_.stack.push_back(binding == nullptr ? Value::Undefined() : *binding);
        break;
      }

      // --- Properties --------------------------------------------------------
      case Op::ThrowIfNullishName:
      case Op::ThrowIfNullishKey: {
        const bool keyed = instruction.op == Op::ThrowIfNullishKey;
        const Value& base = vm_.stack[vm_.stack.size() - (keyed ? 2 : 1)];
        if (base.IsNullish()) {
          const std::string what =
              keyed ? ToString(vm_.stack.back()) : code.names[instruction.a];
          pending =
              Throw("TypeError", "cannot read property '" + what + "' of " + ToString(base));
          threw = true;
        }
        break;
      }
      case Op::GetProperty: {
        // Read in place: GetProperty can run a getter, which can call, which can
        // collect. Popping first would take the receiver out of the root set
        // for exactly the duration of the call that needs it alive.
        const Value value = GetProperty(vm_.stack[vm_.stack.size() - 2], KeyFrom(vm_.stack.back()));
        vm_.stack.pop_back();
        vm_.stack.back() = value;
        break;
      }
      case Op::GetPropertyName: {
        const Value value = GetProperty(vm_.stack.back(), code.keys[instruction.a]);
        vm_.stack.back() = value;
        break;
      }
      case Op::SetProperty: {
        const Result stored = SetProperty(vm_.stack[vm_.stack.size() - 3],
                                          KeyFrom(vm_.stack[vm_.stack.size() - 2]),
                                          vm_.stack.back());
        if (stored.IsAbrupt()) {
          pending = stored;
          threw = true;
          break;
        }
        const Value value = vm_.stack.back();
        vm_.stack.resize(vm_.stack.size() - 3);
        vm_.stack.push_back(value);
        break;
      }
      case Op::SetPropertyName: {
        const Result stored = SetProperty(vm_.stack[vm_.stack.size() - 2],
                                          code.keys[instruction.a], vm_.stack.back());
        if (stored.IsAbrupt()) {
          pending = stored;
          threw = true;
          break;
        }
        const Value value = vm_.stack.back();
        vm_.stack.pop_back();
        vm_.stack.back() = value;
        break;
      }
      case Op::DeleteProperty: {
        const Value& base = vm_.stack[vm_.stack.size() - 2];
        const bool removed = base.IsObject() ? base.object->Delete(KeyFrom(vm_.stack.back())) : true;
        vm_.stack.pop_back();
        vm_.stack.back() = Value::Bool(removed);
        break;
      }

      // --- Operators ---------------------------------------------------------
      case Op::Binary: {
        const Result value = ApplyBinary(static_cast<BinaryOp>(instruction.a),
                                         vm_.stack[vm_.stack.size() - 2], vm_.stack.back());
        if (value.IsAbrupt()) {
          pending = value;
          threw = true;
          break;
        }
        vm_.stack.pop_back();
        vm_.stack.back() = value.value;
        break;
      }
      case Op::Not:
        vm_.stack.back() = Value::Bool(!ToBoolean(vm_.stack.back()));
        break;
      case Op::Negate:
        vm_.stack.back() = Value::Number(-ToNumber(vm_.stack.back()));
        break;
      case Op::UnaryPlus:
      case Op::ToNumberOp:
        vm_.stack.back() = Value::Number(ToNumber(vm_.stack.back()));
        break;
      case Op::BitNot:
        vm_.stack.back() = Value::Number(~ToInt32(ToNumber(vm_.stack.back())));
        break;
      case Op::TypeofValue:
        vm_.stack.back() = Value::String(std::string(TypeOf(vm_.stack.back())));
        break;
      case Op::Discard:
        vm_.stack.back() = Value::Undefined();
        break;

      // --- Jumps -------------------------------------------------------------
      case Op::Jump:
        if (instruction.a <= frame->ip) {
          // A back edge: one loop iteration finished with nothing in progress.
          // This is the safepoint the whole exercise was for.
          MaybeCollect();
        }
        frame->ip = instruction.a;
        break;
      case Op::JumpIfFalse:
        if (!ToBoolean(vm_.stack.back())) {
          frame->ip = instruction.a;
        }
        vm_.stack.pop_back();
        break;
      case Op::JumpIfTrue:
        if (ToBoolean(vm_.stack.back())) {
          frame->ip = instruction.a;
        }
        vm_.stack.pop_back();
        break;
      case Op::JumpIfFalsePeek:
        if (!ToBoolean(vm_.stack.back())) {
          frame->ip = instruction.a;
        }
        break;
      case Op::JumpIfTruePeek:
        if (ToBoolean(vm_.stack.back())) {
          frame->ip = instruction.a;
        }
        break;
      case Op::JumpIfNullish:
        if (vm_.stack.back().IsNullish()) {
          frame->ip = instruction.a;
        }
        break;
      case Op::JumpIfNotNullish:
        if (!vm_.stack.back().IsNullish()) {
          frame->ip = instruction.a;
        }
        break;
      case Op::JumpIfNotUndefined:
        if (!vm_.stack.back().IsUndefined()) {
          frame->ip = instruction.a;
        }
        break;

      // --- Arguments ---------------------------------------------------------
      case Op::LoadArgument:
        vm_.stack.push_back(instruction.a < frame->argument_count
                                ? vm_.stack[frame->argument_base + instruction.a]
                                : Value::Undefined());
        break;
      case Op::RestArguments: {
        std::vector<Value> rest;
        for (std::uint32_t i = instruction.a; i < frame->argument_count; ++i) {
          rest.push_back(vm_.stack[frame->argument_base + i]);
        }
        Object* array = NewArray(std::move(rest));
        if (array == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        vm_.stack.push_back(Value::Obj(array));
        break;
      }

      // --- Calls -------------------------------------------------------------
      case Op::Call:
      case Op::CallApply: {
        std::uint32_t argc = instruction.a;
        if (instruction.op == Op::CallApply) {
          const Value list = vm_.stack.back();
          vm_.stack.pop_back();
          const std::size_t count = list.IsObject() ? list.object->ElementCount() : 0;
          if (vm_.stack.size() + count + 2 > kValueStackCapacity) {
            pending = Throw("RangeError", "maximum call stack size exceeded");
            threw = true;
            break;
          }
          for (std::size_t i = 0; i < count; ++i) {
            vm_.stack.push_back(list.object->GetElement(i));
          }
          argc = static_cast<std::uint32_t>(count);
        }
        const std::size_t callee_slot = vm_.stack.size() - argc - 2;
        const Value callee = vm_.stack[callee_slot];
        if (!callee.IsObject() || !callee.object->IsCallable()) {
          pending = Throw("TypeError", ToString(callee) + " is not a function");
          threw = true;
          break;
        }
        if (callee.object->Code() != nullptr) {
          // The case this file exists for: no C++ recursion, and a safepoint
          // right before the call so a recursive allocator can be collected
          // through.
          MaybeCollect();
          const Result pushed = PushFrame(callee.object, callee_slot, argc);
          if (pushed.IsAbrupt()) {
            pending = pushed;
            threw = true;
          }
          break;
        }
        // A native, or a function the tree-walker still owns. Both hold values
        // in C++ locals, so CallFunction raises call_depth_ and no safepoint
        // fires underneath them.
        const std::vector<Value> arguments(
            vm_.stack.begin() + static_cast<std::ptrdiff_t>(callee_slot) + 2, vm_.stack.end());
        const Value self = vm_.stack[callee_slot + 1];
        const Result returned = CallFunction(callee, self, arguments);
        vm_.stack.resize(callee_slot);
        if (returned.IsAbrupt()) {
          pending = returned;
          threw = true;
          break;
        }
        vm_.stack.push_back(returned.value);
        break;
      }

      case Op::New:
      case Op::NewApply: {
        std::uint32_t argc = instruction.a;
        if (instruction.op == Op::NewApply) {
          const Value list = vm_.stack.back();
          vm_.stack.pop_back();
          const std::size_t count = list.IsObject() ? list.object->ElementCount() : 0;
          if (vm_.stack.size() + count + 1 > kValueStackCapacity) {
            pending = Throw("RangeError", "maximum call stack size exceeded");
            threw = true;
            break;
          }
          for (std::size_t i = 0; i < count; ++i) {
            vm_.stack.push_back(list.object->GetElement(i));
          }
          argc = static_cast<std::uint32_t>(count);
        }
        const std::size_t callee_slot = vm_.stack.size() - argc - 1;
        const Value callee = vm_.stack[callee_slot];
        const std::vector<Value> arguments(
            vm_.stack.begin() + static_cast<std::ptrdiff_t>(callee_slot) + 1, vm_.stack.end());
        const Result instance = Construct(callee, arguments);
        vm_.stack.resize(callee_slot);
        if (instance.IsAbrupt()) {
          pending = instance;
          threw = true;
          break;
        }
        vm_.stack.push_back(instance.value);
        break;
      }

      case Op::Return: {
        Value value = vm_.stack.back();
        const Frame done = *frame;
        if (done.promise != nullptr) {
          // An async call does not return its value to its caller. It settles
          // the promise it handed over -- at the first `await` if it suspended,
          // or right here if it never did -- and the promise is what goes back.
          SettleAsyncResult(done.promise, value, false);
          value = Value::Obj(done.promise);
        }
        vm_.frames.pop_back();
        vm_.scopes.resize(done.scope_base);
        vm_.locals.resize(done.locals_base);
        vm_.iterations.resize(done.iteration_base);
        vm_.stack.resize(done.stack_base);
        vm_.stack.push_back(value);
        if (vm_.frames.size() == entry_depth) {
          Value out = vm_.stack.back();
          vm_.stack.pop_back();
          return Result::Normal(std::move(out));
        }
        break;
      }

      case Op::Await: {
        const Value awaited = vm_.stack.back();
        vm_.stack.pop_back();
        const Result suspended = SuspendForAwait(awaited);
        if (suspended.IsAbrupt()) {
          pending = suspended;
          threw = true;
          break;
        }
        // `frame` is dangling from here: the frame it pointed at has been
        // lifted off the stack and filed. Nothing below reads it, and the loop
        // takes the top frame again from the top.
        if (vm_.frames.size() == entry_depth) {
          Value out = vm_.stack.back();
          vm_.stack.pop_back();
          return Result::Normal(std::move(out));
        }
        break;
      }

      case Op::Yield:
      case Op::GeneratorEntry: {
        // The entry suspend hands the caller the generator itself; a `yield`
        // hands the resumer the value on the stack. Both leave what they hand
        // over exactly where a Return would have written it, which is why the
        // tail below is the same one Await has.
        Value handed;
        GeneratorState::Status status = GeneratorState::Status::Start;
        if (instruction.op == Op::Yield) {
          handed = vm_.stack.back();
          vm_.stack.pop_back();
          status = GeneratorState::Status::Suspended;
        } else if (frame->generator == nullptr) {
          pending = Throw("SyntaxError", "generator entry outside a generator");
          threw = true;
          break;
        } else {
          handed = Value::Obj(frame->generator);
        }
        const Result suspended = SuspendForYield(handed, status);
        if (suspended.IsAbrupt()) {
          pending = suspended;
          threw = true;
          break;
        }
        // `frame` is dangling from here, exactly as it is after an Await.
        if (vm_.frames.size() == entry_depth) {
          Value out = vm_.stack.back();
          vm_.stack.pop_back();
          return Result::Normal(std::move(out));
        }
        break;
      }

      // --- Building values ---------------------------------------------------
      case Op::NewArray: {
        Object* array = NewArray({});
        if (array == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        vm_.stack.push_back(Value::Obj(array));
        break;
      }
      case Op::ArrayPush:
        vm_.stack[vm_.stack.size() - 2].object->PushElement(vm_.stack.back());
        vm_.stack.pop_back();
        break;
      case Op::ArrayHole: {
        Object* array = vm_.stack.back().object;
        array->ResizeElements(array->ElementCount() + 1);
        break;
      }
      case Op::ArraySpread: {
        std::vector<Value> items;
        const Result collected = CollectIterable(vm_.stack.back(), items);
        if (collected.IsAbrupt()) {
          pending = collected;
          threw = true;
          break;
        }
        Object* array = vm_.stack[vm_.stack.size() - 2].object;
        for (Value& item : items) {
          array->PushElement(std::move(item));
        }
        vm_.stack.pop_back();
        break;
      }
      case Op::NewObject: {
        Object* object = NewObject();
        if (object == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        vm_.stack.push_back(Value::Obj(object));
        break;
      }
      case Op::ObjectSet:
        vm_.stack[vm_.stack.size() - 3].object->Set(KeyFrom(vm_.stack[vm_.stack.size() - 2]),
                                                    vm_.stack.back());
        vm_.stack.resize(vm_.stack.size() - 2);
        break;
      case Op::ObjectSetName:
        vm_.stack[vm_.stack.size() - 2].object->Set(code.keys[instruction.a], vm_.stack.back());
        vm_.stack.pop_back();
        break;
      case Op::ObjectGetter:
      case Op::ObjectSetter: {
        const Value& accessor = vm_.stack.back();
        if (!accessor.IsObject()) {
          pending = Throw("TypeError", "an accessor must be a function");
          threw = true;
          break;
        }
        const bool getter = instruction.op == Op::ObjectGetter;
        // Defined rather than set, so that `{ get x(){}, set x(v){} }` fills in
        // the two halves of one property rather than the second replacing the
        // first.
        vm_.stack[vm_.stack.size() - 3].object->DefineAccessor(
            KeyFrom(vm_.stack[vm_.stack.size() - 2]), getter ? accessor.object : nullptr,
            getter ? nullptr : accessor.object);
        vm_.stack.resize(vm_.stack.size() - 2);
        break;
      }
      case Op::ObjectSpread: {
        const Value source = vm_.stack.back();
        if (source.IsObject()) {
          // The key list is copied first: reading a property can run a getter,
          // and a getter that adds a property to the source would otherwise
          // invalidate the sequence being walked.
          const std::vector<std::string> keys = source.object->Keys();
          for (const std::string& key : keys) {
            const Value value = GetProperty(source, key);
            vm_.stack[vm_.stack.size() - 2].object->Set(key, value);
          }
        }
        vm_.stack.pop_back();
        break;
      }
      case Op::Closure:
      case Op::ClosureArrow: {
        Object* function = heap_.AllocateObject(Object::Kind::Function);
        if (function == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        const CompiledFunction& target = *code.functions[instruction.a];
        const bool arrow = instruction.op == Op::ClosureArrow;
        function->SetPrototype(well_known_.function_prototype);
        function->MakeCompiled(&target, CurrentScope(), arrow);
        function->Set("name", Value::String(target.name));
        function->Set("length", Value::Number(static_cast<double>(target.parameter_count)));
        if (arrow) {
          // Captured now, not at call time. That is the entire semantic
          // difference between an arrow and a function expression.
          Value* self = CurrentScope()->Lookup("this");
          function->SetBoundThis(self == nullptr ? Value::Undefined() : *self);
        } else {
          // Every ordinary function gets a fresh `prototype` object, because any
          // of them can be called with `new`. An arrow cannot, which is why it
          // does not get one.
          if (Object* prototype = NewObject()) {
            prototype->Set("constructor", Value::Obj(function));
            function->Set("prototype", Value::Obj(prototype));
          }
        }
        vm_.stack.push_back(Value::Obj(function));
        break;
      }
      case Op::ClassLiteral: {
        // Handed to the tree-walking evaluator, which holds values in C++
        // locals -- so the depth goes up and no safepoint fires underneath.
        ++call_depth_;
        const Result value = EvaluateClass(*code.nodes[instruction.a], *CurrentScope(), &code);
        --call_depth_;
        if (value.IsAbrupt()) {
          pending = value;
          threw = true;
          break;
        }
        vm_.stack.push_back(value.value);
        break;
      }
      case Op::LoadSuperBase: {
        // The prototype of the object the method was *defined* on, not the one
        // it was called through. Using the receiver instead makes a three-level
        // hierarchy recurse into itself.
        Value* home = FrameName("__home__", kSlotHome);
        if (home == nullptr || !home->IsObject() || home->object->Prototype() == nullptr) {
          pending = Throw("SyntaxError", "'super' is only valid inside a method");
          threw = true;
          break;
        }
        vm_.stack.push_back(Value::Obj(home->object->Prototype()));
        break;
      }
      case Op::SuperCall: {
        // Copied out rather than pointed at, because everything below this
        // runs a constructor and a set of field initializers -- and a binding
        // read out of the frame is a pointer into a stack those can push onto.
        const Value* found = FrameName("__function__", kSlotFunction);
        const Value current = found == nullptr ? Value::Undefined() : *found;
        const Value* self = FrameName("this", kSlotThis);
        const Value instance = self == nullptr ? Value::Undefined() : *self;
        if (!current.IsObject() || current.object->SuperConstructor() == nullptr) {
          pending = Throw("SyntaxError", "'super' keyword unexpected here");
          threw = true;
          break;
        }
        const std::size_t first = vm_.stack.size() - instruction.a;
        const std::vector<Value> arguments(
            vm_.stack.begin() + static_cast<std::ptrdiff_t>(first), vm_.stack.end());
        Object* parent = current.object->SuperConstructor();
        // The parent constructor and the field initializers both hold values in
        // C++ locals while they run, so no safepoint fires underneath them.
        ++call_depth_;
        Result done = CallFunction(Value::Obj(parent), instance, arguments);
        if (!done.IsAbrupt() && instance.IsObject()) {
          // Fields of *this* class initialize after the super call, which is
          // the ordering that makes a derived field see a base one.
          done = InitializeFields(instance.object, current.object);
        }
        --call_depth_;
        vm_.stack.resize(first);
        if (done.IsAbrupt()) {
          pending = done;
          threw = true;
        }
        break;
      }
      case Op::RegExpLiteral: {
        const Result value = EvaluateRegExpLiteral(*code.nodes[instruction.a]);
        if (value.IsAbrupt()) {
          pending = value;
          threw = true;
          break;
        }
        vm_.stack.push_back(value.value);
        break;
      }
      case Op::TemplateStrings: {
        const TemplateParts parts = SplitTemplate(code.nodes[instruction.a]->string);
        std::vector<Value> chunks;
        chunks.reserve(parts.literals.size());
        for (const std::string& literal : parts.literals) {
          chunks.push_back(Value::String(literal));
        }
        const Value strings = NewArrayValue(chunks);
        if (!strings.IsObject()) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        // `raw` is the same text here: this engine does not process escapes in
        // a template separately, so the two agree for every template without a
        // backslash in it and differ only for those with one.
        strings.object->Set("raw", NewArrayValue(std::move(chunks)));
        vm_.stack.push_back(strings);
        break;
      }

      // --- Scopes ------------------------------------------------------------
      case Op::PushScope: {
        Environment* scope = heap_.AllocateEnvironment(CurrentScope());
        if (scope == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        // Sized before anything runs in it, so that a declaration control flow
        // skipped still leaves the ones after it where the compiler said.
        scope->Reserve(instruction.a);
        vm_.scopes.push_back(scope);
        break;
      }
      case Op::PopScope:
        vm_.scopes.resize(vm_.scopes.size() - instruction.a);
        break;
      case Op::ClearLocals: {
        // Entering a block in a flattened function. Its slots go back to
        // reserved-but-unset, which is what makes the second time round a loop
        // behave like the first: `x` before its own `let x` is a
        // ReferenceError every iteration, not just the one that declared it.
        const std::size_t first = frame->locals_base + LocalsBase(instruction.a);
        const std::size_t last = std::min(first + LocalsCount(instruction.a), vm_.locals.size());
        for (std::size_t i = first; i < last; ++i) {
          vm_.locals[i] = Binding{};
        }
        break;
      }

      // --- Iteration ---------------------------------------------------------
      case Op::IterateOpen: {
        if (vm_.iterations.size() >= kIterationCapacity) {
          pending = Throw("RangeError", "too many open iterations");
          threw = true;
          break;
        }
        // Reserved on the first cursor rather than in the constructor, the way
        // the locals stack is: a page that opens none should not pay for the
        // vector. From here it never grows, which is what keeps the reference
        // a stepping instruction holds valid across the page's own `next`.
        if (vm_.iterations.capacity() < kIterationCapacity) {
          vm_.iterations.reserve(kIterationCapacity);
        }
        Iteration cursor;
        const Result opened = OpenIteration(vm_.stack.back(), cursor);
        if (opened.IsAbrupt()) {
          pending = opened;
          threw = true;
          break;
        }
        vm_.iterations.push_back(cursor);
        vm_.stack.pop_back();
        break;
      }
      case Op::IterateNext:
      case Op::IterateStep: {
        Iteration& cursor = vm_.iterations.back();
        Value item;
        bool done = cursor.done;
        if (!done) {
          const Result advanced = StepIteration(cursor, item, done);
          if (advanced.IsAbrupt()) {
            pending = advanced;
            threw = true;
            break;
          }
          cursor.done = done;
        }
        if (done) {
          if (instruction.op == Op::IterateNext) {
            frame->ip = instruction.a;
          } else {
            // A pattern longer than its iterable fills the rest with undefined.
            vm_.stack.push_back(Value::Undefined());
          }
          break;
        }
        vm_.stack.push_back(std::move(item));
        break;
      }
      case Op::IterateDelegate: {
        Iteration& cursor = vm_.iterations.back();
        Value item;
        bool done = cursor.done;
        if (!done) {
          const Result advanced = StepIteration(cursor, item, done);
          if (advanced.IsAbrupt()) {
            pending = advanced;
            threw = true;
            break;
          }
          cursor.done = done;
        }
        if (done) {
          // `item` is the delegate's return value, which StepIteration reads
          // off the final result. Pushed rather than dropped, because it is
          // what the `yield*` expression is worth.
          frame->ip = instruction.a;
        }
        vm_.stack.push_back(std::move(item));
        break;
      }
      case Op::IterateRest: {
        Iteration& cursor = vm_.iterations.back();
        std::vector<Value> rest;
        bool done = cursor.done;
        while (!done) {
          Value item;
          const Result advanced = StepIteration(cursor, item, done);
          if (advanced.IsAbrupt()) {
            pending = advanced;
            threw = true;
            break;
          }
          if (done) {
            break;
          }
          if (rest.size() >= kMaxAllocationLength) {
            pending = Throw("RangeError", "too many values to destructure");
            threw = true;
            break;
          }
          rest.push_back(std::move(item));
        }
        if (threw) {
          break;
        }
        vm_.iterations.back().done = true;
        Object* array = NewArray(std::move(rest));
        if (array == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        vm_.stack.push_back(Value::Obj(array));
        break;
      }
      case Op::IterateClose: {
        // Not a resize any more: an iterator a loop walked away from is owed a
        // `return`, and for a generator that is the difference between its
        // frame being released and being filed for the life of the page.
        const std::size_t down_to = vm_.iterations.size() - instruction.a;
        const Result closed = CloseIterations(down_to);
        if (closed.IsAbrupt()) {
          pending = closed;
          threw = true;
        }
        break;
      }
      case Op::ForInKeys: {
        std::vector<Value> keys;
        const Value& subject = vm_.stack.back();
        if (subject.IsObject()) {
          Object* object = subject.object;
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
        Object* array = NewArray(std::move(keys));
        if (array == nullptr) {
          pending = Throw("RangeError", "out of memory");
          threw = true;
          break;
        }
        vm_.stack.back() = Value::Obj(array);
        break;
      }

      // --- The script's value ------------------------------------------------
      case Op::SetCompletion:
        vm_.stack[frame->stack_base + 2 + frame->argument_count] = vm_.stack.back();
        vm_.stack.pop_back();
        break;
      case Op::ClearCompletion:
        vm_.stack[frame->stack_base + 2 + frame->argument_count] = Value::Undefined();
        break;
      case Op::LoadCompletion:
        vm_.stack.push_back(vm_.stack[frame->stack_base + 2 + frame->argument_count]);
        break;

      // --- Abrupt ------------------------------------------------------------
      case Op::ThrowOp:
        pending = Result{Completion::Throw, vm_.stack.back(), {}};
        vm_.stack.pop_back();
        threw = true;
        break;
      case Op::ThrowSyntaxError:
        pending = Throw("SyntaxError", ToString(code.constants[instruction.a]));
        threw = true;
        break;
    }

    if (threw) {
      if (!UnwindToHandler(pending.value, entry_depth)) {
        return pending;
      }
      if (vm_.frames.size() == entry_depth) {
        // An async call whose rejection the unwinder turned into its promise,
        // with nobody left above to receive it. The promise is the result.
        Value out = vm_.stack.back();
        vm_.stack.pop_back();
        return Result::Normal(std::move(out));
      }
    }
  }
  return Result::Normal();
}

}  // namespace microbrowser::js

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "js/Interpreter.h"

// What a frame is, and everything done to one that is not running it.
//
// Split out of Vm.cpp, which is the instruction loop. The division is by
// subject rather than by size: below is how a call becomes a frame, how a name
// becomes a slot in one, how a throw takes frames off, and what the collector
// can see through them. The loop next door reads all of it and none of it reads
// the loop.
//
// The one thing worth reading twice is the order in UnwindToHandler. A frame
// leaving the stack is the last chance to answer whatever was waiting on the
// call it belonged to, and there are three different waiters -- nobody, an
// async call's promise, and an async generator's current request -- so the
// throw stops at a different place in each case.

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
  if (vm_.frames.capacity() < kFrameCapacity) {
    // Reserved once, on the first call that needs it. The capacity is fixed
    // because an instruction holds a Frame* into this while it runs -- a
    // reallocation under that pointer is a write into freed memory.
    vm_.frames.reserve(kFrameCapacity);
  }
  if (call_depth_ >= kMaxCallDepth || vm_.frames.size() >= kFrameCapacity) {
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
  if (!function->IsArrow()) {
    // Taken rather than read: `new.target` belongs to this call. An arrow has
    // none of its own -- like `this` -- so its slot stays unset and the walk
    // out finds the enclosing function's.
    declare(kSlotNewTarget, "__newtarget__", pending_new_target_, true);
    pending_new_target_ = Value::Undefined();
  }
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
  if (code->is_async && !code->is_generator) {
    // Made before a line of the body runs, because the body can suspend on its
    // first instruction and the caller has to be handed this either way.
    //
    // Not for an async generator: what its call hands back is the generator,
    // and its promises are one per `next` rather than one per call. Those live
    // on the request queue, which is the whole difference between the two.
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
    Object* generator = NewGenerator(code->is_async);
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

Value Interpreter::NewReturnSignal(const Value& value) {
  Object* signal = heap_.AllocateObject(Object::Kind::Plain);
  if (signal == nullptr) {
    return Value::Undefined();
  }
  // Identified by its prototype rather than by a property, because a property
  // is something a finalizer could delete if it ever got hold of one.
  signal->SetPrototype(well_known_.return_signal);
  signal->Set("value", value);
  return Value::Obj(signal);
}

bool Interpreter::IsReturnSignal(const Value& thrown) const {
  return thrown.IsObject() && well_known_.return_signal != nullptr &&
         thrown.object->Prototype() == well_known_.return_signal;
}

bool Interpreter::UnwindToHandler(const Value& thrown, std::size_t entry_depth) {
  // A forced return travels as a throw so that it runs the finalizers a throw
  // would -- and must run none of the catch clauses one would.
  const bool forced_return = IsReturnSignal(thrown);
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
      if (forced_return && !handler.is_finally) {
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
    if (forced_return) {
      // Every finalizer between the `yield` and here has run. What is left is
      // the return the page asked for, delivered to whichever of the three
      // things is waiting on this frame.
      const Value* returned = thrown.object->GetOwn("value");
      const Value value = returned == nullptr ? Value::Undefined() : *returned;
      if (IsAsyncGeneratorFrame(done)) {
        SettleAsyncRequest(done.generator, value, true, false);
        CloseGenerator(done.generator);
        DrainAsyncRequests(done.generator, false, Value::Undefined());
      } else if (done.generator != nullptr) {
        CloseGenerator(done.generator);
      }
      vm_.stack.push_back(value);
      return true;
    }
    if (IsAsyncGeneratorFrame(done)) {
      // The throw stops here for the reason it stops at an async call: what the
      // caller holds is a promise, so a throw is a rejection. The difference is
      // which promise -- the one for the request being answered, and then the
      // same answer for everything queued behind it.
      SettleAsyncRequest(done.generator, thrown, true, true);
      CloseGenerator(done.generator);
      DrainAsyncRequests(done.generator, true, thrown);
      vm_.stack.push_back(Value::Obj(done.generator));
      return true;
    }
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

}  // namespace microbrowser::js

#include <cstdint>
#include <utility>
#include <vector>

#include "js/Interpreter.h"

// Suspending and resuming a call.
//
// This is the file the bytecode machine was built for. A tree-walker's state is
// C++ stack frames: `await` has nowhere to go, because there is no way to put a
// C++ frame down and pick it up again. Here a frame is a record and every stack
// it points into is a vector, so suspending is copying five slices out and
// putting them back.
//
// The five are the frame itself, its slice of the value stack, the block scopes
// it pushed, the `for...of` cursors it left open, and its bindings when they are
// in the frame rather than on the heap. That list is not a design choice -- it
// is the same list the handler table records depths of, and for the same reason:
// an `await` can happen half way through an expression, inside three blocks,
// with two cursors open.
//
// Two things make it simpler than it sounds:
//
//   * **The suspending frame is always the top one.** `await` appears only in
//     the body of the async function it belongs to, so nothing of anyone else's
//     is above it when the instruction runs. Taking the top slice off five
//     vectors is a resize.
//
//   * **A resumed frame has no caller.** It is put back on an empty machine by
//     the microtask drain, and where its result would have gone is a slot
//     nobody reads. What the call actually returns went to its caller at the
//     first `await`: the promise, which the frame carries and settles.

namespace microbrowser::js {

namespace {

// A page can `await` a promise nobody resolves, and that call is then waiting
// for ever -- its frame filed, its values held. A real engine frees it when the
// promise becomes unreachable; here the filed frames are roots, so they do not
// become unreachable and the count has to be bounded instead. Past this,
// `await` throws, which is the answer a page gets for running out of any other
// bounded resource.
constexpr std::size_t kMaxSuspendedCalls = 10'000;

}  // namespace

void Interpreter::GatherSuspensionRoots(std::vector<Object*>& objects,
                                        std::vector<Environment*>& scopes) const {
  for (const auto& [id, suspension] : suspensions_.live) {
    const Frame& frame = suspension.frame;
    for (Object* held : {frame.function, frame.promise}) {
      if (held != nullptr) {
        objects.push_back(held);
      }
    }
    if (frame.scope != nullptr) {
      scopes.push_back(frame.scope);
    }
    for (const Value& value : suspension.stack) {
      if (value.IsObject() || value.IsSymbol()) {
        objects.push_back(value.object);
      }
    }
    for (const Binding& binding : suspension.locals) {
      if (binding.live && (binding.value.IsObject() || binding.value.IsSymbol())) {
        objects.push_back(binding.value.object);
      }
    }
    for (Environment* scope : suspension.scopes) {
      if (scope != nullptr) {
        scopes.push_back(scope);
      }
    }
    for (const Iteration& cursor : suspension.iterations) {
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
}

Result Interpreter::SuspendForAwait(const Value& awaited) {
  if (vm_.frames.empty()) {
    return Throw("SyntaxError", "await is only valid inside an async function");
  }
  const Frame& frame = vm_.frames.back();
  if (frame.promise == nullptr) {
    // The compiler rejects `await` outside an async function, so reaching this
    // means a chunk and a frame that disagree rather than a program that does
    // something unusual. An error beats reading a null.
    return Throw("SyntaxError", "await is only valid inside an async function");
  }
  if (suspensions_.live.size() >= kMaxSuspendedCalls) {
    return Throw("RangeError", "too many suspended async calls");
  }

  const std::uint64_t id = suspensions_.next++;
  Suspension suspension;
  suspension.frame = frame;
  // Everything from the callee slot up, so that the arguments come back too:
  // LoadArgument indexes into this region and the resumed frame has to find
  // them where it left them.
  suspension.stack.assign(vm_.stack.begin() + static_cast<std::ptrdiff_t>(frame.stack_base),
                          vm_.stack.end());
  suspension.scopes.assign(vm_.scopes.begin() + static_cast<std::ptrdiff_t>(frame.scope_base),
                           vm_.scopes.end());
  suspension.iterations.assign(
      vm_.iterations.begin() + static_cast<std::ptrdiff_t>(frame.iteration_base),
      vm_.iterations.end());
  suspension.locals.assign(vm_.locals.begin() + static_cast<std::ptrdiff_t>(frame.locals_base),
                           vm_.locals.end());

  Object* promise = frame.promise;
  const std::size_t stack_base = frame.stack_base;
  const std::size_t scope_base = frame.scope_base;
  const std::size_t iteration_base = frame.iteration_base;
  const std::size_t locals_base = frame.locals_base;

  // Filed before the machine is unwound, so that a collection between the two
  // finds these values in one place or the other and never in neither.
  suspensions_.live.emplace(id, std::move(suspension));

  vm_.frames.pop_back();
  vm_.iterations.resize(iteration_base);
  vm_.scopes.resize(scope_base);
  vm_.locals.resize(locals_base);
  vm_.stack.resize(stack_base);
  // Where the call's result goes, which for an async call is its promise and is
  // the same place an ordinary Return writes to. The caller cannot tell that
  // the callee is not finished, which is the entire contract.
  vm_.stack.push_back(Value::Obj(promise));

  AwaitOn(awaited, id);
  return Result::Normal();
}

Result Interpreter::ResumeSuspended(std::uint64_t suspension, const Value& value, bool rejected) {
  const auto found = suspensions_.live.find(suspension);
  if (found == suspensions_.live.end()) {
    return Result::Normal();  // already resumed, which a settled-twice promise cannot cause
  }
  Suspension held = std::move(found->second);
  suspensions_.live.erase(found);

  if (vm_.stack.size() + held.stack.size() + 2 > kValueStackCapacity ||
      vm_.locals.size() + held.locals.size() > kLocalsCapacity ||
      vm_.frames.size() >= kFrameCapacity) {
    // Nowhere to put it back. The call cannot continue, so its promise is
    // rejected rather than the frame being dropped silently.
    const Result out = Throw("RangeError", "maximum call stack size exceeded");
    SettleAsyncResult(held.frame.promise, out.value, true);
    return out;
  }

  // Rebased: the machine's stacks are wherever this turn left them, so every
  // base the frame recorded moves by the same delta.
  const std::size_t stack_base = vm_.stack.size();
  held.frame.stack_base = stack_base;
  held.frame.argument_base = stack_base + 2;
  held.frame.scope_base = vm_.scopes.size();
  held.frame.iteration_base = vm_.iterations.size();
  held.frame.locals_base = vm_.locals.size();
  for (const Value& slot : held.stack) {
    vm_.stack.push_back(slot);
  }
  for (Environment* scope : held.scopes) {
    vm_.scopes.push_back(scope);
  }
  for (const Iteration& cursor : held.iterations) {
    vm_.iterations.push_back(cursor);
  }
  for (const Binding& binding : held.locals) {
    vm_.locals.push_back(binding);
  }

  const std::size_t entry_depth = vm_.frames.size();
  vm_.frames.push_back(held.frame);

  if (rejected) {
    // The awaited promise was rejected, so the `await` throws. The frame's ip
    // is one past the Await instruction, which is what UnwindToHandler reads
    // back to -- so a `try` the await was written inside catches it, and
    // nothing else has to know that the throw crossed a turn.
    if (!UnwindToHandler(value, entry_depth)) {
      return Result{Completion::Throw, value, {}};
    }
    if (vm_.frames.size() == entry_depth) {
      // The unwinder rejected the call's promise and popped its frame. The
      // value it left is that promise, which nobody here wants.
      vm_.stack.resize(stack_base);
      return Result::Normal();
    }
  } else {
    vm_.stack.push_back(value);
  }

  Result result = RunFrames(entry_depth);
  vm_.stack.resize(stack_base);
  return result;
}

}  // namespace microbrowser::js

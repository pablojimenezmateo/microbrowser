#include <cstdint>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// Suspending and resuming a call: `await`, and `yield`.
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
// **`await` and `yield` are the same machinery with different triggers.** That
// is the whole of what a generator adds here. FileRunningFrame and PutFrameBack
// are the two halves, shared verbatim; what differs is only
//
//   * what puts the frame back -- a settled promise's microtask for `await`, a
//     call to `next`/`throw`/`return` for a generator;
//   * where the id of the filed frame is remembered -- in a promise reaction
//     for `await`, on the generator's own state for a generator;
//   * what the caller was handed when the frame first came off -- the promise,
//     or the generator object itself.
//
// Two things make it simpler than it sounds:
//
//   * **The suspending frame is always the top one.** `await` appears only in
//     the body of the async function it belongs to, and `yield` only in the
//     body of its generator, so nothing of anyone else's is above it when the
//     instruction runs. Taking the top slice off five vectors is a resize.
//
//   * **A resumed frame has no caller.** An awaiting one is put back on an
//     empty machine by the microtask drain, and a generator's is put back by a
//     native `next` with the frames of whoever called it below; either way
//     where its result would have gone is a slot nobody reads. What the call
//     actually returned went to its caller at the first suspend -- the promise
//     or the generator, which the frame carries.
//
// What is not here: async generators. `async function*` is both flags at once,
// and the compiler refuses it rather than picking one -- the two suspends would
// have to compose (an `await` inside a generator body files a frame that a
// microtask must put back and then re-file at the next `yield`), and composing
// them is a design rather than an addition. `for await...of` waits on the same.

namespace microbrowser::js {

namespace {

// A page can `await` a promise nobody resolves, and that call is then waiting
// for ever -- its frame filed, its values held. A real engine frees it when the
// promise becomes unreachable; here the filed frames are roots, so they do not
// become unreachable and the count has to be bounded instead. Past this,
// `await` throws, which is the answer a page gets for running out of any other
// bounded resource.
constexpr std::size_t kMaxSuspendedCalls = 10'000;

// The same bound, one queue over. A page can call an async generator's `next`
// in a loop without awaiting any of them, and every unanswered request is a
// promise held here until the body gets to it.
constexpr std::size_t kMaxQueuedRequests = 10'000;

}  // namespace

void Interpreter::GatherSuspensionRoots(std::vector<Object*>& objects,
                                        std::vector<Environment*>& scopes) const {
  for (const auto& [id, suspension] : suspensions_.live) {
    const Frame& frame = suspension.frame;
    for (Object* held : {frame.function, frame.promise, frame.generator}) {
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

std::uint64_t Interpreter::FileRunningFrame(Value result) {
  if (suspensions_.live.size() >= kMaxSuspendedCalls) {
    return 0;  // ids start at one, so zero is "nowhere to put it"
  }
  const std::uint64_t id = suspensions_.next++;
  const Frame& frame = vm_.frames.back();
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
  // Where the call's result goes -- the same slot an ordinary Return writes to.
  // For an async call that is its promise and for a generator's first suspend
  // it is the generator; either way the caller cannot tell that the callee is
  // not finished, which is the entire contract. `result` is taken by value
  // because every caller has one that came off the stack this just truncated.
  vm_.stack.push_back(std::move(result));
  return id;
}

bool Interpreter::PutFrameBack(Suspension& held) {
  if (vm_.stack.size() + held.stack.size() + 2 > kValueStackCapacity ||
      vm_.locals.size() + held.locals.size() > kLocalsCapacity ||
      vm_.iterations.size() + held.iterations.size() > kIterationCapacity ||
      vm_.frames.size() >= kFrameCapacity) {
    return false;
  }
  // A frame filed with cursors open is put back with them open, and this is the
  // one push onto the cursor stack that does not go through IterateOpen -- so
  // the reservation has to happen here too, or a resume is where the vector
  // first grows.
  if (!held.iterations.empty() && vm_.iterations.capacity() < kIterationCapacity) {
    vm_.iterations.reserve(kIterationCapacity);
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
  vm_.frames.push_back(held.frame);
  return true;
}

Result Interpreter::SuspendForAwait(const Value& awaited) {
  const bool inside_async =
      !vm_.frames.empty() &&
      (vm_.frames.back().promise != nullptr || IsAsyncGeneratorFrame(vm_.frames.back()));
  if (!inside_async) {
    // The compiler rejects `await` outside an async function, so reaching this
    // means a chunk and a frame that disagree rather than a program that does
    // something unusual. An error beats reading a null.
    return Throw("SyntaxError", "await is only valid inside an async function");
  }
  // An async generator has no promise of its own -- its promises are one per
  // request -- so what goes where the call's result would have gone is the
  // generator. Nobody reads it either way: what the caller actually received
  // went to it at the entry suspend.
  Object* generator = vm_.frames.back().generator;
  Object* promise = vm_.frames.back().promise;
  const std::uint64_t id =
      FileRunningFrame(promise != nullptr ? Value::Obj(promise) : Value::Obj(generator));
  if (id == 0) {
    return Throw("RangeError", "too many suspended async calls");
  }
  if (generator != nullptr) {
    // Awaiting, not Suspended: a `next` arriving now must queue rather than
    // put the frame back, because a settled promise is already going to.
    if (GeneratorState* state = heap_.FindGenerator(generator)) {
      state->suspension = id;
      state->status = GeneratorState::Status::Awaiting;
    }
  }
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
  // Null unless this is an async generator's `await`, in which case the
  // request being answered has not been settled yet and the pump owes it a
  // turn once the body gets somewhere.
  Object* generator = IsAsyncGeneratorFrame(held.frame) ? held.frame.generator : nullptr;

  const std::size_t entry_depth = vm_.frames.size();
  const std::size_t stack_base = vm_.stack.size();
  if (!PutFrameBack(held)) {
    // Nowhere to put it back. The call cannot continue, so its promise is
    // rejected rather than the frame being dropped silently.
    const Result out = Throw("RangeError", "maximum call stack size exceeded");
    if (generator != nullptr) {
      SettleAsyncRequest(generator, out.value, true, true);
      CloseGenerator(generator);
      DrainAsyncRequests(generator, true, out.value);
      return out;
    }
    SettleAsyncResult(held.frame.promise, out.value, true);
    return out;
  }
  if (generator != nullptr) {
    if (GeneratorState* state = heap_.FindGenerator(generator)) {
      state->suspension = 0;
      state->status = GeneratorState::Status::Running;
    }
  }

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
  if (generator != nullptr) {
    // The body ran to a `yield`, a `return`, another `await`, or off the end;
    // each of those settled the request it was answering. Whatever is queued
    // behind it is this pump's business, and this is the one place that can
    // do it -- a microtask is what put the frame back, so there is no `next`
    // above waiting to.
    const GeneratorState* state = heap_.FindGenerator(generator);
    if (state != nullptr && state->status == GeneratorState::Status::Running) {
      CloseGenerator(generator);
    }
    PumpAsyncGenerator(generator);
  }
  return result;
}

// --- Generators -------------------------------------------------------------

Object* Interpreter::NewGenerator(bool is_async) {
  Object* generator = heap_.AllocateObject(Object::Kind::Plain);
  if (generator == nullptr) {
    return nullptr;
  }
  generator->SetPrototype(is_async ? well_known_.async_generator_prototype
                                   : well_known_.generator_prototype);
  // Attached now rather than at the first suspend, so that every generator
  // object in existence has state -- a lookup that comes back null is then a
  // page calling `next` on something that is not a generator, which is a
  // TypeError, rather than a case to guess about.
  heap_.AttachGenerator(generator);
  return generator;
}

Result Interpreter::SuspendForYield(const Value& value, GeneratorState::Status status) {
  if (vm_.frames.empty() || vm_.frames.back().generator == nullptr) {
    // The compiler rejects `yield` outside a generator, so reaching this means
    // a chunk and a frame that disagree rather than a program doing something
    // unusual. An error beats reading a null.
    return Throw("SyntaxError", "yield is only valid inside a generator");
  }
  Object* generator = vm_.frames.back().generator;
  const std::uint64_t id = FileRunningFrame(value);
  if (id == 0) {
    return Throw("RangeError", "too many suspended calls");
  }
  // Looked up after the frame is filed rather than before: the generator is
  // held alive by the filed frame either way, and doing it in this order means
  // there is no window where the state says Suspended and no frame is filed.
  GeneratorState* state = heap_.FindGenerator(generator);
  if (state == nullptr) {
    return Throw("TypeError", "this generator has no state");
  }
  state->suspension = id;
  state->status = status;
  return Result::Normal();
}

Result Interpreter::ResumeGenerator(Object* generator, const Value& sent, bool thrown) {
  GeneratorState* state = generator == nullptr ? nullptr : heap_.FindGenerator(generator);
  if (state == nullptr) {
    return Throw("TypeError", "not a generator");
  }
  const auto found = suspensions_.live.find(state->suspension);
  if (found == suspensions_.live.end()) {
    // Nothing filed. A generator that has already finished, and the caller has
    // already handled that -- so this is a state that disagrees with the table
    // rather than an ordinary path. Completing it is the safe answer.
    state->status = GeneratorState::Status::Done;
    return Result::Normal();
  }
  Suspension held = std::move(found->second);
  suspensions_.live.erase(found);
  state->suspension = 0;
  // Running while its frame is on the machine, which is what makes a generator
  // that calls its own `next` a TypeError rather than the same frame put back
  // twice. Set before anything can run a line of the body.
  state->status = GeneratorState::Status::Running;

  const std::size_t entry_depth = vm_.frames.size();
  const std::size_t stack_base = vm_.stack.size();
  if (!PutFrameBack(held)) {
    // Nowhere to put it back, so the generator cannot continue. It is done
    // rather than merely stuck: the frame has already been taken out of the
    // table, and leaving a status that says otherwise would mean a `next` that
    // looks for a frame nothing holds.
    state->status = GeneratorState::Status::Done;
    return Throw("RangeError", "maximum call stack size exceeded");
  }

  if (thrown) {
    // `g.throw(e)`: the value arrives at the `yield` as a throw rather than as
    // a sent value. The frame's ip is one past the Yield, which is what
    // UnwindToHandler reads back to -- so a `try` the yield was written inside
    // catches it, and nothing else has to know the throw crossed a call.
    if (!UnwindToHandler(sent, entry_depth)) {
      // Nothing in the body caught it. The unwinder has already popped the
      // frame and truncated every stack, so the throw goes to whoever called
      // `throw` -- which is what the spec says, and the generator is done.
      state->status = GeneratorState::Status::Done;
      vm_.stack.resize(stack_base);
      return Result{Completion::Throw, sent, {}};
    }
    if (vm_.frames.size() == entry_depth) {
      // The unwinder handled it and popped the frame without there being any
      // body left to run: a forced return through a generator with no `finally`
      // in it, or a throw that an async generator turned into a rejection. It
      // left one value behind, and for the first of those that value is what
      // the generator returned.
      Value out = vm_.stack.back();
      vm_.stack.pop_back();
      vm_.stack.resize(stack_base);
      return Result::Normal(std::move(out));
    }
  } else {
    vm_.stack.push_back(sent);
  }

  Result result = RunFrames(entry_depth);
  vm_.stack.resize(stack_base);
  // Looked up again rather than reused: the body just ran, and a body that
  // made another generator rehashed the table this points into.
  state = heap_.FindGenerator(generator);
  if (state != nullptr && state->status == GeneratorState::Status::Running) {
    // Still Running means nothing suspended it, so the body ran off its end or
    // threw. A `yield` would have left it Suspended -- which is how a resume
    // tells a yielded value from a returned one without the two being
    // different kinds of Result.
    state->status = GeneratorState::Status::Done;
  }
  return result;
}

namespace {

// `{ value, done }`, which every one of the three methods returns.
Value IterationResult(Interpreter& interpreter, Value value, bool done) {
  Value result = interpreter.NewObjectValue();
  if (!result.IsObject()) {
    return result;  // the heap is full, and the caller above turned that into a throw
  }
  result.object->Set("value", std::move(value));
  result.object->Set("done", Value::Bool(done));
  return result;
}

// The generator a method was called on, or null when it was called on
// something else -- which a page can arrange with
// `Object.getPrototypeOf(g()).next.call({})`.
GeneratorState* SelfState(NativeCall& call) {
  if (!call.self.IsObject()) {
    return nullptr;
  }
  return call.interpreter.GetHeap().FindGenerator(call.self.object);
}

}  // namespace

void Interpreter::InstallGeneratorPrototype() {
  Object* prototype = well_known_.generator_prototype;

  InstallNative(prototype, "next", [](NativeCall& call) {
    GeneratorState* state = SelfState(call);
    if (state == nullptr) {
      return call.Throw("TypeError", "next called on something that is not a generator");
    }
    if (state->status == GeneratorState::Status::Running) {
      return call.Throw("TypeError", "this generator is already running");
    }
    if (state->status == GeneratorState::Status::Done) {
      return IterationResult(call.interpreter, Value::Undefined(), true);
    }
    Object* generator = call.self.object;
    const Result stepped =
        call.interpreter.ResumeGenerator(generator, Argument(call.arguments, 0), false);
    if (stepped.IsAbrupt()) {
      return call.ThrowValue(stepped.value);
    }
    // Suspended means a `yield` filed the frame again, so there is more to
    // come. Anything else means the body finished, and what came back is what
    // it returned.
    const GeneratorState* after = call.interpreter.GetHeap().FindGenerator(generator);
    const bool done = after == nullptr || after->status != GeneratorState::Status::Suspended;
    return IterationResult(call.interpreter, stepped.value, done);
  });

  InstallNative(prototype, "throw", [](NativeCall& call) {
    GeneratorState* state = SelfState(call);
    if (state == nullptr) {
      return call.Throw("TypeError", "throw called on something that is not a generator");
    }
    if (state->status == GeneratorState::Status::Running) {
      return call.Throw("TypeError", "this generator is already running");
    }
    const Value thrown = Argument(call.arguments, 0);
    if (state->status != GeneratorState::Status::Suspended) {
      // Not started, or finished. Either way the body does not run: a `try` in
      // it has not been entered, so there is nothing that could catch this.
      // The generator is completed and the value is thrown at the caller.
      call.interpreter.CloseGenerator(call.self.object);
      return call.ThrowValue(thrown);
    }
    Object* generator = call.self.object;
    const Result stepped = call.interpreter.ResumeGenerator(generator, thrown, true);
    if (stepped.IsAbrupt()) {
      return call.ThrowValue(stepped.value);
    }
    // Caught inside the body, which then went on to yield or to return.
    const GeneratorState* after = call.interpreter.GetHeap().FindGenerator(generator);
    const bool done = after == nullptr || after->status != GeneratorState::Status::Suspended;
    return IterationResult(call.interpreter, stepped.value, done);
  });

  InstallNative(prototype, "return", [](NativeCall& call) {
    GeneratorState* state = SelfState(call);
    if (state == nullptr) {
      return call.Throw("TypeError", "return called on something that is not a generator");
    }
    if (state->status == GeneratorState::Status::Running) {
      return call.Throw("TypeError", "this generator is already running");
    }
    const Value value = Argument(call.arguments, 0);
    if (state->status != GeneratorState::Status::Suspended) {
      // Not started, or finished. There is no `finally` that could have been
      // entered, so there is nothing to run and nothing to put back.
      call.interpreter.CloseGenerator(call.self.object);
      return IterationResult(call.interpreter, value, true);
    }
    // Sitting at a `yield`, so the body has to be resumed for its finalizers.
    // A `finally` can yield again, or return something else, or throw -- all
    // three are the body running, and all three are what the page wrote.
    Object* generator = call.self.object;
    const Result returned = call.interpreter.ReturnFromGenerator(generator, value);
    if (returned.IsAbrupt()) {
      return call.ThrowValue(returned.value);
    }
    const GeneratorState* after = call.interpreter.GetHeap().FindGenerator(generator);
    const bool done = after == nullptr || after->status != GeneratorState::Status::Suspended;
    return IterationResult(call.interpreter, returned.value, done);
  });

  // A generator is its own iterator, which is what makes `for (const x of g())`
  // work with nothing else installed -- and spread, and destructuring, and
  // every other thing that goes through OpenIteration.
  prototype->Set(PropertyKey::Symbol(SymbolIterator()),
                 NewNativeValue("[Symbol.iterator]", [](NativeCall& call) { return call.self; }));

  // %AsyncGeneratorPrototype%. Every method is the same shape: queue a request
  // of one of the three kinds and hand back the promise it will be answered
  // with. Which is the whole difference from the three above -- there, `next`
  // has an answer by the time it returns; here it has one later, and the queue
  // is what lets a second `next` arrive before the first is answered.
  Object* async_prototype = well_known_.async_generator_prototype;
  const auto request = [this, async_prototype](const char* name, AsyncRequest::Kind kind) {
    InstallNative(async_prototype, name, [kind, name](NativeCall& call) {
      Object* generator = call.self.IsObject() ? call.self.object : nullptr;
      if (generator == nullptr ||
          call.interpreter.GetHeap().FindGenerator(generator) == nullptr) {
        // A rejected promise rather than a throw: these return promises, and a
        // method that sometimes throws and sometimes rejects is two error
        // paths for one mistake.
        const Value promise = call.interpreter.NewPromiseValue();
        if (promise.IsObject()) {
          call.interpreter.SettleAsyncResult(
              promise.object,
              call.interpreter.MakeError(
                  "TypeError", std::string(name) + " called on something that is not an async "
                                                   "generator"),
              true);
        }
        return promise;
      }
      return call.interpreter.EnqueueAsyncRequest(generator, kind, Argument(call.arguments, 0));
    });
  };
  request("next", AsyncRequest::Kind::Next);
  request("throw", AsyncRequest::Kind::Throw);
  request("return", AsyncRequest::Kind::Return);

  if (well_known_.symbol_async_iterator != nullptr) {
    async_prototype->Set(
        PropertyKey::Symbol(well_known_.symbol_async_iterator),
        NewNativeValue("[Symbol.asyncIterator]", [](NativeCall& call) { return call.self; }));
  }
}

// --- Async generators -------------------------------------------------------

bool Interpreter::IsAsyncGeneratorFrame(const Frame& frame) {
  return frame.generator != nullptr && frame.code != nullptr && frame.code->is_async;
}

void Interpreter::SettleAsyncRequest(Object* generator, const Value& value, bool done,
                                     bool rejected) {
  const auto found = suspensions_.requests.find(generator);
  if (found == suspensions_.requests.end() || found->second.empty()) {
    // Nothing asked. Reachable when a body runs on past a `yield` nobody is
    // waiting for, which the pump does not do -- so this is a state that
    // disagrees with itself rather than an ordinary path, and dropping the
    // value is the only thing left to do with it.
    return;
  }
  const AsyncRequest request = found->second.front();
  found->second.erase(found->second.begin());
  if (found->second.empty()) {
    suspensions_.requests.erase(found);
  }
  if (rejected) {
    SettleAsyncResult(request.promise, value, true);
    return;
  }
  SettleAsyncResult(request.promise, IterationResult(*this, value, done), false);
}

void Interpreter::DrainAsyncRequests(Object* generator, bool rejected, const Value& reason) {
  const auto found = suspensions_.requests.find(generator);
  if (found == suspensions_.requests.end()) {
    return;
  }
  std::vector<AsyncRequest> queued = std::move(found->second);
  suspensions_.requests.erase(found);
  for (const AsyncRequest& request : queued) {
    if (rejected) {
      SettleAsyncResult(request.promise, reason, true);
      continue;
    }
    SettleAsyncResult(request.promise, IterationResult(*this, Value::Undefined(), true), false);
  }
}

Value Interpreter::EnqueueAsyncRequest(Object* generator, AsyncRequest::Kind kind,
                                       const Value& value) {
  const Value promise = NewPromiseValue();
  if (!promise.IsObject()) {
    return promise;
  }
  std::vector<AsyncRequest>& queue = suspensions_.requests[generator];
  if (queue.size() >= kMaxQueuedRequests) {
    // A page can call `next` in a loop without ever awaiting one, and every
    // unanswered request is a promise held here. Bounded for the reason the
    // suspensions are: past it the promise is rejected, which is an answer the
    // page can catch rather than an allocator failure.
    SettleAsyncResult(promise.object,
                      MakeError("RangeError", "too many pending requests on one async generator"),
                      true);
    return promise;
  }
  AsyncRequest request;
  request.promise = promise.object;
  request.value = value;
  request.kind = kind;
  queue.push_back(std::move(request));
  PumpAsyncGenerator(generator);
  return promise;
}

void Interpreter::PumpAsyncGenerator(Object* generator) {
  for (;;) {
    const GeneratorState* state = heap_.FindGenerator(generator);
    if (state == nullptr) {
      return;
    }
    if (state->status == GeneratorState::Status::Running ||
        state->status == GeneratorState::Status::Awaiting) {
      // A resume is in flight. Whoever finishes it pumps again, which is what
      // keeps this from putting the same frame back twice.
      return;
    }
    const auto found = suspensions_.requests.find(generator);
    if (found == suspensions_.requests.end() || found->second.empty()) {
      return;
    }
    const AsyncRequest::Kind kind = found->second.front().kind;
    const Value sent = found->second.front().value;

    // A finished generator, or one that has not started and is being asked for
    // something other than a value: answered without running a line, because
    // there is no `try` in the body that could have been entered.
    const bool unstarted_abrupt =
        state->status == GeneratorState::Status::Start && kind != AsyncRequest::Kind::Next;
    if (state->status == GeneratorState::Status::Done || unstarted_abrupt) {
      CloseGenerator(generator);
      switch (kind) {
        case AsyncRequest::Kind::Next:
          SettleAsyncRequest(generator, Value::Undefined(), true, false);
          break;
        case AsyncRequest::Kind::Return:
          SettleAsyncRequest(generator, sent, true, false);
          break;
        case AsyncRequest::Kind::Throw:
          SettleAsyncRequest(generator, sent, true, true);
          break;
      }
      continue;
    }

    if (kind == AsyncRequest::Kind::Return) {
      // Resumed for its finalizers, the way the sync generator's `return` is.
      // ReturnFromGenerator throws a signal no `catch` can see and every
      // `finally` stops at, and the frame boundary is where it becomes the
      // return -- which for an async generator means settling the request that
      // asked for it. So there is nothing to settle here.
      ReturnFromGenerator(generator, sent);
      continue;
    }

    // Suspended at a `yield`, or not started. Put the frame back and let it
    // run; whichever of Yield, Return or the unwinder it reaches settles the
    // request at the front of the queue, and the loop comes back for the next.
    ResumeGenerator(generator, sent, kind == AsyncRequest::Kind::Throw);
  }
}

Result Interpreter::ReturnFromGenerator(Object* generator, const Value& value) {
  GeneratorState* state = generator == nullptr ? nullptr : heap_.FindGenerator(generator);
  if (state == nullptr || state->status != GeneratorState::Status::Suspended) {
    CloseGenerator(generator);
    return Result::Normal(value);
  }
  const Value signal = NewReturnSignal(value);
  if (!signal.IsObject()) {
    // No room to build the signal, so the finalizers cannot be run. Dropping
    // the frame is worse than running them and is better than leaving it filed
    // for ever, which is the choice this whole path exists to avoid.
    CloseGenerator(generator);
    return Result::Normal(value);
  }
  // Resumed as a throw of the signal. UnwindToHandler skips every `catch` and
  // stops at every `finally`, and when the frame runs out it turns the signal
  // back into the return it always was.
  return ResumeGenerator(generator, signal, true);
}

void Interpreter::CloseGenerator(Object* generator) {
  GeneratorState* state = generator == nullptr ? nullptr : heap_.FindGenerator(generator);
  if (state == nullptr) {
    return;
  }
  // The filed frame goes with it. Without this the suspension stays a root for
  // the life of the interpreter, and a page that abandons generators in a loop
  // reaches the cap and starts getting RangeErrors from unrelated code.
  suspensions_.live.erase(state->suspension);
  state->suspension = 0;
  state->status = GeneratorState::Status::Done;
}

}  // namespace microbrowser::js

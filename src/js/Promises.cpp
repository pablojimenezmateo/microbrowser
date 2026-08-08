#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// Promise, and the microtask queue it settles through.
//
// The queue is the part that touches the browser rather than the language. A
// microtask exists only because something already ran -- a script, an event
// handler, a settled promise -- so draining it costs a wakeup that was already
// happening. It never asks for one of its own, which is what keeps the
// zero-idle-CPU invariant intact: there is no timer here, no poll, and nothing
// that has to be checked periodically to see whether a promise is ready.
//
// `setTimeout` is a different question and is not here. That one genuinely
// needs a wakeup, and it has to arrive as an `IdleWaitState::next_deadline_ms`
// rather than as a queue that gets polled.
//
// A promise's state lives in properties on the object, the same way a Map's
// entries do: the collector marks them for free, and a reaction list that the
// collector cannot see is a use-after-free waiting for a slow network.

namespace microbrowser::js {

namespace {

constexpr const char* kStateKey = "#state";
constexpr const char* kValueKey = "#value";
constexpr const char* kReactionsKey = "#reactions";
constexpr const char* kPromiseKey = "#promise";
constexpr const char* kAlreadyKey = "#settled";
// On a reaction that resumes a suspended `await` rather than calling a handler.
constexpr const char* kSuspensionKey = "#suspension";

// A promise is in exactly one of these, and only ever moves out of Pending.
enum class State : int { Pending = 0, Fulfilled = 1, Rejected = 2 };

bool IsPromise(const Value& value) {
  return value.IsObject() && value.object->GetOwn(kStateKey) != nullptr;
}

State StateOf(const Object& promise) {
  const Value* state = promise.GetOwn(kStateKey);
  return state == nullptr ? State::Pending : static_cast<State>(static_cast<int>(ToNumber(*state)));
}

// A reaction: what to run when the promise settles, and what to settle with the
// result. One record per `then`, held in an array on the promise so the
// collector sees it.
Value MakeReaction(Interpreter& interpreter, const Value& on_fulfilled,
                   const Value& on_rejected, const Value& derived) {
  Value reaction = interpreter.NewObjectValue();
  if (reaction.IsObject()) {
    reaction.object->SetHidden("#onFulfilled", on_fulfilled);
    reaction.object->SetHidden("#onRejected", on_rejected);
    reaction.object->SetHidden("#derived", derived);
  }
  return reaction;
}

void ScheduleReaction(Interpreter& interpreter, const Value& reaction, State state,
                      const Value& value) {
  if (!reaction.IsObject()) {
    return;
  }
  const bool rejected = state == State::Rejected;
  const Value* handler =
      reaction.object->GetOwn(rejected ? "#onRejected" : "#onFulfilled");
  const Value* derived = reaction.object->GetOwn("#derived");
  const Value* suspension = reaction.object->GetOwn(kSuspensionKey);
  Interpreter::Microtask task;
  task.callee = handler == nullptr ? Value::Undefined() : *handler;
  task.argument = value;
  task.derived = derived == nullptr ? Value::Undefined() : *derived;
  task.rejected = rejected;
  task.suspension =
      suspension == nullptr ? 0 : static_cast<std::uint64_t>(ToNumber(*suspension));
  interpreter.EnqueueMicrotask(std::move(task));
}

void SettlePromise(Interpreter& interpreter, const Value& promise, State state,
                   const Value& value);

// The spec's resolve procedure, which is not just "fulfill".
//
// A promise resolved with a thenable adopts it instead of fulfilling with it,
// which is the whole reason `return fetch(...)` inside a `then` flattens rather
// than handing the caller a promise of a promise.
void ResolvePromise(Interpreter& interpreter, const Value& promise, const Value& value) {
  if (!promise.IsObject()) {
    return;
  }
  if (value.IsObject() && value.object == promise.object) {
    // Resolving a promise with itself would wait for itself forever, so the
    // spec makes it a rejection rather than a hang.
    SettlePromise(interpreter, promise, State::Rejected,
                  interpreter.MakeError("TypeError", "a promise cannot resolve to itself"));
    return;
  }
  if (!value.IsObject()) {
    SettlePromise(interpreter, promise, State::Fulfilled, value);
    return;
  }
  const Value then = interpreter.GetPropertyValue(value, "then");
  if (!then.IsObject() || !then.object->IsCallable()) {
    SettlePromise(interpreter, promise, State::Fulfilled, value);
    return;
  }

  // A thenable's `then` is called from a microtask, not now: a page's own
  // thenable must not run during the resolve that found it.
  Value adopter = interpreter.NewNativeValue("", [](NativeCall& call) {
    const Value* target =
        call.callee == nullptr ? nullptr : call.callee->GetOwn(kPromiseKey);
    const Value* thenable = call.callee == nullptr ? nullptr : call.callee->GetOwn("#thenable");
    const Value* then_method = call.callee == nullptr ? nullptr : call.callee->GetOwn("#then");
    if (target == nullptr || thenable == nullptr || then_method == nullptr) {
      return Value::Undefined();
    }
    // One settle between them: whichever of the two runs first wins, and the
    // other is ignored. A page's thenable is allowed to call both.
    Value guard = call.interpreter.NewObjectValue();
    if (guard.IsObject()) {
      guard.object->Set(kAlreadyKey, Value::Bool(false));
    }
    const auto settler = [&call](State state) {
      return call.interpreter.NewNativeValue("", [state](NativeCall& inner) {
        const Value* once = inner.callee == nullptr ? nullptr : inner.callee->GetOwn("#once");
        if (once != nullptr && once->IsObject()) {
          const Value* already = once->object->GetOwn(kAlreadyKey);
          if (already != nullptr && ToBoolean(*already)) {
            return Value::Undefined();
          }
          once->object->Set(kAlreadyKey, Value::Bool(true));
        }
        const Value* subject =
            inner.callee == nullptr ? nullptr : inner.callee->GetOwn(kPromiseKey);
        if (subject == nullptr) {
          return Value::Undefined();
        }
        if (state == State::Fulfilled) {
          ResolvePromise(inner.interpreter, *subject, Argument(inner.arguments, 0));
        } else {
          SettlePromise(inner.interpreter, *subject, State::Rejected,
                        Argument(inner.arguments, 0));
        }
        return Value::Undefined();
      });
    };
    const Value resolve = settler(State::Fulfilled);
    const Value reject = settler(State::Rejected);
    for (const Value* half : {&resolve, &reject}) {
      if (half->IsObject()) {
        half->object->Set(kPromiseKey, *target);
        half->object->SetHidden("#once", guard);
      }
    }
    const Result called =
        call.interpreter.CallFunction(*then_method, *thenable, {resolve, reject});
    if (called.IsAbrupt()) {
      // A `then` that throws rejects the promise, unless it had already
      // settled it before throwing.
      const Value* already = guard.IsObject() ? guard.object->GetOwn(kAlreadyKey) : nullptr;
      if (already == nullptr || !ToBoolean(*already)) {
        if (guard.IsObject()) {
          guard.object->Set(kAlreadyKey, Value::Bool(true));
        }
        SettlePromise(call.interpreter, *target, State::Rejected, called.value);
      }
    }
    return Value::Undefined();
  });
  if (!adopter.IsObject()) {
    return;
  }
  adopter.object->Set(kPromiseKey, promise);
  adopter.object->SetHidden("#thenable", value);
  adopter.object->SetHidden("#then", then);
  Interpreter::Microtask task;
  task.callee = adopter;
  interpreter.EnqueueMicrotask(std::move(task));
}

void SettlePromise(Interpreter& interpreter, const Value& promise, State state,
                   const Value& value) {
  if (!promise.IsObject() || StateOf(*promise.object) != State::Pending) {
    return;  // a promise settles once; every later attempt is a no-op
  }
  promise.object->Set(kStateKey, Value::Number(static_cast<double>(state)));
  promise.object->Set(kValueKey, value);
  const Value* reactions = promise.object->GetOwn(kReactionsKey);
  if (reactions != nullptr && reactions->IsObject()) {
    for (std::size_t i = 0; i < reactions->object->ElementCount(); ++i) {
      ScheduleReaction(interpreter, reactions->object->GetElement(i), state, value);
    }
    // Dropped once scheduled: keeping them would keep every handler a
    // long-lived promise ever had alive for as long as the promise is.
    reactions->object->SetElements({}, {});
  }
}

// The `then` half of every combinator: attaches a reaction and returns the
// promise it will settle.
Value PerformThen(Interpreter& interpreter, const Value& promise, const Value& on_fulfilled,
                  const Value& on_rejected) {
  Value derived = interpreter.NewPromiseValue();
  if (!derived.IsObject() || !promise.IsObject()) {
    return derived;
  }
  const Value reaction = MakeReaction(interpreter, on_fulfilled, on_rejected, derived);
  const State state = StateOf(*promise.object);
  if (state == State::Pending) {
    const Value* reactions = promise.object->GetOwn(kReactionsKey);
    if (reactions != nullptr && reactions->IsObject() && reaction.IsObject()) {
      reactions->object->PushElement(reaction);
    }
    return derived;
  }
  const Value* settled = promise.object->GetOwn(kValueKey);
  ScheduleReaction(interpreter, reaction, state, settled == nullptr ? Value::Undefined()
                                                                   : *settled);
  return derived;
}

// A resolve/reject pair bound to one promise, for the executor and for the
// combinators. Both carry the promise as a property rather than a capture.
std::pair<Value, Value> MakeSettlers(Interpreter& interpreter, const Value& promise) {
  Value resolve = interpreter.NewNativeValue("resolve", [](NativeCall& call) {
    const Value* target = call.callee == nullptr ? nullptr : call.callee->GetOwn(kPromiseKey);
    if (target != nullptr) {
      ResolvePromise(call.interpreter, *target, Argument(call.arguments, 0));
    }
    return Value::Undefined();
  });
  Value reject = interpreter.NewNativeValue("reject", [](NativeCall& call) {
    const Value* target = call.callee == nullptr ? nullptr : call.callee->GetOwn(kPromiseKey);
    if (target != nullptr) {
      SettlePromise(call.interpreter, *target, State::Rejected, Argument(call.arguments, 0));
    }
    return Value::Undefined();
  });
  for (const Value* half : {&resolve, &reject}) {
    if (half->IsObject()) {
      half->object->Set(kPromiseKey, promise);
    }
  }
  return {resolve, reject};
}

// A promise for a value that may not be one. `Promise.resolve(p)` is `p`.
Value PromiseFor(Interpreter& interpreter, const Value& value) {
  if (IsPromise(value)) {
    return value;
  }
  const Value promise = interpreter.NewPromiseValue();
  ResolvePromise(interpreter, promise, value);
  return promise;
}

}  // namespace

// --- The queue -------------------------------------------------------------

void Interpreter::EnqueueMicrotask(Microtask task) {
  if (microtasks_.size() >= kMaxAllocationLength) {
    return;  // saturated rather than unbounded; the drain below is what bounds it
  }
  microtasks_.push_back(std::move(task));
}

void Interpreter::DrainMicrotasks() {
  // A microtask may queue another, and a page can write `const loop = () =>
  // Promise.resolve().then(loop); loop();` -- which is a genuine hang in every
  // browser, because the queue is drained to empty before anything else runs.
  //
  // It is not a hang here. Past the bound the drain stops and leaves the rest
  // queued, so the window stays responsive and the work continues on the next
  // turn. Losing promptness is a far better failure than losing the browser.
  //
  // **Budget refresh at the outermost drain only.** youtube's player bootstrap
  // does `NZU().then(initPlayer_)`; when the player module is already loaded,
  // NZU is `Promise.resolve([])` and `initPlayer_` → `Application.create` runs
  // as a microtask on whatever steps kevlar already spent. That throws
  // `script ran too long`, leaves the player proxy's `eue` flag stuck, and the
  // watch page never stamps `#movie_player`. Refreshing here — once per outer
  // drain, never per job and never when nested — is not the per-CallCompiled
  // reset TD-0018 forbids: the hang guard still covers the whole drain as one
  // budget, and a microtask storm still hits 20M and stops.
  const bool outermost = microtask_drain_depth_ == 0;
  ++microtask_drain_depth_;
  if (outermost && vm_.frames.empty() && steps_ > kMaxSteps / 2) {
    BeginHostTurn();
  }
  constexpr std::size_t kMaxJobsPerTurn = 100'000;
  for (std::size_t ran = 0; ran < kMaxJobsPerTurn && !microtasks_.empty(); ++ran) {
    // Between jobs, with nothing in progress and every queued job a root, is
    // as safe a point to collect as between top-level statements -- and it has
    // to happen here, or a long promise chain fills the heap without ever
    // reaching one. Before the pop, so the job about to run is still rooted.
    MaybeCollect();
    const Microtask task = microtasks_.front();
    microtasks_.erase(microtasks_.begin());

    // Rooted across the job. CallCompiled deliberately does not raise
    // call_depth_ (the machine's stacks are data and GatherVmRoots sees them),
    // so a safepoint inside a then-handler can collect anything that is only a
    // C++ local. The job was rooted while queued; once popped it is not -- and
    // `derived` is never pushed onto the VM stack, so without this a handler
    // that allocated past the threshold freed the promise ResolvePromise then
    // wrote. youtube.com hit it from requestAnimationFrame → DrainMicrotasks.
    // See ValueRoot.
    const ValueRoot root_callee(*this, task.callee);
    const ValueRoot root_argument(*this, task.argument);
    const ValueRoot root_derived(*this, task.derived);

    if (task.suspension != 0) {
      // A call waiting on an `await`. Put back and run here rather than through
      // a handler. `argument` stays rooted above for the same reason a then
      // handler's derived does: ResumeSuspended may collect before the value
      // is on the machine's stacks.
      const Result resumed = ResumeSuspended(task.suspension, task.argument, task.rejected);
      if (resumed.IsAbrupt()) {
        console_.push_back("Uncaught (in async function) " + ToString(resumed.value));
      }
      continue;
    }
    const bool has_handler = task.callee.IsObject() && task.callee.object->IsCallable();
    if (!task.derived.IsObject()) {
      // A plain job, from queueMicrotask or a thenable adoption. Nothing to
      // settle, and a throw has nowhere to go but the console.
      if (has_handler) {
        const Result ran_job = CallFunction(task.callee, Value::Undefined(), {task.argument});
        if (ran_job.IsAbrupt()) {
          console_.push_back("Uncaught (in microtask) " + ToString(ran_job.value));
        }
      }
      continue;
    }
    if (!has_handler) {
      // `p.then(f)` with a rejection, or `p.catch(f)` with a value: the
      // outcome passes through untouched to the derived promise. This is what
      // makes a rejection travel down a chain until something catches it.
      // ResolvePromise may still run a thenable's `then` through CallCompiled,
      // which is why `derived` stays rooted above rather than only for the
      // handler path.
      if (task.rejected) {
        SettlePromise(*this, task.derived, State::Rejected, task.argument);
      } else {
        ResolvePromise(*this, task.derived, task.argument);
      }
      continue;
    }
    const Result handled = CallFunction(task.callee, Value::Undefined(), {task.argument});
    // Same hole as the job fields: the handler's completion is a C++ local
    // while ResolvePromise may allocate (and collect) adopting a thenable.
    const ValueRoot root_handled(*this, handled.value);
    if (handled.IsAbrupt()) {
      SettlePromise(*this, task.derived, State::Rejected, handled.value);
      continue;
    }
    ResolvePromise(*this, task.derived, handled.value);
  }
  --microtask_drain_depth_;
}

Value Interpreter::NewPromiseValue() {
  Object* promise = heap_.AllocateObject(Object::Kind::Plain);
  if (promise == nullptr) {
    return Value::Undefined();
  }
  promise->SetPrototype(well_known_.promise_prototype);
  promise->Set(kStateKey, Value::Number(static_cast<double>(State::Pending)));
  promise->Set(kValueKey, Value::Undefined());
  promise->Set(kReactionsKey, NewArrayValue({}));
  return Value::Obj(promise);
}

// --- What an async call waits on and settles -------------------------------

void Interpreter::AwaitOn(const Value& value, std::uint64_t suspension) {
  const Value promise = PromiseFor(*this, value);
  if (!promise.IsObject()) {
    return;
  }
  // A reaction with no handlers and no derived promise: what it does when it
  // runs is put a frame back, and the drain does that itself rather than
  // calling anything. Everything else about it -- when it is scheduled, that a
  // rejection takes the other path -- is a reaction like any other, which is
  // why it is one.
  Value reaction = MakeReaction(*this, Value::Undefined(), Value::Undefined(),
                                Value::Undefined());
  if (!reaction.IsObject()) {
    return;
  }
  reaction.object->Set(kSuspensionKey, Value::Number(static_cast<double>(suspension)));
  const State state = StateOf(*promise.object);
  if (state == State::Pending) {
    const Value* reactions = promise.object->GetOwn(kReactionsKey);
    if (reactions != nullptr && reactions->IsObject()) {
      reactions->object->PushElement(reaction);
    }
    return;
  }
  const Value* settled = promise.object->GetOwn(kValueKey);
  ScheduleReaction(*this, reaction, state, settled == nullptr ? Value::Undefined() : *settled);
}

void Interpreter::SettleAsyncResult(Object* promise, const Value& value, bool rejected) {
  if (promise == nullptr) {
    return;
  }
  if (rejected) {
    SettlePromise(*this, Value::Obj(promise), State::Rejected, value);
    return;
  }
  // Resolve rather than fulfil: `async function f(){ return g() }` where `g` is
  // async has to hand the caller one promise rather than a promise of one.
  ResolvePromise(*this, Value::Obj(promise), value);
}

// --- Promise ---------------------------------------------------------------

void Interpreter::InstallPromises() {
  InstallNative(well_known_.promise_prototype, "then", [](NativeCall& call) {
    if (!IsPromise(call.self)) {
      return call.Throw("TypeError", "Promise.prototype.then called on a non-Promise");
    }
    return PerformThen(call.interpreter, call.self, Argument(call.arguments, 0),
                       Argument(call.arguments, 1));
  });
  InstallNative(well_known_.promise_prototype, "catch", [](NativeCall& call) {
    if (!IsPromise(call.self)) {
      return call.Throw("TypeError", "Promise.prototype.catch called on a non-Promise");
    }
    // `catch(f)` is `then(undefined, f)`, and writing it that way rather than
    // separately is what stops the two from disagreeing.
    return PerformThen(call.interpreter, call.self, Value::Undefined(),
                       Argument(call.arguments, 0));
  });
  InstallNative(well_known_.promise_prototype, "finally", [](NativeCall& call) {
    if (!IsPromise(call.self)) {
      return call.Throw("TypeError", "Promise.prototype.finally called on a non-Promise");
    }
    // Both handlers are the same function, and neither changes the outcome:
    // `finally` observes without intercepting. A rejection passes through it.
    const Value handler = Argument(call.arguments, 0);
    const auto wrap = [&call, &handler](bool rethrow) {
      Value wrapper = call.interpreter.NewNativeValue("", [rethrow](NativeCall& inner) {
        const Value* body = inner.callee == nullptr ? nullptr : inner.callee->GetOwn("#body");
        if (body != nullptr && body->IsObject() && body->object->IsCallable()) {
          const Result ran = inner.interpreter.CallFunction(*body, Value::Undefined(), {});
          if (ran.IsAbrupt()) {
            return inner.ThrowValue(ran.value);
          }
        }
        const Value passed = Argument(inner.arguments, 0);
        return rethrow ? inner.ThrowValue(passed) : passed;
      });
      if (wrapper.IsObject()) {
        wrapper.object->SetHidden("#body", handler);
      }
      return wrapper;
    };
    return PerformThen(call.interpreter, call.self, wrap(false), wrap(true));
  });

  Object* constructor = NewNative("Promise", [](NativeCall& call) {
    const Value executor = Argument(call.arguments, 0);
    if (!executor.IsObject() || !executor.object->IsCallable()) {
      return call.Throw("TypeError", "Promise resolver is not a function");
    }
    const Value promise = call.interpreter.NewPromiseValue();
    if (!promise.IsObject()) {
      return call.Throw("RangeError", "out of memory");
    }
    const std::pair<Value, Value> settlers = MakeSettlers(call.interpreter, promise);
    // The executor runs *now*, synchronously, which is the one part of a
    // promise that is not deferred. A page depends on it: the pattern
    // `new Promise(r => { handle = r })` only works because of it.
    const Result ran = call.interpreter.CallFunction(executor, Value::Undefined(),
                                                     {settlers.first, settlers.second});
    if (ran.IsAbrupt()) {
      SettlePromise(call.interpreter, promise, State::Rejected, ran.value);
    }
    return promise;
  });
  if (constructor == nullptr) {
    return;
  }
  constructor->Set("prototype", Value::Obj(well_known_.promise_prototype));
  well_known_.promise_prototype->SetHidden("constructor", Value::Obj(constructor));
  global_scope_->Declare("Promise", Value::Obj(constructor), false);

  InstallNative(constructor, "resolve", [](NativeCall& call) {
    return PromiseFor(call.interpreter, Argument(call.arguments, 0));
  });
  InstallNative(constructor, "reject", [](NativeCall& call) {
    const Value promise = call.interpreter.NewPromiseValue();
    SettlePromise(call.interpreter, promise, State::Rejected, Argument(call.arguments, 0));
    return promise;
  });

  // --- The combinators ------------------------------------------------------
  //
  // All four are one walk with three differences: what settles the result,
  // what each element contributes, and whether the first answer wins. Written
  // once with those named, rather than four times.
  enum class Combinator { All, AllSettled, Race, Any };
  const auto combinator = [this, constructor](const char* name, Combinator kind) {
    InstallNative(constructor, name, [kind](NativeCall& call) {
      const Value result = call.interpreter.NewPromiseValue();
      if (!result.IsObject()) {
        return call.Throw("RangeError", "out of memory");
      }
      std::vector<Value> items;
      const Result collected =
          call.interpreter.CollectIterable(Argument(call.arguments, 0), items);
      if (collected.IsAbrupt()) {
        SettlePromise(call.interpreter, result, State::Rejected, collected.value);
        return result;
      }

      // Shared state, as an object so the per-element handlers can reach it
      // without a capture: how many are still outstanding, and the slots each
      // will fill.
      Value shared = call.interpreter.NewObjectValue();
      if (!shared.IsObject()) {
        return call.Throw("RangeError", "out of memory");
      }
      shared.object->SetHidden("#remaining", Value::Number(static_cast<double>(items.size())));
      shared.object->SetHidden("#slots", call.interpreter.NewArrayValue(
                                       std::vector<Value>(items.size(), Value::Undefined())));
      shared.object->Set(kPromiseKey, result);

      if (items.empty()) {
        // An empty input settles immediately, and what it settles to differs:
        // `all` and `allSettled` fulfil with nothing, `any` rejects, and
        // `race` waits forever.
        if (kind == Combinator::All || kind == Combinator::AllSettled) {
          SettlePromise(call.interpreter, result, State::Fulfilled,
                        call.interpreter.NewArrayValue({}));
        } else if (kind == Combinator::Any) {
          SettlePromise(call.interpreter, result, State::Rejected,
                        call.interpreter.MakeError("AggregateError", "all promises were rejected"));
        }
        return result;
      }

      for (std::size_t i = 0; i < items.size(); ++i) {
        const Value element = PromiseFor(call.interpreter, items[i]);
        const auto handler = [&call, &shared, i, kind](bool rejected) {
          Value native = call.interpreter.NewNativeValue("", [kind, rejected](NativeCall& inner) {
            const Value* state = inner.callee == nullptr ? nullptr : inner.callee->GetOwn("#state");
            const Value* at = inner.callee == nullptr ? nullptr : inner.callee->GetOwn("#at");
            if (state == nullptr || !state->IsObject() || at == nullptr) {
              return Value::Undefined();
            }
            Object& shared_state = *state->object;
            const Value* target = shared_state.GetOwn(kPromiseKey);
            const Value* slots = shared_state.GetOwn("#slots");
            if (target == nullptr || slots == nullptr || !slots->IsObject()) {
              return Value::Undefined();
            }
            const Value outcome = Argument(inner.arguments, 0);
            const auto index = static_cast<std::size_t>(ToNumber(*at));

            // The first answer decides, for the two where it does.
            if (kind == Combinator::Race) {
              if (rejected) {
                SettlePromise(inner.interpreter, *target, State::Rejected, outcome);
              } else {
                ResolvePromise(inner.interpreter, *target, outcome);
              }
              return Value::Undefined();
            }
            if (kind == Combinator::All && rejected) {
              SettlePromise(inner.interpreter, *target, State::Rejected, outcome);
              return Value::Undefined();
            }
            if (kind == Combinator::Any && !rejected) {
              ResolvePromise(inner.interpreter, *target, outcome);
              return Value::Undefined();
            }

            Value recorded = outcome;
            if (kind == Combinator::AllSettled) {
              // Each slot is a record rather than a value, which is the whole
              // difference between allSettled and all.
              recorded = inner.interpreter.NewObjectValue();
              if (recorded.IsObject()) {
                recorded.object->Set("status",
                                     Value::String(rejected ? "rejected" : "fulfilled"));
                recorded.object->Set(rejected ? "reason" : "value", outcome);
              }
            }
            slots->object->SetElement(index, recorded);

            const Value* remaining = shared_state.GetOwn("#remaining");
            const double left = (remaining == nullptr ? 0.0 : ToNumber(*remaining)) - 1.0;
            shared_state.Set("#remaining", Value::Number(left));
            if (left > 0.0) {
              return Value::Undefined();
            }
            if (kind == Combinator::Any) {
              SettlePromise(inner.interpreter, *target, State::Rejected,
                            inner.interpreter.MakeError("AggregateError",
                                                        "all promises were rejected"));
            } else {
              SettlePromise(inner.interpreter, *target, State::Fulfilled, *slots);
            }
            return Value::Undefined();
          });
          if (native.IsObject()) {
            native.object->SetHidden("#state", shared);
            native.object->SetHidden("#at", Value::Number(static_cast<double>(i)));
          }
          return native;
        };
        PerformThen(call.interpreter, element, handler(false), handler(true));
      }
      return result;
    });
  };
  combinator("all", Combinator::All);
  combinator("allSettled", Combinator::AllSettled);
  combinator("race", Combinator::Race);
  combinator("any", Combinator::Any);

  // The one way a page reaches the queue directly. Same rule as everything
  // else here: it runs at the end of the turn that queued it.
  global_scope_->Declare(
      "queueMicrotask",
      NewNativeValue("queueMicrotask",
                     [](NativeCall& call) {
                       const Value job = Argument(call.arguments, 0);
                       if (!job.IsObject() || !job.object->IsCallable()) {
                         return call.Throw("TypeError", "queueMicrotask requires a function");
                       }
                       Microtask task;
                       task.callee = job;
                       call.interpreter.EnqueueMicrotask(std::move(task));
                       return Value::Undefined();
                     }),
      false);
}

}  // namespace microbrowser::js

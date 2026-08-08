#include "bindings/Timers.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "bindings/BindingSupport.h"
#include "bindings/TrustedScript.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where the callbacks live: a plain object on the global, keyed by timer id.
//
// In the JavaScript heap rather than in this class, for the reason the wrapper
// cache is: the collector cannot see a `js::Value` in a C++ field, and a
// callback it cannot see is one it will free while the timer still points at
// it. The global object is unconditionally a root.
constexpr const char* kCallbacksSlot = "#timers";
// The queue this class is, so the natives can reach it without a capture --
// captures are invisible to the collector, and this one is a raw pointer whose
// lifetime nothing would be tracking.
constexpr const char* kQueueSlot = "#timerQueue";
// The epoch a delay is measured from, kept beside the callbacks so the natives
// can read it without a second channel.
constexpr const char* kNowSlot = "#timerNow";

// The longest delay a page may ask for. Beyond about twenty-four days a
// millisecond count stops fitting the shapes browsers historically used, and a
// page that asks for a year is a page that has computed a delay wrong.
constexpr std::int64_t kMaxDelayMs = 2'147'483'647;

// Host tasks (MessageChannel) queued during this RunDue may run in the same
// turn, up to this many. HTML separates "run a task" from "update the
// rendering"; youtube's kevlar scheduler posts one stamp slice per channel
// message, and one LayoutAndPaint per slice made the post-load drain
// unusable (TD-0018). setTimeout(0) still waits for the next turn — that
// rule is what stops a busy page spinning forever inside one RunDue. Nested
// timeouts then clamp to 4ms (HTML), so the loop sleeps rather than spins.
constexpr int kMaxHostTasksPerTurn = 64;
// HTML: after five nested timer tasks, force at least 4ms.
constexpr std::uint32_t kTimerNestingClamp = 5;
constexpr std::int64_t kNestedTimerMinMs = 4;

js::Object* Callbacks(js::Interpreter& interpreter) {
  const Value* slot = interpreter.Global()->GetOwn(kCallbacksSlot);
  return slot != nullptr && slot->IsObject() ? slot->object : nullptr;
}

}  // namespace

bool TimerQueue::QueueTask(js::Interpreter& interpreter, const js::Value& callback) {
  if (!callback.IsObject() || !callback.object->IsCallable()) {
    return false;
  }
  js::Object* global = interpreter.Global();
  const Value* queue_slot = global->GetOwn(kQueueSlot);
  const Value* now_slot = global->GetOwn(kNowSlot);
  js::Object* callbacks = Callbacks(interpreter);
  if (queue_slot == nullptr || now_slot == nullptr || callbacks == nullptr) {
    return false;
  }
  auto* queue = reinterpret_cast<TimerQueue*>(static_cast<std::uintptr_t>(queue_slot->number));
  Timer timer;
  timer.id = queue->next_id_++;
  // Due *now*, which in this queue means "the next time RunDue is called" --
  // the same thing `setTimeout(f, 0)` gets, and the same thing the loop wakes
  // for.
  timer.due_ms = static_cast<std::int64_t>(now_slot->number);
  timer.host_task = true;
  timer.trust_scripts = TrustedScriptContextActive(interpreter);
  queue->timers_.push_back(timer);
  callbacks->Set(js::NumberToString(timer.id), callback);
  return true;
}

void TimerQueue::Install(js::Interpreter& interpreter, std::int64_t now_ms) {
  js::Object* global = interpreter.Global();
  const Value callbacks = interpreter.NewObjectValue();
  if (!callbacks.IsObject()) {
    return;
  }
  global->Set(kCallbacksSlot, callbacks);
  global->Set(kQueueSlot, PointerValue(this));
  global->Set(kNowSlot, Value::Number(static_cast<double>(now_ms)));

  const auto schedule = [&interpreter, global](const char* name, bool repeating) {
    const Value native = interpreter.NewNativeValue(name, [repeating](NativeCall& call) {
      const Value handler = Argument(call.arguments, 0);
      if (!handler.IsObject() || !handler.object->IsCallable()) {
        // A string argument would be `eval` by another name, and there is no
        // path from a string to running code in this engine. Refused rather
        // than ignored, so a page that relies on it fails where it wrote it.
        return call.Throw("TypeError", "a timer callback must be a function");
      }
      js::Object* host = call.interpreter.Global();
      const Value* queue_slot = host->GetOwn(kQueueSlot);
      const Value* now_slot = host->GetOwn(kNowSlot);
      js::Object* callbacks_object = Callbacks(call.interpreter);
      if (queue_slot == nullptr || now_slot == nullptr || callbacks_object == nullptr) {
        return Value::Number(0);
      }
      auto* queue = reinterpret_cast<TimerQueue*>(
          static_cast<std::uintptr_t>(queue_slot->number));

      // A negative or absent delay is zero, which means "after this turn"
      // rather than "now" -- the callback still waits for the queue.
      const double requested = js::ToNumber(Argument(call.arguments, 1));
      std::int64_t delay = std::clamp<std::int64_t>(
          std::isfinite(requested) ? static_cast<std::int64_t>(requested) : 0, 0, kMaxDelayMs);
      // Nested timer clamping (HTML): once five timer tasks deep, a sub-4ms
      // delay becomes 4ms. Without this, BeginTask-on-every-timeout plus
      // setTimeout(0) chains spun the loop at zero idle (TD-0018 hang).
      const std::uint32_t nesting = queue->active_nesting_;
      if (nesting > kTimerNestingClamp && delay < kNestedTimerMinMs) {
        delay = kNestedTimerMinMs;
      }

      Timer timer;
      timer.id = queue->next_id_++;
      timer.due_ms = static_cast<std::int64_t>(now_slot->number) + delay;
      timer.interval_ms = repeating ? std::max<std::int64_t>(delay, 1) : 0;
      timer.repeating = repeating;
      timer.nesting_level = nesting + 1;
      timer.trust_scripts = TrustedScriptContextActive(call.interpreter);
      queue->timers_.push_back(timer);
      callbacks_object->Set(js::NumberToString(timer.id), handler);
      return Value::Number(timer.id);
    });
    if (native.IsObject()) {
      global->Set(name, native);
      interpreter.GlobalScope()->Declare(name, native, false);
    }
  };
  schedule("setTimeout", false);
  schedule("setInterval", true);

  const auto cancel = [&interpreter, global](const char* name) {
    const Value native = interpreter.NewNativeValue(name, [](NativeCall& call) {
      js::Object* host = call.interpreter.Global();
      const Value* queue_slot = host->GetOwn(kQueueSlot);
      js::Object* callbacks_object = Callbacks(call.interpreter);
      if (queue_slot == nullptr || callbacks_object == nullptr) {
        return Value::Undefined();
      }
      auto* queue = reinterpret_cast<TimerQueue*>(
          static_cast<std::uintptr_t>(queue_slot->number));
      const double id = js::ToNumber(Argument(call.arguments, 0));
      queue->timers_.erase(std::remove_if(queue->timers_.begin(), queue->timers_.end(),
                                          [id](const Timer& timer) { return timer.id == id; }),
                           queue->timers_.end());
      // The callback goes too, or cancelling a timer would leak its closure
      // for as long as the page lives.
      callbacks_object->Delete(js::NumberToString(id));
      return Value::Undefined();
    });
    if (native.IsObject()) {
      global->Set(name, native);
      interpreter.GlobalScope()->Declare(name, native, false);
    }
  };
  cancel("clearTimeout");
  cancel("clearInterval");
}

std::optional<std::uint32_t> TimerQueue::NextDelay(std::int64_t now_ms) const {
  if (timers_.empty()) {
    return std::nullopt;  // nothing scheduled, so the loop may block
  }
  std::int64_t soonest = timers_.front().due_ms;
  for (const Timer& timer : timers_) {
    soonest = std::min(soonest, timer.due_ms);
  }
  // A deadline that has already passed is zero rather than negative, and the
  // idle policy turns a zero into one sleep rather than a spin.
  return static_cast<std::uint32_t>(std::max<std::int64_t>(0, soonest - now_ms));
}

bool TimerQueue::RunDue(js::Interpreter& interpreter, std::int64_t now_ms) {
  js::Object* global = interpreter.Global();
  global->Set(kNowSlot, Value::Number(static_cast<double>(now_ms)));
  js::Object* callbacks = Callbacks(interpreter);
  if (callbacks == nullptr || timers_.empty()) {
    return false;
  }

  // Everything due now, oldest deadline first, decided before any of them
  // runs: a callback may schedule another, and a *timer* scheduled with a zero
  // delay during this pass must wait for the next one or a page could spin the
  // loop forever inside a single turn. Host tasks (MessageChannel) are
  // different — see the drain below.
  std::vector<Timer> due;
  for (const Timer& timer : timers_) {
    if (timer.due_ms <= now_ms) {
      due.push_back(timer);
    }
  }
  if (due.empty()) {
    return false;
  }
  std::stable_sort(due.begin(), due.end(),
                   [](const Timer& a, const Timer& b) { return a.due_ms < b.due_ms; });

  const auto run_one = [&](const Timer& timer) {
    const std::string key = js::NumberToString(timer.id);
    const Value* handler = callbacks->GetOwn(key);
    if (handler == nullptr || !handler->IsObject()) {
      return;  // cancelled by an earlier callback in this same pass
    }
    const Value callback = *handler;
    if (timer.repeating) {
      for (Timer& live : timers_) {
        if (live.id == timer.id) {
          live.due_ms = now_ms + live.interval_ms;
          live.nesting_level = timer.nesting_level + 1;
        }
      }
    } else {
      timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                   [&timer](const Timer& each) { return each.id == timer.id; }),
                    timers_.end());
      callbacks->Delete(key);
    }
    // Every macrotask is a fresh host turn (TD-0018). Nested setTimeout(0)
    // chains are kept from spinning by the 4ms clamp above, not by sharing
    // a spent step budget.
    interpreter.BeginTask();
    active_nesting_ = timer.host_task ? 0 : timer.nesting_level;
    TrustedScriptInvocation trust(interpreter, timer.trust_scripts);
    (void)interpreter.CallFunction(callback, Value::Undefined(), {});
    active_nesting_ = 0;
  };

  for (const Timer& timer : due) {
    run_one(timer);
  }

  // Drain host tasks that the callbacks above just queued. Cooperative
  // schedulers post the next slice with due_ms == now; without this they each
  // force a LayoutAndPaint in the engine. Cap so a forever-posting channel
  // cannot own the turn — the rest wait for the next RunDue, which is when
  // the loop may paint.
  int batched = 0;
  while (batched < kMaxHostTasksPerTurn) {
    const Timer* next = nullptr;
    for (const Timer& timer : timers_) {
      if (!timer.host_task || timer.due_ms > now_ms) {
        continue;
      }
      if (next == nullptr || timer.due_ms < next->due_ms ||
          (timer.due_ms == next->due_ms && timer.id < next->id)) {
        next = &timer;
      }
    }
    if (next == nullptr) {
      break;
    }
    const Timer copy = *next;
    run_one(copy);
    ++batched;
  }
  if (batched > 0) {
    util::AddPerformanceCounter(util::PerfCounterId::TimersHostTasksBatched,
                                static_cast<std::uint64_t>(batched));
  }

  // A timer's callback is a turn of its own, so anything it queued settles
  // before the timer is over -- the same rule a script and an event get.
  interpreter.DrainMicrotasks();
  return true;
}

}  // namespace microbrowser::bindings

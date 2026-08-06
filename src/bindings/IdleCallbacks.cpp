#include "bindings/IdleCallbacks.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "bindings/BindingSupport.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

constexpr const char* kCallbacksSlot = "#idleCallbacks";
constexpr const char* kQueueSlot = "#idleQueue";
constexpr const char* kNowSlot = "#idleNow";
constexpr const char* kDeadlineStartSlot = "#idleDeadlineStart";

js::Object* Callbacks(js::Interpreter& interpreter) {
  const Value* slot = interpreter.Global()->GetOwn(kCallbacksSlot);
  return slot != nullptr && slot->IsObject() ? slot->object : nullptr;
}

Value MakeIdleDeadline(js::Interpreter& interpreter, std::int64_t start_ms, bool timed_out) {
  const Value deadline = interpreter.NewObjectValue();
  if (!deadline.IsObject()) {
    return Value::Undefined();
  }
  deadline.object->Set(kDeadlineStartSlot, Value::Number(static_cast<double>(start_ms)));
  deadline.object->Set("didTimeout", Value::Bool(timed_out));

  const Value time_remaining = interpreter.NewNativeValue(
      "timeRemaining", [](NativeCall& call) {
        const Value* start_slot = call.self.object->GetOwn(kDeadlineStartSlot);
        const Value* now_slot = call.interpreter.Global()->GetOwn(kNowSlot);
        if (start_slot == nullptr || now_slot == nullptr) {
          return Value::Number(0.0);
        }
        const std::int64_t deadline_start_ms = static_cast<std::int64_t>(js::ToNumber(*start_slot));
        const std::int64_t clock_now_ms = static_cast<std::int64_t>(js::ToNumber(*now_slot));
        const std::int64_t remaining = kIdleCallbackBudgetMs - (clock_now_ms - deadline_start_ms);
        return Value::Number(static_cast<double>(std::max<std::int64_t>(0, remaining)));
      });
  if (time_remaining.IsObject()) {
    deadline.object->Set("timeRemaining", time_remaining);
  }
  return deadline;
}

}  // namespace

void IdleCallbacks::Install(js::Interpreter& interpreter, std::int64_t now_ms) {
  js::Object* global = interpreter.Global();
  const Value callbacks = interpreter.NewObjectValue();
  if (!callbacks.IsObject()) {
    return;
  }
  global->Set(kCallbacksSlot, callbacks);
  global->Set(kQueueSlot, PointerValue(this));
  global->Set(kNowSlot, Value::Number(static_cast<double>(now_ms)));

  const auto define = [&interpreter, global](const char* name, auto body) {
    const Value native = interpreter.NewNativeValue(name, body);
    if (native.IsObject()) {
      global->Set(name, native);
      interpreter.GlobalScope()->Declare(name, native, false);
    }
  };

  define("requestIdleCallback", [](NativeCall& call) {
    const Value handler = Argument(call.arguments, 0);
    if (!handler.IsObject() || !handler.object->IsCallable()) {
      return call.Throw("TypeError", "an idle callback must be a function");
    }
    js::Object* host = call.interpreter.Global();
    const Value* queue_slot = host->GetOwn(kQueueSlot);
    const Value* now_slot = host->GetOwn(kNowSlot);
    js::Object* callbacks_object = Callbacks(call.interpreter);
    if (queue_slot == nullptr || now_slot == nullptr || callbacks_object == nullptr) {
      return Value::Number(0);
    }
    auto* queue =
        reinterpret_cast<IdleCallbacks*>(static_cast<std::uintptr_t>(queue_slot->number));
    const std::int64_t request_now_ms = static_cast<std::int64_t>(js::ToNumber(*now_slot));

    Entry entry;
    entry.id = queue->next_id_++;
    entry.idle_after_ms = request_now_ms;
    if (call.arguments.size() >= 2 && call.arguments[1].IsObject()) {
      const Value* timeout = call.arguments[1].object->GetOwn("timeout");
      if (timeout != nullptr) {
        const double requested = js::ToNumber(*timeout);
        if (std::isfinite(requested) && requested >= 0.0) {
          entry.has_timeout = true;
          entry.timeout_at_ms =
              request_now_ms + std::clamp<std::int64_t>(static_cast<std::int64_t>(requested), 0,
                                                std::numeric_limits<std::int64_t>::max() / 2);
        }
      }
    }
    queue->pending_.push_back(entry);
    callbacks_object->Set(js::NumberToString(entry.id), handler);
    return Value::Number(entry.id);
  });

  define("cancelIdleCallback", [](NativeCall& call) {
    js::Object* host = call.interpreter.Global();
    const Value* queue_slot = host->GetOwn(kQueueSlot);
    js::Object* callbacks_object = Callbacks(call.interpreter);
    if (queue_slot == nullptr || callbacks_object == nullptr) {
      return Value::Undefined();
    }
    auto* queue =
        reinterpret_cast<IdleCallbacks*>(static_cast<std::uintptr_t>(queue_slot->number));
    const double id = js::ToNumber(Argument(call.arguments, 0));
    queue->pending_.erase(std::remove_if(queue->pending_.begin(), queue->pending_.end(),
                                         [id](const Entry& entry) { return entry.id == id; }),
                          queue->pending_.end());
    callbacks_object->Delete(js::NumberToString(id));
    return Value::Undefined();
  });
}

std::optional<std::uint32_t> IdleCallbacks::NextDelay(std::int64_t now_ms) const {
  if (pending_.empty()) {
    return std::nullopt;
  }
  std::int64_t soonest = std::numeric_limits<std::int64_t>::max();
  for (const Entry& entry : pending_) {
    const std::int64_t idle_due = std::max<std::int64_t>(0, entry.idle_after_ms - now_ms);
    if (entry.has_timeout) {
      soonest = std::min(soonest, std::max<std::int64_t>(0, entry.timeout_at_ms - now_ms));
      soonest = std::min(soonest, idle_due);
    } else {
      soonest = std::min(soonest, idle_due);
    }
  }
  return static_cast<std::uint32_t>(soonest);
}

bool IdleCallbacks::RunDue(js::Interpreter& interpreter, std::int64_t now_ms) {
  js::Object* global = interpreter.Global();
  global->Set(kNowSlot, Value::Number(static_cast<double>(now_ms)));
  js::Object* callbacks = Callbacks(interpreter);
  if (callbacks == nullptr || pending_.empty()) {
    return false;
  }

  std::vector<Entry> due;
  for (const Entry& entry : pending_) {
    const bool timed_out = entry.has_timeout && now_ms >= entry.timeout_at_ms;
    const bool idle = now_ms >= entry.idle_after_ms;
    if (timed_out || idle) {
      due.push_back(entry);
    }
  }
  if (due.empty()) {
    return false;
  }

  for (const Entry& entry : due) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&entry](const Entry& live) { return live.id == entry.id; }),
                   pending_.end());
    const std::string key = js::NumberToString(entry.id);
    const Value* handler = callbacks->GetOwn(key);
    if (handler == nullptr || !handler->IsObject()) {
      continue;
    }
    const Value callback = *handler;
    callbacks->Delete(key);
    const bool timed_out = entry.has_timeout && now_ms >= entry.timeout_at_ms;
    const Value deadline = MakeIdleDeadline(interpreter, now_ms, timed_out);
    (void)interpreter.CallFunction(callback, Value::Undefined(), {deadline});
    AddPerformanceCounter(PerfCounterId::IdleCallbacksRun);
  }
  interpreter.DrainMicrotasks();
  return true;
}

}  // namespace microbrowser::bindings

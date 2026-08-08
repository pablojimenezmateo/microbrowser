#include "bindings/AnimationFrames.h"

#include <algorithm>
#include <string>
#include <utility>

#include "bindings/BindingSupport.h"
#include "bindings/TrustedScript.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// The callbacks, on the global, keyed by id -- in the JavaScript heap rather
// than in this class for the reason the timers' are: the collector cannot see a
// `js::Value` in a C++ field, and a callback it cannot see is one it frees
// while this still points at it. The global object is unconditionally a root.
constexpr const char* kCallbacksSlot = "#frames";
// This object, so the natives can reach it without a capture -- a capture is
// invisible to the collector, and this one is a raw pointer whose lifetime
// nothing would be tracking.
constexpr const char* kQueueSlot = "#frameQueue";

js::Object* Callbacks(js::Interpreter& interpreter) {
  const Value* slot = interpreter.Global()->GetOwn(kCallbacksSlot);
  return slot != nullptr && slot->IsObject() ? slot->object : nullptr;
}

}  // namespace

void AnimationFrames::Install(js::Interpreter& interpreter, std::int64_t now_ms) {
  js::Object* global = interpreter.Global();
  const Value callbacks = interpreter.NewObjectValue();
  if (!callbacks.IsObject()) {
    return;
  }
  origin_ms_ = now_ms;
  next_frame_ms_ = now_ms;
  global->Set(kCallbacksSlot, callbacks);
  global->Set(kQueueSlot, PointerValue(this));

  const auto define = [&interpreter, global](const char* name, auto body) {
    const Value native = interpreter.NewNativeValue(name, body);
    if (native.IsObject()) {
      global->Set(name, native);
      interpreter.GlobalScope()->Declare(name, native, false);
    }
  };

  define("requestAnimationFrame", [](NativeCall& call) {
    const Value handler = Argument(call.arguments, 0);
    if (!handler.IsObject() || !handler.object->IsCallable()) {
      return call.Throw("TypeError", "an animation frame callback must be a function");
    }
    js::Object* host = call.interpreter.Global();
    const Value* queue_slot = host->GetOwn(kQueueSlot);
    js::Object* callbacks_object = Callbacks(call.interpreter);
    if (queue_slot == nullptr || callbacks_object == nullptr) {
      return Value::Number(0);
    }
    auto* frames =
        reinterpret_cast<AnimationFrames*>(static_cast<std::uintptr_t>(queue_slot->number));
    const double id = frames->next_id_++;
    frames->pending_.push_back(PendingFrame{id, TrustedScriptContextActive(call.interpreter)});
    callbacks_object->Set(js::NumberToString(id), handler);
    return Value::Number(id);
  });

  define("cancelAnimationFrame", [](NativeCall& call) {
    js::Object* host = call.interpreter.Global();
    const Value* queue_slot = host->GetOwn(kQueueSlot);
    js::Object* callbacks_object = Callbacks(call.interpreter);
    if (queue_slot == nullptr || callbacks_object == nullptr) {
      return Value::Undefined();
    }
    auto* frames =
        reinterpret_cast<AnimationFrames*>(static_cast<std::uintptr_t>(queue_slot->number));
    const double id = js::ToNumber(Argument(call.arguments, 0));
    frames->pending_.erase(
        std::remove_if(frames->pending_.begin(), frames->pending_.end(),
                       [id](const PendingFrame& frame) { return frame.id == id; }),
        frames->pending_.end());
    // The callback goes too, or cancelling would leak its closure for as long
    // as the page lives.
    callbacks_object->Delete(js::NumberToString(id));
    return Value::Undefined();
  });
}

std::optional<std::uint32_t> AnimationFrames::NextDelay(std::int64_t now_ms) const {
  if (pending_.empty()) {
    // The line ADR 0011 asked for: nothing has asked for a frame, so no frame
    // is scheduled and the loop may block indefinitely.
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(std::max<std::int64_t>(0, next_frame_ms_ - now_ms));
}

bool AnimationFrames::RunDue(js::Interpreter& interpreter, std::int64_t now_ms) {
  if (pending_.empty() || now_ms < next_frame_ms_) {
    return false;
  }
  js::Object* callbacks = Callbacks(interpreter);
  if (callbacks == nullptr) {
    return false;
  }

  // The frame is claimed before anything runs. A callback that asks for another
  // frame is asking for the *next* one, and taking the list first is what makes
  // that true rather than an infinite loop inside one turn -- the same rule the
  // timers follow for a zero delay.
  const std::vector<PendingFrame> due = std::exchange(pending_, {});
  next_frame_ms_ = now_ms + kFrameIntervalMs;

  // One timestamp for the whole frame. Two callbacks handed two different times
  // is how two animations that should agree drift apart. Relative to the page's
  // own origin, because a high-resolution wall clock is a fingerprinting
  // surface and a cross-process timing oracle.
  const Value timestamp = Value::Number(static_cast<double>(now_ms - origin_ms_));

  for (const PendingFrame& frame : due) {
    const std::string key = js::NumberToString(frame.id);
    const Value* handler = callbacks->GetOwn(key);
    if (handler == nullptr || !handler->IsObject()) {
      continue;  // cancelled by an earlier callback in this same frame
    }
    const Value callback = *handler;
    callbacks->Delete(key);
    // TD-0018: youtube's lazy list drains via `_.Ot` → rAF → setTimeout →
    // `tryRenderChunk_` → another rAF. Without a fresh hang-guard budget those
    // frames inherit kevlar's spent `kMaxSteps` and abort before `fillRange_`
    // stamps the remaining `shownItems` (same reason timers/idle/MessageChannel
    // call BeginTask).
    interpreter.BeginTask();
    TrustedScriptInvocation trust(interpreter, frame.trust_scripts);
    (void)interpreter.CallFunction(callback, Value::Undefined(), {timestamp});
  }
  // A frame is a turn of its own, so anything its callbacks queued settles
  // before the frame is over -- the same rule a script, an event and a timer
  // get.
  interpreter.DrainMicrotasks();
  return true;
}

}  // namespace microbrowser::bindings

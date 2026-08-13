// A dedicated worker's global scope.
//
// ADR 0022 §1. `Workers.cpp` is the thread; this is what a script running on it can see.
//
// **Why this is the highest-value surface in the browser that was missing.** 1,763 of the
// web-platform-tests suite's 42,185 files are a `.any.worker.html` variant -- the *same* assertions as
// the `.any.html` file beside them, in a global that did not exist here -- and every one of them was a
// twenty-second timeout rather than a result. They do not time out because the assertions fail: they
// time out because `importScripts("/resources/testharness.js")` threw on line one and nothing was ever
// reported. A worker global with no `importScripts` is not a partial feature, it is an unreachable one.
//
// The surface is deliberately *small* and deliberately *not* the window's. `document`, `Element`,
// `getComputedStyle` and every path to them are absent, which ADR 0022 requires and which this file
// makes structural: it is built from `src/js` and `WorkerScopeHost`, so there is nothing here to reach
// the DOM with.

#include "engine/WorkerScope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "bindings/Fingerprint.h"
#include "js/Interpreter.h"
#include "util/PerformanceCounters.h"
#include "util/UserAgent.h"

namespace microbrowser::engine {

namespace {

using js::NativeCall;
using js::Value;

// The steady clock, in milliseconds. Steady rather than the system clock, because a timer that fired
// early or late because the machine's clock was corrected is a bug nobody would find.
double NowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration<double, std::milli>(now).count();
}

// Where the timer callbacks live: a table hung off the global object, so the collector walks it.
// A `js::Value` held only by a C++ container is invisible to the mark phase.
constexpr const char* kTimerTable = "#worker-timers";
constexpr const char* kListenerTable = "#worker-listeners";

const Value& Argument(const std::vector<Value>& arguments, std::size_t index) {
  static const Value undefined = Value::Undefined();
  return index < arguments.size() ? arguments[index] : undefined;
}

// HTML's clamp on a timer's delay: a negative or non-finite delay is zero, which is what every engine
// does and what a page that computes a delay from a subtraction relies on.
double ClampDelay(const Value& value) {
  const double requested = js::ToNumber(value);
  if (!std::isfinite(requested) || requested < 0.0) {
    return 0.0;
  }
  return requested;
}

}  // namespace

WorkerScope::WorkerScope(js::Interpreter& interpreter, WorkerScopeHost& host, std::string name,
                         WorkerLocation location)
    : interpreter_(interpreter),
      host_(host),
      name_(std::move(name)),
      location_(std::move(location)) {}

void WorkerScope::Define(const char* name, const js::Value& value) {
  interpreter_.Global()->Set(name, value);
  // Both, and both are load-bearing. testharness.js calls `addEventListener(...)` unqualified at the
  // bottom of the file and `self.addEventListener(...)` in its worker environment class; a name that is
  // only a property of the global is not in scope for the first, and one that is only a binding is not
  // reachable through `self` for the second.
  interpreter_.GlobalScope()->Declare(name, value, false);
}

void WorkerScope::Install() {
  js::Object* global = interpreter_.Global();
  if (global == nullptr) {
    return;
  }
  const Value self = Value::Obj(global);
  Define("self", self);
  // `globalThis` is already the global in every engine; saying it here costs nothing and a worker
  // script that uses it rather than `self` is common.
  Define("globalThis", self);
  global->Set("name", Value::String(name_));

  InstallGlobalScopeTypes();
  InstallEvents();
  InstallTimers();
  InstallLocation();
  InstallNavigator();
  InstallImportScripts();

  const Value post_message =
      interpreter_.NewNativeValue("postMessage", [this](NativeCall& call) -> Value {
        const std::optional<js::SerializedValue> serialized =
            js::StructuredSerialize(call.interpreter, Argument(call.arguments, 0));
        if (!serialized.has_value()) {
          // The specification's `DataCloneError`. Throwing rather than dropping: a message that
          // silently vanished because it held a function debugs the receiver.
          return call.Throw("Error", "DataCloneError: the message could not be cloned");
        }
        host_.PostToPage(*serialized);
        return Value::Undefined();
      });
  Define("postMessage", post_message);

  const Value close = interpreter_.NewNativeValue("close", [this](NativeCall&) -> Value {
    host_.RequestClose();
    return Value::Undefined();
  });
  Define("close", close);
}

// --- The type hierarchy -------------------------------------------------------------------------
//
// testharness.js decides what environment it is in with
// `global_scope instanceof DedicatedWorkerGlobalScope`, and falls back to `WorkerGlobalScope`. Both
// have to be constructors reachable by name whose `prototype` is on the global's own prototype chain.
// Without them the harness concludes it is in a *shell*, which has no way to report anything back --
// which is one of the two reasons every worker test in the suite reported nothing.
void WorkerScope::InstallGlobalScopeTypes() {
  js::Object* global = interpreter_.Global();
  const Value worker_proto = interpreter_.NewObjectValue();
  const Value dedicated_proto = interpreter_.NewObjectValue();
  if (!worker_proto.IsObject() || !dedicated_proto.IsObject() || global == nullptr) {
    return;
  }
  // Object.prototype stays at the end of the chain: the global is still an ordinary object and
  // `self.hasOwnProperty` must keep working.
  worker_proto.object->SetPrototype(global->Prototype());
  dedicated_proto.object->SetPrototype(worker_proto.object);
  global->SetPrototype(dedicated_proto.object);

  const auto interface_type = [this](const char* name, const Value& prototype) {
    const Value constructor = interpreter_.NewNativeValue(name, [name](NativeCall& call) -> Value {
      // Illegal constructor, which is what every interface object without a constructor is. A worker
      // script that calls it has a bug; one that only uses it for `instanceof` never reaches here.
      return call.Throw("TypeError", std::string("Illegal constructor: ") + name);
    });
    if (constructor.IsObject()) {
      constructor.object->Set("prototype", prototype);
      prototype.object->Set("constructor", constructor);
      Define(name, constructor);
    }
  };
  interface_type("WorkerGlobalScope", worker_proto);
  interface_type("DedicatedWorkerGlobalScope", dedicated_proto);
}

// --- Events -------------------------------------------------------------------------------------

js::Value WorkerScope::ListenersFor(const std::string& type, bool create) {
  js::Object* global = interpreter_.Global();
  if (global == nullptr) {
    return Value::Undefined();
  }
  Value table = Value::Undefined();
  if (const Value* existing = global->GetOwn(kListenerTable)) {
    table = *existing;
  }
  if (!table.IsObject()) {
    if (!create) {
      return Value::Undefined();
    }
    table = interpreter_.NewObjectValue();
    if (!table.IsObject()) {
      return Value::Undefined();
    }
    global->SetHidden(kListenerTable, table);
  }
  Value list = Value::Undefined();
  if (const Value* existing = table.object->GetOwn(type)) {
    list = *existing;
  }
  if (!list.IsObject()) {
    if (!create) {
      return Value::Undefined();
    }
    list = interpreter_.NewArrayValue({});
    if (!list.IsObject()) {
      return Value::Undefined();
    }
    table.object->Set(type, list);
  }
  return list;
}

js::Value WorkerScope::MakeEvent(const char* type) {
  const Value event = interpreter_.NewObjectValue();
  if (!event.IsObject()) {
    return event;
  }
  const Value self = Value::Obj(interpreter_.Global());
  event.object->Set("type", Value::String(type));
  event.object->Set("target", self);
  event.object->Set("currentTarget", self);
  event.object->Set("srcElement", self);
  event.object->Set("eventPhase", Value::Number(2.0));  // AT_TARGET
  event.object->Set("bubbles", Value::Bool(false));
  event.object->Set("cancelable", Value::Bool(false));
  event.object->Set("composed", Value::Bool(false));
  event.object->Set("isTrusted", Value::Bool(true));
  event.object->Set("defaultPrevented", Value::Bool(false));
  event.object->Set("timeStamp", Value::Number(interpreter_.NowMilliseconds()));
  const Value prevent = interpreter_.NewNativeValue("preventDefault", [](NativeCall& call) -> Value {
    if (call.self.IsObject()) {
      const Value* cancelable = call.self.object->Get("cancelable");
      if (cancelable != nullptr && js::ToBoolean(*cancelable)) {
        call.self.object->Set("defaultPrevented", Value::Bool(true));
      }
    }
    return Value::Undefined();
  });
  if (prevent.IsObject()) {
    event.object->Set("preventDefault", prevent);
  }
  for (const char* name : {"stopPropagation", "stopImmediatePropagation"}) {
    // There is no tree here, so propagation is one target and stopping it stops nothing. Present
    // rather than absent because a listener written for a document calls it unconditionally, and a
    // `TypeError` there would lose every assertion after it.
    const Value stop = interpreter_.NewNativeValue(name, [](NativeCall&) -> Value {
      return Value::Undefined();
    });
    if (stop.IsObject()) {
      event.object->Set(name, stop);
    }
  }
  return event;
}

void WorkerScope::InstallEvents() {
  const Value add = interpreter_.NewNativeValue(
      "addEventListener", [this](NativeCall& call) -> Value {
        const std::string type = js::ToString(Argument(call.arguments, 0));
        const Value listener = Argument(call.arguments, 1);
        if (!listener.IsObject()) {
          return Value::Undefined();
        }
        const Value list = ListenersFor(type, true);
        if (!list.IsObject()) {
          return Value::Undefined();
        }
        // The DOM's "already in the set" rule: the same callback registered twice for one type is one
        // listener. A harness that adds its handler on every call would otherwise fire it N times.
        for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
          const Value existing = list.object->GetElement(i);
          if (existing.IsObject() && existing.object == listener.object) {
            return Value::Undefined();
          }
        }
        list.object->PushElement(listener);
        return Value::Undefined();
      });
  Define("addEventListener", add);

  const Value remove = interpreter_.NewNativeValue(
      "removeEventListener", [this](NativeCall& call) -> Value {
        const std::string type = js::ToString(Argument(call.arguments, 0));
        const Value listener = Argument(call.arguments, 1);
        const Value list = ListenersFor(type, false);
        if (!list.IsObject() || !listener.IsObject()) {
          return Value::Undefined();
        }
        std::vector<Value> kept;
        for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
          const Value existing = list.object->GetElement(i);
          if (!existing.IsObject() || existing.object != listener.object) {
            kept.push_back(existing);
          }
        }
        list.object->SetElements(kept, std::vector<bool>(kept.size(), true));
        return Value::Undefined();
      });
  Define("removeEventListener", remove);

  const Value dispatch =
      interpreter_.NewNativeValue("dispatchEvent", [this](NativeCall& call) -> Value {
        const Value event = Argument(call.arguments, 0);
        if (!event.IsObject()) {
          return call.Throw("TypeError", "dispatchEvent needs an event");
        }
        Dispatch(event);
        const Value* prevented = event.object->Get("defaultPrevented");
        return Value::Bool(prevented == nullptr || !js::ToBoolean(*prevented));
      });
  Define("dispatchEvent", dispatch);

  // `new Event(type, init)` and `new MessageEvent(type, init)`. Small, and present because a worker
  // test that constructs one and dispatches it is ordinary -- and because `ErrorEvent` is what an
  // uncaught error arrives as.
  const auto event_type = [this](const char* name) {
    const Value constructor =
        interpreter_.NewNativeValue(name, [this, name](NativeCall& call) -> Value {
          const Value event = MakeEvent(name);
          if (!event.IsObject()) {
            return event;
          }
          event.object->Set("type", Value::String(js::ToString(Argument(call.arguments, 0))));
          event.object->Set("isTrusted", Value::Bool(false));
          const Value init = Argument(call.arguments, 1);
          if (init.IsObject()) {
            for (const std::string& key : init.object->EnumerableKeys()) {
              if (const Value* found = init.object->Get(key)) {
                event.object->Set(key, *found);
              }
            }
          }
          return event;
        });
    if (constructor.IsObject()) {
      Define(name, constructor);
    }
  };
  event_type("Event");
  event_type("MessageEvent");
  event_type("ErrorEvent");
}

void WorkerScope::Dispatch(const js::Value& event) {
  if (!event.IsObject()) {
    return;
  }
  const Value* type_value = event.object->Get("type");
  const std::string type = type_value == nullptr ? std::string() : js::ToString(*type_value);
  const Value self = Value::Obj(interpreter_.Global());
  event.object->Set("currentTarget", self);
  // The `on<type>` handler first, then the registered listeners: the same order the page's
  // `RunListenersOn` uses, and the same reason -- a property assigned before any listener was added is
  // where the specification would have registered it.
  if (const Value* handler = interpreter_.Global()->Get("on" + type);
      handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
    const js::Result outcome = interpreter_.CallFunction(*handler, self, {event});
    if (outcome.completion == js::Completion::Throw) {
      host_.ReportError(js::ToString(outcome.value));
    }
  }
  const Value list = ListenersFor(type, false);
  if (!list.IsObject()) {
    return;
  }
  // A copy of the list, because a listener may add or remove one while it runs -- and the DOM says the
  // set is snapshotted before the first is called.
  std::vector<Value> listeners;
  for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
    listeners.push_back(list.object->GetElement(i));
  }
  for (const Value& listener : listeners) {
    if (!listener.IsObject()) {
      continue;
    }
    Value callee = listener;
    if (!listener.object->IsCallable()) {
      // An `EventListener` object rather than a function: `{handleEvent(e) {...}}`. Rare, and one
      // line, and a test that used one would otherwise fail for a reason that looks like the feature
      // under test.
      const Value* handle = listener.object->Get("handleEvent");
      if (handle == nullptr || !handle->IsObject() || !handle->object->IsCallable()) {
        continue;
      }
      callee = *handle;
    }
    const js::Result outcome = interpreter_.CallFunction(callee, listener, {event});
    if (outcome.completion == js::Completion::Throw) {
      // Reported, not propagated: one listener that throws must not silence the ones after it.
      host_.ReportError(js::ToString(outcome.value));
    }
  }
}

// --- Timers -------------------------------------------------------------------------------------
//
// A worker's timers are its own: they run on its thread, between messages, and the loop sleeps on a
// condition variable until the earliest one is due. Zero idle CPU is preserved exactly as it is on the
// main thread -- a worker with nothing pending blocks, and one with a timer blocks until the deadline.

void WorkerScope::InstallTimers() {
  const auto schedule = [this](NativeCall& call, bool repeating) -> Value {
    const Value callback = Argument(call.arguments, 0);
    if (!callback.IsObject() || !callback.object->IsCallable()) {
      // A string body would be `eval`, which this browser does not have (ADR 0039). Returning an id
      // that never fires is what every engine does for a non-callable first argument.
      return Value::Number(0.0);
    }
    const double delay = ClampDelay(Argument(call.arguments, 1));
    js::Object* global = interpreter_.Global();
    if (global == nullptr) {
      return Value::Number(0.0);
    }
    Value table = Value::Undefined();
    if (const Value* existing = global->GetOwn(kTimerTable)) {
      table = *existing;
    }
    if (!table.IsObject()) {
      table = interpreter_.NewObjectValue();
      if (!table.IsObject()) {
        return Value::Number(0.0);
      }
      global->SetHidden(kTimerTable, table);
    }
    const std::uint64_t id = ++next_timer_id_;
    // The callback and the trailing arguments `setTimeout(f, 0, a, b)` passes, as one array, in the
    // heap. Nothing about a timer is held in a C++ container except its deadline.
    std::vector<Value> entry{callback};
    for (std::size_t i = 2; i < call.arguments.size(); ++i) {
      entry.push_back(call.arguments[i]);
    }
    const Value stored = interpreter_.NewArrayValue(entry);
    if (!stored.IsObject()) {
      return Value::Number(0.0);
    }
    table.object->Set(std::to_string(id), stored);
    Timer timer;
    timer.id = id;
    timer.due_ms = NowMs() + delay;
    timer.interval_ms = delay;
    timer.repeating = repeating;
    timers_.push_back(timer);
    return Value::Number(static_cast<double>(id));
  };

  const Value set_timeout = interpreter_.NewNativeValue(
      "setTimeout", [schedule](NativeCall& call) -> Value { return schedule(call, false); });
  Define("setTimeout", set_timeout);
  const Value set_interval = interpreter_.NewNativeValue(
      "setInterval", [schedule](NativeCall& call) -> Value { return schedule(call, true); });
  Define("setInterval", set_interval);

  const Value clear = interpreter_.NewNativeValue("clearTimeout", [this](NativeCall& call) -> Value {
    const double id = js::ToNumber(Argument(call.arguments, 0));
    if (!std::isfinite(id) || id <= 0.0) {
      return Value::Undefined();
    }
    const auto numeric = static_cast<std::uint64_t>(id);
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [numeric](const Timer& timer) { return timer.id == numeric; }),
                  timers_.end());
    if (js::Object* global = interpreter_.Global()) {
      if (const Value* table = global->GetOwn(kTimerTable); table != nullptr && table->IsObject()) {
        table->object->Delete(std::to_string(numeric));
      }
    }
    return Value::Undefined();
  });
  Define("clearTimeout", clear);
  Define("clearInterval", clear);

  const Value queue_microtask =
      interpreter_.NewNativeValue("queueMicrotask", [](NativeCall& call) -> Value {
        const Value callback = Argument(call.arguments, 0);
        if (!callback.IsObject() || !callback.object->IsCallable()) {
          return call.Throw("TypeError", "queueMicrotask needs a function");
        }
        js::Interpreter::Microtask task;
        task.callee = callback;
        call.interpreter.EnqueueMicrotask(std::move(task));
        return Value::Undefined();
      });
  Define("queueMicrotask", queue_microtask);
}

double WorkerScope::NextDelayMs() const {
  if (timers_.empty()) {
    return -1.0;
  }
  const double now = NowMs();
  double soonest = timers_.front().due_ms;
  for (const Timer& timer : timers_) {
    soonest = std::min(soonest, timer.due_ms);
  }
  return std::max(0.0, soonest - now);
}

void WorkerScope::RunDueTimers() {
  if (timers_.empty()) {
    return;
  }
  const double now = NowMs();
  // Snapshot of what is due, in deadline order, because a callback may add or clear timers. Ordering
  // by deadline rather than by insertion is the specification's: two timers armed together with
  // different delays fire in delay order.
  std::vector<Timer> due;
  for (const Timer& timer : timers_) {
    if (timer.due_ms <= now) {
      due.push_back(timer);
    }
  }
  std::stable_sort(due.begin(), due.end(),
                   [](const Timer& a, const Timer& b) { return a.due_ms < b.due_ms; });
  for (const Timer& timer : due) {
    // Still live? A callback earlier in this batch may have cleared it, and firing a cleared timer is
    // exactly the bug `clearTimeout` exists to prevent.
    const auto found = std::find_if(timers_.begin(), timers_.end(), [&timer](const Timer& live) {
      return live.id == timer.id;
    });
    if (found == timers_.end()) {
      continue;
    }
    js::Object* global = interpreter_.Global();
    const Value* table = global == nullptr ? nullptr : global->GetOwn(kTimerTable);
    if (table == nullptr || !table->IsObject()) {
      return;
    }
    const Value* entry = table->object->GetOwn(std::to_string(timer.id));
    if (entry == nullptr || !entry->IsObject()) {
      timers_.erase(found);
      continue;
    }
    std::vector<Value> arguments;
    for (std::size_t i = 1; i < entry->object->ElementCount(); ++i) {
      arguments.push_back(entry->object->GetElement(i));
    }
    const Value callback = entry->object->GetElement(0);
    if (timer.repeating) {
      // Re-armed *before* the callback runs, so that a callback which clears its own interval wins.
      found->due_ms = NowMs() + std::max(1.0, timer.interval_ms);
    } else {
      timers_.erase(found);
      table->object->Delete(std::to_string(timer.id));
    }
    const js::Result outcome =
        interpreter_.CallFunction(callback, Value::Obj(interpreter_.Global()), arguments);
    if (outcome.completion == js::Completion::Throw) {
      ReportUncaught(outcome.value, location_.href);
    }
    util::AddPerformanceCounter(util::PerfCounterId::WorkerTimersFired);
  }
}

// --- importScripts ------------------------------------------------------------------------------

void WorkerScope::InstallImportScripts() {
  const Value import_scripts =
      interpreter_.NewNativeValue("importScripts", [this](NativeCall& call) -> Value {
        // Every argument is fetched and run **in order**, and a failure stops the rest. The
        // specification fetches them in parallel and runs them in order; running them in order is the
        // observable half, and one at a time is what a blocking host call can offer.
        for (const Value& argument : call.arguments) {
          const std::string specifier = js::ToString(argument);
          std::string body;
          if (!host_.FetchSync(specifier, &body)) {
            // The specification's `NetworkError`. It has to throw: a harness that imported
            // testharness.js and got nothing would run its whole file against a missing `test()` and
            // report a `ReferenceError` instead of the reason.
            return call.Throw("Error",
                              "NetworkError: importScripts could not load " + specifier);
          }
          const js::Result outcome = interpreter_.Run(body);
          if (outcome.completion == js::Completion::Throw) {
            return call.ThrowValue(outcome.value);
          }
        }
        return Value::Undefined();
      });
  Define("importScripts", import_scripts);
}

// --- location and navigator ---------------------------------------------------------------------

void WorkerScope::InstallLocation() {
  const Value location = interpreter_.NewObjectValue();
  if (!location.IsObject()) {
    return;
  }
  const auto set = [&location](const char* name, const std::string& text) {
    location.object->Set(name, Value::String(text));
  };
  set("href", location_.href);
  set("origin", location_.origin);
  set("protocol", location_.protocol);
  set("host", location_.host);
  set("hostname", location_.hostname);
  set("port", location_.port);
  set("pathname", location_.pathname);
  set("search", location_.search);
  set("hash", location_.hash);
  const std::string href = location_.href;
  const Value to_string = interpreter_.NewNativeValue(
      "toString", [href](NativeCall&) -> Value { return Value::String(href); });
  if (to_string.IsObject()) {
    location.object->Set("toString", to_string);
  }
  Define("location", location);
}

void WorkerScope::InstallNavigator() {
  const Value navigator = interpreter_.NewObjectValue();
  if (!navigator.IsObject()) {
    return;
  }
  // The same constants the window's navigator answers with -- `bindings::Fingerprint.h` and
  // `util::kUserAgent`, not copies. ADR 0029 §1's rule is that every copy of this browser gives one
  // answer; a worker that gave a *different* one would be a second fingerprinting surface, and a
  // page can read both.
  navigator.object->Set("userAgent", Value::String(std::string(util::kUserAgent)));
  navigator.object->Set("appVersion", Value::String(std::string(util::kUserAgent)));
  navigator.object->Set("appName", Value::String("Netscape"));
  navigator.object->Set("appCodeName", Value::String("Mozilla"));
  navigator.object->Set("product", Value::String("Gecko"));
  navigator.object->Set("platform", Value::String(std::string(bindings::kPlatform)));
  navigator.object->Set("language", Value::String(std::string(bindings::kLanguage)));
  navigator.object->Set("hardwareConcurrency",
                        Value::Number(static_cast<double>(bindings::kHardwareConcurrency)));
  navigator.object->Set("onLine", Value::Bool(true));
  const Value languages = interpreter_.NewArrayValue({Value::String(std::string(bindings::kLanguage))});
  if (languages.IsObject()) {
    navigator.object->Set("languages", languages);
  }
  Define("navigator", navigator);
}

// --- Running, dispatching, and the end of a turn --------------------------------------------------

void WorkerScope::ReportUncaught(const js::Value& error, const std::string& url) {
  const Value event = MakeEvent("error");
  std::string message = js::ToString(error);
  if (error.IsObject()) {
    if (const Value* text = error.object->Get("message"); text != nullptr) {
      message = js::ToString(*text);
    }
  }
  if (event.IsObject()) {
    event.object->Set("message", Value::String(message));
    event.object->Set("filename", Value::String(url));
    event.object->Set("lineno", Value::Number(0.0));
    event.object->Set("colno", Value::Number(0.0));
    event.object->Set("error", error);
    event.object->Set("cancelable", Value::Bool(true));
    Dispatch(event);
    const Value* prevented = event.object->Get("defaultPrevented");
    if (prevented != nullptr && js::ToBoolean(*prevented)) {
      return;
    }
  }
  // Nothing in the worker handled it, so the page does -- as an `error` event on its `Worker` object,
  // which is where the specification puts it and the only place a page could see it from.
  host_.ReportError(message);
}

void WorkerScope::RunScript(const std::string& source, const std::string& url) {
  const js::Result outcome = interpreter_.Run(source);
  if (outcome.completion == js::Completion::Throw) {
    ReportUncaught(outcome.value, url);
  }
}

void WorkerScope::DeliverMessage(const js::SerializedValue& message) {
  const Value event = MakeEvent("message");
  if (!event.IsObject()) {
    return;
  }
  // Deserialised here, in this heap. The bytes were opaque the whole way across, which is ADR 0022's
  // "messages cross by value" and the reason there is no lock on either side of this.
  event.object->Set("data", js::StructuredDeserialize(interpreter_, message));
  event.object->Set("origin", Value::String(location_.origin));
  event.object->Set("lastEventId", Value::String(""));
  event.object->Set("source", Value::Null());
  const Value ports = interpreter_.NewArrayValue({});
  if (ports.IsObject()) {
    event.object->Set("ports", ports);
  }
  Dispatch(event);
}

void WorkerScope::EndTurn() {
  // The microtask queue, drained at the end of the task rather than left: a worker whose promise
  // callbacks ran only when the *next* message arrived would look like it had stalled.
  interpreter_.DrainMicrotasks();
  const std::vector<std::string>& console = interpreter_.ConsoleOutput();
  for (std::size_t i = console_cursor_; i < console.size(); ++i) {
    host_.ReportConsole(console[i]);
  }
  console_cursor_ = console.size();
}

}  // namespace microbrowser::engine

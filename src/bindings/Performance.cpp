#include "bindings/Performance.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/Fingerprint.h"
#include "bindings/PerformanceEntries.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using performance_entries::MakeEntry;
using performance_entries::Record;
using util::AddPerformanceCounter;
using util::PerfCounterId;

// Every entry this document has produced, in order, and every observer watching.
// On the global, which is a root the collector already walks.
constexpr const char* kEntriesSlot = "#performance:entries";
constexpr const char* kObserversSlot = "#performance:observers";
// Set on an observer while it has records queued, so the frame step can skip a
// page whose observers are all idle without walking their queues.
constexpr const char* kQueuedSlot = "#queued";

// The entry types this browser actually delivers.
//
// `longtask` is deliberately absent: there is no task scheduler to observe, and a
// list that claimed it would leave a page waiting on a callback that never comes.
// `paint`, `layout-shift` and `largest-contentful-paint` are absent for the same
// reason -- each needs a measurement this browser does not take.
constexpr std::string_view kSupportedTypes[] = {"mark", "measure", "navigation", "resource"};

bool IsSupportedType(std::string_view type) {
  return std::find(std::begin(kSupportedTypes), std::end(kSupportedTypes), type) !=
         std::end(kSupportedTypes);
}

// How many entries one document keeps. A page can call `mark` in a loop, so the
// list is bounded and the oldest go -- the same argument the history list and the
// violation log make. Past the bound `mark` still succeeds, because a page that
// gets a throw from an instrumentation call is a page that breaks over telemetry.
constexpr std::size_t kMaxEntries = 2048;

Value EntriesArray(js::Interpreter& interpreter) {
  if (const Value* existing = interpreter.Global()->GetOwn(kEntriesSlot)) {
    return *existing;
  }
  const Value list = interpreter.NewArrayValue({});
  if (list.IsObject()) {
    interpreter.Global()->SetHidden(kEntriesSlot, list);
  }
  return list;
}

// Deliberately does *not* create the list. A document that never made an
// observer must not allocate one to be told there is nothing to deliver.
const Value* ExistingObservers(js::Interpreter& interpreter) {
  const Value* observers = interpreter.Global()->GetOwn(kObserversSlot);
  return observers != nullptr && observers->IsObject() ? observers : nullptr;
}

Value ObserversArray(js::Interpreter& interpreter) {
  if (const Value* existing = ExistingObservers(interpreter)) {
    return *existing;
  }
  const Value list = interpreter.NewArrayValue({});
  if (list.IsObject()) {
    interpreter.Global()->SetHidden(kObserversSlot, list);
  }
  return list;
}


std::string StringField(const Value& object, const char* name) {
  if (!object.IsObject()) {
    return {};
  }
  const Value* value = object.object->Get(name);
  return value == nullptr ? std::string() : js::ToString(*value);
}

}  // namespace

// The two the other half of the module needs, declared in PerformanceEntries.h
// and defined here because this is where the entry list and the observers live.
namespace performance_entries {

Value MakeEntry(js::Interpreter& interpreter, std::string_view type, std::string_view name,
                double start, double duration) {
  const Value entry = interpreter.NewObjectValue();
  if (!entry.IsObject()) {
    return entry;
  }
  entry.object->Set("entryType", Value::String(std::string(type)));
  entry.object->Set("name", Value::String(std::string(name)));
  entry.object->Set("startTime", Value::Number(start));
  entry.object->Set("duration", Value::Number(duration));
  return entry;
}

void Record(js::Interpreter& interpreter, const Value& entry) {
  if (!entry.IsObject()) {
    return;
  }
  const Value entries = EntriesArray(interpreter);
  if (entries.IsObject()) {
    if (entries.object->ElementCount() >= kMaxEntries) {
      // Drop the oldest. `SetElements` rather than a shift, because the element
      // storage is the array's own and rebuilding it is the one operation that
      // does not go through a bounds check per element.
      std::vector<Value> kept;
      kept.reserve(kMaxEntries);
      for (std::size_t i = 1; i < entries.object->ElementCount(); ++i) {
        kept.push_back(entries.object->GetElement(i));
      }
      entries.object->SetElements(kept, std::vector<bool>(kept.size(), true));
    }
    entries.object->PushElement(entry);
  }
  AddPerformanceCounter(PerfCounterId::PerformanceEntries);

  const Value* observers = ExistingObservers(interpreter);
  if (observers == nullptr) {
    return;
  }
  const std::string type = StringField(entry, "entryType");
  for (std::size_t i = 0; i < observers->object->ElementCount(); ++i) {
    const Value observer = observers->object->GetElement(i);
    if (!observer.IsObject()) {
      continue;
    }
    const Value* types = observer.object->GetOwn("#types");
    if (types == nullptr || !types->IsObject()) {
      continue;
    }
    bool watching = false;
    for (std::size_t t = 0; t < types->object->ElementCount(); ++t) {
      watching = watching || js::ToString(types->object->GetElement(t)) == type;
    }
    if (!watching) {
      continue;
    }
    const Value* queue = observer.object->GetOwn("#queue");
    if (queue != nullptr && queue->IsObject()) {
      queue->object->PushElement(entry);
      observer.object->SetHidden(kQueuedSlot, Value::Bool(true));
    }
  }
}

}  // namespace performance_entries

namespace {

// The list handed to an observer's callback: `getEntries`, and the two filtered
// forms. Built per delivery rather than kept, because it describes one delivery.
Value MakeEntryList(js::Interpreter& interpreter, std::vector<Value> entries) {
  const Value list = interpreter.NewObjectValue();
  if (!list.IsObject()) {
    return list;
  }
  const Value array = interpreter.NewArrayValue(std::move(entries));
  list.object->SetHidden("#entries", array);

  const auto filtered = [](NativeCall& call, const char* field) {
    const Value* stored = call.self.IsObject() ? call.self.object->GetOwn("#entries") : nullptr;
    if (stored == nullptr || !stored->IsObject()) {
      return call.interpreter.NewArrayValue({});
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    std::vector<Value> out;
    for (std::size_t i = 0; i < stored->object->ElementCount(); ++i) {
      const Value entry = stored->object->GetElement(i);
      if (StringField(entry, field) == wanted) {
        out.push_back(entry);
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  };

  const Value get_entries = interpreter.NewNativeValue("getEntries", [](NativeCall& call) {
    const Value* stored = call.self.IsObject() ? call.self.object->GetOwn("#entries") : nullptr;
    return stored == nullptr ? call.interpreter.NewArrayValue({}) : *stored;
  });
  const Value by_type = interpreter.NewNativeValue(
      "getEntriesByType", [filtered](NativeCall& call) { return filtered(call, "entryType"); });
  const Value by_name = interpreter.NewNativeValue(
      "getEntriesByName", [filtered](NativeCall& call) { return filtered(call, "name"); });
  if (get_entries.IsObject()) {
    list.object->Set("getEntries", get_entries);
  }
  if (by_type.IsObject()) {
    list.object->Set("getEntriesByType", by_type);
  }
  if (by_name.IsObject()) {
    list.object->Set("getEntriesByName", by_name);
  }
  return list;
}

}  // namespace

void Performance::Install(js::Interpreter& interpreter, std::int64_t now_ms) {
  origin_ms_ = now_ms;
  const Value performance = interpreter.NewObjectValue();
  if (!performance.IsObject()) {
    return;
  }

  // `now()`. The page's clock, and the only reason this is a native rather than a
  // stored number: a page reads it repeatedly and expects it to move.
  const Value now = interpreter.NewNativeValue("now", [](NativeCall& call) {
    // The clock the host last told this object about. A native cannot ask the
    // system what time it is -- that is what makes the number a duration since
    // the document started rather than a wall clock, which would be a
    // fingerprinting surface.
    const Value* last = call.interpreter.Global()->GetOwn("#performance:now");
    return Value::Number(last == nullptr ? 0.0 : js::ToNumber(*last));
  });
  if (now.IsObject()) {
    performance.object->Set("now", now);
  }
  performance.object->Set("timeOrigin", Value::Number(0.0));
  // `Performance` is an EventTarget. Without the prototype,
  // `performance.addEventListener` is undefined and hr-time/basic.any.html's
  // last subtest is a TypeError rather than a dispatch.
  if (const Value* event_target = interpreter.GlobalScope()->Lookup("EventTarget");
      event_target != nullptr && event_target->IsObject()) {
    if (const Value* proto = event_target->object->Get("prototype");
        proto != nullptr && proto->IsObject()) {
      performance.object->SetPrototype(proto->object);
    }
  }

  const Value mark = interpreter.NewNativeValue("mark", [](NativeCall& call) {
    const std::string name = js::ToString(Argument(call.arguments, 0));
    // `{startTime}` in the options, which reddit's page-load timer uses to stamp
    // a mark at a moment that has already passed.
    double start = 0.0;
    const Value* last = call.interpreter.Global()->GetOwn("#performance:now");
    start = last == nullptr ? 0.0 : js::ToNumber(*last);
    const Value options = Argument(call.arguments, 1);
    if (options.IsObject()) {
      if (const Value* given = options.object->Get("startTime")) {
        start = js::ToNumber(*given);
      }
    }
    const Value entry = MakeEntry(call.interpreter, "mark", name, start, 0.0);
    Record(call.interpreter, entry);
    return entry;
  });
  if (mark.IsObject()) {
    performance.object->Set("mark", mark);
  }

  const Value measure = interpreter.NewNativeValue("measure", [](NativeCall& call) {
    const std::string name = js::ToString(Argument(call.arguments, 0));
    // Two shapes: `measure(name, startMark, endMark)` and
    // `measure(name, {start, end})`. Both are in the wild and reddit uses the
    // second, so both resolve through one helper rather than two.
    double start = 0.0;
    double end = 0.0;
    const Value* last = call.interpreter.Global()->GetOwn("#performance:now");
    end = last == nullptr ? 0.0 : js::ToNumber(*last);

    const auto mark_time = [&call](const std::string& mark_name) -> double {
      const Value* entries = call.interpreter.Global()->GetOwn(kEntriesSlot);
      if (entries == nullptr || !entries->IsObject()) {
        return 0.0;
      }
      // The *last* mark with that name, which is what the specification says and
      // what a page that marks the same name once per route expects.
      double found = 0.0;
      for (std::size_t i = 0; i < entries->object->ElementCount(); ++i) {
        const Value entry = entries->object->GetElement(i);
        if (StringField(entry, "name") == mark_name &&
            StringField(entry, "entryType") == "mark") {
          const Value* start_time = entry.object->Get("startTime");
          found = start_time == nullptr ? 0.0 : js::ToNumber(*start_time);
        }
      }
      return found;
    };

    const Value second = Argument(call.arguments, 1);
    if (second.IsObject()) {
      if (const Value* given = second.object->Get("start")) {
        start = js::ToNumber(*given);
      }
      if (const Value* given = second.object->Get("end")) {
        end = js::ToNumber(*given);
      }
    } else if (!second.IsUndefined()) {
      start = mark_time(js::ToString(second));
      const Value third = Argument(call.arguments, 2);
      if (!third.IsUndefined()) {
        end = mark_time(js::ToString(third));
      }
    }
    const Value entry =
        MakeEntry(call.interpreter, "measure", name, start, std::max(0.0, end - start));
    Record(call.interpreter, entry);
    return entry;
  });
  if (measure.IsObject()) {
    performance.object->Set("measure", measure);
  }

  const auto query = [](NativeCall& call, const char* field) {
    const Value* entries = call.interpreter.Global()->GetOwn(kEntriesSlot);
    if (entries == nullptr || !entries->IsObject()) {
      return call.interpreter.NewArrayValue({});
    }
    if (field == nullptr) {
      return *entries;
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    std::vector<Value> out;
    for (std::size_t i = 0; i < entries->object->ElementCount(); ++i) {
      const Value entry = entries->object->GetElement(i);
      if (StringField(entry, field) == wanted) {
        out.push_back(entry);
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  };
  const Value get_entries = interpreter.NewNativeValue(
      "getEntries", [query](NativeCall& call) { return query(call, nullptr); });
  const Value by_type = interpreter.NewNativeValue(
      "getEntriesByType", [query](NativeCall& call) { return query(call, "entryType"); });
  const Value by_name = interpreter.NewNativeValue(
      "getEntriesByName", [query](NativeCall& call) { return query(call, "name"); });
  if (get_entries.IsObject()) {
    performance.object->Set("getEntries", get_entries);
  }
  if (by_type.IsObject()) {
    performance.object->Set("getEntriesByType", by_type);
  }
  if (by_name.IsObject()) {
    performance.object->Set("getEntriesByName", by_name);
  }

  // `clearMarks` and `clearMeasures`, because a page that instruments a route
  // change clears between routes and would otherwise grow the list until the
  // bound above starts dropping the entries it is about to read.
  const auto clear = [](NativeCall& call, const char* type) {
    const Value* entries = call.interpreter.Global()->GetOwn(kEntriesSlot);
    if (entries == nullptr || !entries->IsObject()) {
      return Value::Undefined();
    }
    const Value named = Argument(call.arguments, 0);
    const std::string wanted = named.IsUndefined() ? std::string() : js::ToString(named);
    std::vector<Value> kept;
    for (std::size_t i = 0; i < entries->object->ElementCount(); ++i) {
      const Value entry = entries->object->GetElement(i);
      const bool matches_type = StringField(entry, "entryType") == type;
      const bool matches_name = wanted.empty() || StringField(entry, "name") == wanted;
      if (!(matches_type && matches_name)) {
        kept.push_back(entry);
      }
    }
    entries->object->SetElements(kept, std::vector<bool>(kept.size(), true));
    return Value::Undefined();
  };
  const Value clear_marks = interpreter.NewNativeValue(
      "clearMarks", [clear](NativeCall& call) { return clear(call, "mark"); });
  const Value clear_measures = interpreter.NewNativeValue(
      "clearMeasures", [clear](NativeCall& call) { return clear(call, "measure"); });
  if (clear_marks.IsObject()) {
    performance.object->Set("clearMarks", clear_marks);
  }
  if (clear_measures.IsObject()) {
    performance.object->Set("clearMeasures", clear_measures);
  }

  // `timing`, from whatever the engine has already told this object. Built here
  // rather than lazily on first read because the read that matters is
  // youtube.com's very first inline script, and an accessor that could allocate
  // would be one more thing between a page and its own clock. The other half of
  // the module: see PerformanceTiming.cpp.
  InstallTiming(interpreter, performance);

  const Value to_json = interpreter.NewNativeValue("toJSON", [](NativeCall& call) {
    const Value json = call.interpreter.NewObjectValue();
    if (!json.IsObject() || !call.self.IsObject()) {
      return json;
    }
    json.object->Set("timeOrigin",
                     call.interpreter.GetPropertyValue(call.self, "timeOrigin"));
    json.object->Set("timing", call.interpreter.GetPropertyValue(call.self, "timing"));
    json.object->Set("navigation",
                     call.interpreter.GetPropertyValue(call.self, "navigation"));
    return json;
  });
  if (to_json.IsObject()) {
    performance.object->Set("toJSON", to_json);
  }

  interpreter.Global()->Set("performance", performance);
  interpreter.GlobalScope()->Declare("performance", performance, false);
  interpreter.Global()->SetHidden("#performance:now", Value::Number(kTimerResolutionMs));

  // --- PerformanceObserver --------------------------------------------------

  const Value prototype = interpreter.NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  const Value observe = interpreter.NewNativeValue("observe", [](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->GetOwn("#types") == nullptr) {
      return call.Throw("TypeError", "observe called on something that is not an observer");
    }
    const Value options = Argument(call.arguments, 0);
    std::vector<std::string> wanted;
    bool buffered = false;
    if (options.IsObject()) {
      if (const Value* type = options.object->Get("type")) {
        if (!type->IsUndefined()) {
          wanted.push_back(js::ToString(*type));
        }
      }
      if (const Value* list = options.object->Get("entryTypes")) {
        if (list->IsObject()) {
          for (std::size_t i = 0; i < list->object->ElementCount(); ++i) {
            wanted.push_back(js::ToString(list->object->GetElement(i)));
          }
        }
      }
      if (const Value* flag = options.object->Get("buffered")) {
        buffered = js::ToBoolean(*flag);
      }
    }
    // An unsupported type registers nothing and does **not** throw, which is what
    // the specification says and what makes `supportedEntryTypes` the honest way
    // to find out: a page that observes `longtask` here gets no callback and no
    // error, exactly as it does in Safari.
    const Value* types = call.self.object->GetOwn("#types");
    for (const std::string& type : wanted) {
      if (!IsSupportedType(type)) {
        continue;
      }
      types->object->PushElement(Value::String(type));
    }
    // Registered after the types, so an observer with none watches nothing.
    const Value observers = ObserversArray(call.interpreter);
    if (observers.IsObject()) {
      bool already = false;
      for (std::size_t i = 0; i < observers.object->ElementCount(); ++i) {
        const Value entry = observers.object->GetElement(i);
        already = already || (entry.IsObject() && entry.object == call.self.object);
      }
      if (!already) {
        observers.object->PushElement(call.self);
      }
    }
    if (!buffered) {
      return Value::Undefined();
    }
    // `buffered: true` means the entries that already happened. reddit observes
    // `navigation` this way, and without it the entry it wants was recorded
    // before its own script ran.
    const Value* entries = call.interpreter.Global()->GetOwn(kEntriesSlot);
    const Value* queue = call.self.object->GetOwn("#queue");
    if (entries == nullptr || !entries->IsObject() || queue == nullptr || !queue->IsObject()) {
      return Value::Undefined();
    }
    for (std::size_t i = 0; i < entries->object->ElementCount(); ++i) {
      const Value entry = entries->object->GetElement(i);
      const std::string type = StringField(entry, "entryType");
      if (std::find(wanted.begin(), wanted.end(), type) != wanted.end()) {
        queue->object->PushElement(entry);
        call.self.object->SetHidden(kQueuedSlot, Value::Bool(true));
      }
    }
    return Value::Undefined();
  });
  if (observe.IsObject()) {
    prototype.object->Set("observe", observe);
  }

  const Value disconnect = interpreter.NewNativeValue("disconnect", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    const Value* observers = ExistingObservers(call.interpreter);
    if (observers == nullptr) {
      return Value::Undefined();
    }
    std::vector<Value> kept;
    for (std::size_t i = 0; i < observers->object->ElementCount(); ++i) {
      const Value entry = observers->object->GetElement(i);
      if (!entry.IsObject() || entry.object != call.self.object) {
        kept.push_back(entry);
      }
    }
    observers->object->SetElements(kept, std::vector<bool>(kept.size(), true));
    call.self.object->SetHidden(kQueuedSlot, Value::Bool(false));
    return Value::Undefined();
  });
  if (disconnect.IsObject()) {
    prototype.object->Set("disconnect", disconnect);
  }

  const Value take_records = interpreter.NewNativeValue("takeRecords", [](NativeCall& call) {
    const Value* queue = call.self.IsObject() ? call.self.object->GetOwn("#queue") : nullptr;
    if (queue == nullptr || !queue->IsObject()) {
      return call.interpreter.NewArrayValue({});
    }
    std::vector<Value> taken;
    for (std::size_t i = 0; i < queue->object->ElementCount(); ++i) {
      taken.push_back(queue->object->GetElement(i));
    }
    queue->object->SetElements({}, {});
    call.self.object->SetHidden(kQueuedSlot, Value::Bool(false));
    return call.interpreter.NewArrayValue(std::move(taken));
  });
  if (take_records.IsObject()) {
    prototype.object->Set("takeRecords", take_records);
  }

  const Value constructor =
      interpreter.NewNativeValue("PerformanceObserver", [prototype](NativeCall& call) {
        const Value callback = Argument(call.arguments, 0);
        if (!callback.IsObject() || !callback.object->IsCallable()) {
          return call.Throw("TypeError", "PerformanceObserver requires a callback");
        }
        const Value observer = call.interpreter.NewObjectValue();
        if (!observer.IsObject()) {
          return Value::Undefined();
        }
        observer.object->SetPrototype(prototype.object);
        observer.object->SetHidden("#callback", callback);
        observer.object->SetHidden("#types", call.interpreter.NewArrayValue({}));
        observer.object->SetHidden("#queue", call.interpreter.NewArrayValue({}));
        observer.object->SetHidden(kQueuedSlot, Value::Bool(false));
        return observer;
      });
  if (!constructor.IsObject()) {
    return;
  }
  // The list a page reads to find out what it can ask for. Frozen, because a page
  // that could write to it would be writing to every other page's copy of the
  // truth about this browser.
  std::vector<Value> supported;
  for (const std::string_view type : kSupportedTypes) {
    supported.push_back(Value::String(std::string(type)));
  }
  const Value supported_types = interpreter.NewArrayValue(std::move(supported));
  if (supported_types.IsObject()) {
    supported_types.object->Freeze();
    constructor.object->Set("supportedEntryTypes", supported_types);
  }
  // `prototype` on the constructor as well as on the instances, or
  // `observer instanceof PerformanceObserver` is false -- which a page checks,
  // and which reddit's own wrapper checks before calling `disconnect`.
  constructor.object->Set("prototype", prototype);
  prototype.object->SetHidden("constructor", constructor);
  interpreter.Global()->Set("PerformanceObserver", constructor);
  interpreter.GlobalScope()->Declare("PerformanceObserver", constructor, false);

  // Everything the engine produced before there was a heap to put it in. Flushed
  // after the observers exist so that a `buffered: true` observe finds it, and
  // before any script runs so that nothing can see the list half-filled.
  for (const PendingEntry& entry : pending_) {
    if (entry.type == "navigation") {
      SetNavigationTiming(&interpreter, entry.dom_content_loaded, entry.load_event,
                          entry.end - entry.start);
    } else {
      AddResourceTiming(&interpreter, entry.name, entry.initiator, entry.start, entry.end,
                        entry.encoded_size, entry.decoded_size);
    }
  }
  pending_.clear();
}

void Performance::Tick(js::Interpreter& interpreter, std::int64_t now_ms) {
  // Floor at the coarsened quantum so the first script of a document whose
  // origin is this turn's clock still answers a positive number. WPT's
  // `now() > 0` is that, and 0 is what integer milliseconds at the origin
  // otherwise produce.
  const double elapsed = std::max(Now(now_ms), kTimerResolutionMs);
  interpreter.Global()->SetHidden("#performance:now", Value::Number(elapsed));
}


bool Performance::DeliverObservations(js::Interpreter& interpreter) {
  const Value* observers = ExistingObservers(interpreter);
  if (observers == nullptr) {
    // A document with no observer, which is almost every document. One pointer
    // comparison per frame.
    return false;
  }
  // Collected before any callback runs: a callback can create an observer, and a
  // walk over a list one of them is appending to is a walk that never ends.
  std::vector<Value> due;
  for (std::size_t i = 0; i < observers->object->ElementCount(); ++i) {
    const Value observer = observers->object->GetElement(i);
    const Value* queued = observer.IsObject() ? observer.object->GetOwn(kQueuedSlot) : nullptr;
    if (queued != nullptr && js::ToBoolean(*queued)) {
      due.push_back(observer);
    }
  }
  if (due.empty()) {
    return false;
  }
  bool ran = false;
  for (const Value& observer : due) {
    const Value* queue = observer.object->GetOwn("#queue");
    const Value* callback = observer.object->GetOwn("#callback");
    if (queue == nullptr || !queue->IsObject() || callback == nullptr) {
      continue;
    }
    std::vector<Value> records;
    for (std::size_t i = 0; i < queue->object->ElementCount(); ++i) {
      records.push_back(queue->object->GetElement(i));
    }
    // Emptied before the callback runs, so a callback that produces an entry
    // queues it for the *next* delivery rather than re-entering this one.
    queue->object->SetElements({}, {});
    observer.object->SetHidden(kQueuedSlot, Value::Bool(false));
    if (records.empty()) {
      continue;
    }
    const Value list = MakeEntryList(interpreter, std::move(records));
    // TD-0018: reddit's perf bundle observes from a callback after the concat
    // polyfill spends the step budget.
    interpreter.BeginTask();
    interpreter.CallFunction(*callback, observer, {list, observer});
    AddPerformanceCounter(PerfCounterId::PerformanceObserverCallbacks);
    ran = true;
  }
  if (ran) {
    interpreter.DrainMicrotasks();
  }
  return ran;
}

}  // namespace microbrowser::bindings

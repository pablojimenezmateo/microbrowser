#include "bindings/Performance.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/Fingerprint.h"
#include "bindings/PerformanceEntries.h"
#include "bindings/WebIdl.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using performance_entries::CloneUserTimingDetail;
using performance_entries::ConvertMarkOptions;
using performance_entries::ConvertMarkToTimestamp;
using performance_entries::ConvertNamedMark;
using performance_entries::IsPerformanceTimingName;
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
constexpr const char* kPerformanceMarker = "#isPerformance";
constexpr const char* kEntryKindSlot = "#entryKind";
constexpr const char* kDetailSlot = "#detail";

bool IsPerformance(const Value& value) {
  if (!value.IsObject()) {
    return false;
  }
  const Value* marker = value.object->GetOwn(kPerformanceMarker);
  return marker != nullptr && marker->type == js::ValueType::Boolean && marker->boolean;
}

bool RequirePerformanceThis(NativeCall& call) {
  if (IsPerformance(call.self)) {
    return true;
  }
  (void)call.Throw("TypeError", "Illegal invocation");
  return false;
}

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

bool IsWindowGlobal(js::Interpreter& interpreter) {
  const Value* window = interpreter.GlobalScope()->Lookup("Window");
  return window != nullptr;
}

double CurrentNow(js::Interpreter& interpreter) {
  const Value* last = interpreter.Global()->GetOwn("#performance:now");
  return last == nullptr ? 0.0 : js::ToNumber(*last);
}

const Value* DictMember(const Value& dict, const char* name) {
  const Value* given = dict.IsObject() ? dict.object->Get(name) : nullptr;
  return given == nullptr || given->IsUndefined() ? nullptr : given;
}

Value RunMeasure(NativeCall& call) {
  if (!RequirePerformanceThis(call)) {
    return call.ThrownValue();
  }
  if (!RequireArguments(call, "Performance", "measure", 1)) {
    return call.ThrownValue();
  }
  const std::string name = js::ToString(Argument(call.arguments, 0));
  const Value second = Argument(call.arguments, 1);
  const Value third = Argument(call.arguments, 2);
  const bool has_end_mark = !third.IsUndefined();
  double start = 0.0;
  double end = CurrentNow(call.interpreter);
  Value detail = Value::Null();

  if (second.IsObject()) {
    const Value* start_m = DictMember(second, "start");
    const Value* end_m = DictMember(second, "end");
    const Value* duration_m = DictMember(second, "duration");
    const Value* detail_m = DictMember(second, "detail");
    if (start_m != nullptr || end_m != nullptr || duration_m != nullptr || detail_m != nullptr) {
      if (has_end_mark || (start_m == nullptr && end_m == nullptr) ||
          (start_m != nullptr && duration_m != nullptr && end_m != nullptr)) {
        return call.Throw("TypeError", "invalid PerformanceMeasureOptions");
      }
    }
    if (has_end_mark) {
      if (!ConvertNamedMark(call, third, end)) {
        return call.ThrownValue();
      }
    } else if (end_m != nullptr) {
      if (!ConvertMarkToTimestamp(call, *end_m, end)) {
        return call.ThrownValue();
      }
    } else if (start_m != nullptr && duration_m != nullptr) {
      if (!ConvertMarkToTimestamp(call, *start_m, start) ||
          !ConvertMarkToTimestamp(call, Value::Number(js::ToNumber(*duration_m)), end)) {
        return call.ThrownValue();
      }
      end = start + end;
    }
    if (start_m != nullptr) {
      if (!ConvertMarkToTimestamp(call, *start_m, start)) {
        return call.ThrownValue();
      }
    } else if (duration_m != nullptr && end_m != nullptr) {
      double duration = 0.0;
      if (!ConvertMarkToTimestamp(call, Value::Number(js::ToNumber(*duration_m)), duration)) {
        return call.ThrownValue();
      }
      start = end - duration;
    }
    if (detail_m != nullptr && !CloneUserTimingDetail(call, *detail_m, detail)) {
      return call.ThrownValue();
    }
  } else if (second.IsUndefined() || second.IsNull()) {
    if (has_end_mark && !ConvertNamedMark(call, third, end)) {
      return call.ThrownValue();
    }
  } else {
    if (!ConvertNamedMark(call, second, start)) {
      return call.ThrownValue();
    }
    if (has_end_mark && !ConvertNamedMark(call, third, end)) {
      return call.ThrownValue();
    }
  }

  const Value entry = MakeEntry(call.interpreter, "measure", name, start, end - start, detail);
  Record(call.interpreter, entry);
  return entry;
}

Value InstallIllegalInterface(js::Interpreter& interpreter, const char* name,
                              const Value& parent_proto, const Value& parent_ctor) {
  const Value prototype = interpreter.NewObjectValue();
  if (!prototype.IsObject()) {
    return prototype;
  }
  if (parent_proto.IsObject()) {
    prototype.object->SetPrototype(parent_proto.object);
  }
  const std::string message = std::string("Illegal constructor: ") + name;
  const Value constructor = interpreter.NewNativeValue(
      name, [message](NativeCall& call) { return call.Throw("TypeError", message); });
  if (!constructor.IsObject()) {
    return prototype;
  }
  if (parent_ctor.IsObject()) {
    constructor.object->SetPrototype(parent_ctor.object);
  }
  SetFunctionLength(constructor, 0);
  DefinePrototypeSlot(constructor.object, prototype);
  DefineNonEnumerable(prototype.object, "constructor", constructor);
  interpreter.GlobalScope()->Declare(name, constructor, false);
  DefineNonEnumerable(interpreter.Global(), name, constructor);
  if (js::Object* tag = interpreter.SymbolToStringTag()) {
    prototype.object->SetHidden(js::PropertyKey::Symbol(tag), Value::String(name));
  }
  return prototype;
}

void InstallDetailGetter(js::Interpreter& interpreter, const Value& prototype,
                         const char* kind) {
  const Value getter = interpreter.NewNativeValue(
      "get detail", [kind](NativeCall& call) -> Value {
        if (!call.self.IsObject()) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        const Value* entry_kind = call.self.object->GetOwn(kEntryKindSlot);
        if (entry_kind == nullptr || js::ToString(*entry_kind) != kind) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        const Value* detail = call.self.object->GetOwn(kDetailSlot);
        return detail == nullptr ? Value::Null() : *detail;
      });
  if (getter.IsObject() && prototype.IsObject()) {
    SetFunctionLength(getter, 0);
    prototype.object->DefineAccessor("detail", getter.object, nullptr);
  }
}

void InstallEntryInterfaces(js::Interpreter& interpreter) {
  const Value entry_proto =
      InstallIllegalInterface(interpreter, "PerformanceEntry", Value::Undefined(), Value::Undefined());
  if (entry_proto.IsObject()) {
    interpreter.Global()->SetHidden("#proto:PerformanceEntry", entry_proto);
    const Value to_json = interpreter.NewNativeValue("toJSON", [](NativeCall& call) -> Value {
      if (!call.self.IsObject() || call.self.object->GetOwn(kEntryKindSlot) == nullptr) {
        return call.Throw("TypeError", "Illegal invocation");
      }
      const Value json = call.interpreter.NewObjectValue();
      if (!json.IsObject()) {
        return json;
      }
      json.object->Set("name", call.interpreter.GetPropertyValue(call.self, "name"));
      json.object->Set("entryType", call.interpreter.GetPropertyValue(call.self, "entryType"));
      json.object->Set("startTime", call.interpreter.GetPropertyValue(call.self, "startTime"));
      json.object->Set("duration", call.interpreter.GetPropertyValue(call.self, "duration"));
      if (const Value* detail = call.self.object->GetOwn(kDetailSlot)) {
        json.object->Set("detail", *detail);
      }
      return json;
    });
    if (to_json.IsObject()) {
      SetFunctionLength(to_json, 0);
      entry_proto.object->Set("toJSON", to_json);
    }
  }

  const Value mark_proto = interpreter.NewObjectValue();
  if (mark_proto.IsObject()) {
    if (entry_proto.IsObject()) {
      mark_proto.object->SetPrototype(entry_proto.object);
    }
    const Value constructor = interpreter.NewNativeValue(
        "PerformanceMark", [](NativeCall& call) -> Value {
          if (!call.interpreter.IsConstructCall(call.self)) {
            return call.Throw("TypeError", "PerformanceMark constructor requires 'new'");
          }
          if (!RequireArguments(call, "PerformanceMark", "constructor", 1)) {
            return call.ThrownValue();
          }
          const std::string name = js::ToString(Argument(call.arguments, 0));
          if (IsWindowGlobal(call.interpreter) && IsPerformanceTimingName(name)) {
            return ThrowDom(call, "SyntaxError",
                            "Failed to construct 'PerformanceMark': '" + name +
                                "' is a reserved PerformanceTiming name");
          }
          double start = CurrentNow(call.interpreter);
          Value detail = Value::Null();
          if (!ConvertMarkOptions(call, Argument(call.arguments, 1), start, detail)) {
            return call.ThrownValue();
          }
          return MakeEntry(call.interpreter, "mark", name, start, 0.0, detail);
        });
    if (constructor.IsObject()) {
      SetFunctionLength(constructor, 1);
      if (const Value* entry_ctor = entry_proto.IsObject() ? entry_proto.object->GetOwn("constructor")
                                                           : nullptr;
          entry_ctor != nullptr && entry_ctor->IsObject()) {
        constructor.object->SetPrototype(entry_ctor->object);
      }
      DefinePrototypeSlot(constructor.object, mark_proto);
      DefineNonEnumerable(mark_proto.object, "constructor", constructor);
      interpreter.GlobalScope()->Declare("PerformanceMark", constructor, false);
      DefineNonEnumerable(interpreter.Global(), "PerformanceMark", constructor);
    }
    if (js::Object* tag = interpreter.SymbolToStringTag()) {
      mark_proto.object->SetHidden(js::PropertyKey::Symbol(tag), Value::String("PerformanceMark"));
    }
    InstallDetailGetter(interpreter, mark_proto, "mark");
    interpreter.Global()->SetHidden("#proto:PerformanceMark", mark_proto);
  }

  const Value entry_ctor = entry_proto.IsObject()
                               ? (entry_proto.object->GetOwn("constructor") != nullptr
                                      ? *entry_proto.object->GetOwn("constructor")
                                      : Value::Undefined())
                               : Value::Undefined();
  const Value measure_proto =
      InstallIllegalInterface(interpreter, "PerformanceMeasure", entry_proto, entry_ctor);
  if (measure_proto.IsObject()) {
    InstallDetailGetter(interpreter, measure_proto, "measure");
    interpreter.Global()->SetHidden("#proto:PerformanceMeasure", measure_proto);
  }
}

}  // namespace

// The two the other half of the module needs, declared in PerformanceEntries.h
// and defined here because this is where the entry list and the observers live.
namespace performance_entries {

Value MakeEntry(js::Interpreter& interpreter, std::string_view type, std::string_view name,
                double start, double duration, const Value& detail) {
  const Value entry = interpreter.NewObjectValue();
  if (!entry.IsObject()) {
    return entry;
  }
  const char* proto_key = "#proto:PerformanceEntry";
  if (type == "mark") {
    proto_key = "#proto:PerformanceMark";
  } else if (type == "measure") {
    proto_key = "#proto:PerformanceMeasure";
  }
  if (const Value* proto = interpreter.Global()->GetOwn(proto_key);
      proto != nullptr && proto->IsObject()) {
    entry.object->SetPrototype(proto->object);
  }
  entry.object->Set("entryType", Value::String(std::string(type)));
  entry.object->Set("name", Value::String(std::string(name)));
  entry.object->Set("startTime", Value::Number(start));
  entry.object->Set("duration", Value::Number(duration));
  entry.object->SetHidden(kEntryKindSlot, Value::String(std::string(type)));
  entry.object->SetHidden(kDetailSlot, detail.IsUndefined() ? Value::Null() : detail);
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
  const Value interface_prototype = interpreter.NewObjectValue();
  const Value performance = interpreter.NewObjectValue();
  if (!interface_prototype.IsObject() || !performance.IsObject()) {
    return;
  }

  // `Performance` is an EventTarget. The chain is instance → Performance.prototype
  // → EventTarget.prototype, which is what makes `performance instanceof Performance`
  // and `performance.addEventListener` both true. Putting methods on the instance
  // skipped the interface object, and every hr-time idlharness check failed.
  if (const Value* event_target = interpreter.GlobalScope()->Lookup("EventTarget");
      event_target != nullptr && event_target->IsObject()) {
    if (const Value* proto = event_target->object->Get("prototype");
        proto != nullptr && proto->IsObject()) {
      interface_prototype.object->SetPrototype(proto->object);
    }
  }
  performance.object->SetPrototype(interface_prototype.object);
  performance.object->SetHidden(kPerformanceMarker, Value::Bool(true));

  const Value interface_constructor = interpreter.NewNativeValue(
      "Performance", [](NativeCall& call) -> Value {
        return call.Throw("TypeError", "Illegal constructor: Performance");
      });
  if (interface_constructor.IsObject()) {
    SetFunctionLength(interface_constructor, 0);
    DefinePrototypeSlot(interface_constructor.object, interface_prototype);
    DefineNonEnumerable(interface_prototype.object, "constructor", interface_constructor);
    interpreter.GlobalScope()->Declare("Performance", interface_constructor, false);
    DefineNonEnumerable(interpreter.Global(), "Performance", interface_constructor);
  }
  if (js::Object* tag = interpreter.SymbolToStringTag()) {
    interface_prototype.object->SetHidden(js::PropertyKey::Symbol(tag),
                                          Value::String("Performance"));
  }

  // `now()`. The page's clock, and the only reason this is a native rather than a
  // stored number: a page reads it repeatedly and expects it to move.
  const Value now = interpreter.NewNativeValue("now", [](NativeCall& call) {
    if (!RequirePerformanceThis(call)) {
      return call.ThrownValue();
    }
    // The clock the host last told this object about. A native cannot ask the
    // system what time it is -- that is what makes the number a duration since
    // the document started rather than a wall clock, which would be a
    // fingerprinting surface.
    const Value* last = call.interpreter.Global()->GetOwn("#performance:now");
    return Value::Number(last == nullptr ? 0.0 : js::ToNumber(*last));
  });
  if (now.IsObject()) {
    SetFunctionLength(now, 0);
    interface_prototype.object->Set("now", now);
  }
  const Value time_origin = interpreter.NewNativeValue("get timeOrigin", [](NativeCall& call) {
    if (!RequirePerformanceThis(call)) {
      return call.ThrownValue();
    }
    return Value::Number(0.0);
  });
  if (time_origin.IsObject()) {
    SetFunctionLength(time_origin, 0);
    interface_prototype.object->DefineAccessor("timeOrigin", time_origin.object, nullptr);
  }

  const Value mark = interpreter.NewNativeValue("mark", [](NativeCall& call) {
    if (!RequirePerformanceThis(call)) {
      return call.ThrownValue();
    }
    if (!RequireArguments(call, "Performance", "mark", 1)) {
      return call.ThrownValue();
    }
    const std::string name = js::ToString(Argument(call.arguments, 0));
    if (IsWindowGlobal(call.interpreter) && IsPerformanceTimingName(name)) {
      return ThrowDom(call, "SyntaxError",
                      "Failed to execute 'mark' on 'Performance': '" + name +
                          "' is a reserved PerformanceTiming name");
    }
    // `{startTime}` in the options, which reddit's page-load timer uses to stamp
    // a mark at a moment that has already passed.
    double start = CurrentNow(call.interpreter);
    Value detail = Value::Null();
    if (!ConvertMarkOptions(call, Argument(call.arguments, 1), start, detail)) {
      return call.ThrownValue();
    }
    const Value entry = MakeEntry(call.interpreter, "mark", name, start, 0.0, detail);
    Record(call.interpreter, entry);
    return entry;
  });
  if (mark.IsObject()) {
    SetFunctionLength(mark, 1);
    interface_prototype.object->Set("mark", mark);
  }

  const Value measure = interpreter.NewNativeValue("measure", RunMeasure);
  if (measure.IsObject()) {
    SetFunctionLength(measure, 1);
    interface_prototype.object->Set("measure", measure);
  }

  const auto query = [](NativeCall& call, const char* field) {
    if (!RequirePerformanceThis(call)) {
      return call.ThrownValue();
    }
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
    SetFunctionLength(get_entries, 0);
    interface_prototype.object->Set("getEntries", get_entries);
  }
  if (by_type.IsObject()) {
    SetFunctionLength(by_type, 1);
    interface_prototype.object->Set("getEntriesByType", by_type);
  }
  if (by_name.IsObject()) {
    SetFunctionLength(by_name, 1);
    interface_prototype.object->Set("getEntriesByName", by_name);
  }

  // `clearMarks` and `clearMeasures`, because a page that instruments a route
  // change clears between routes and would otherwise grow the list until the
  // bound above starts dropping the entries it is about to read.
  const auto clear = [](NativeCall& call, const char* type) {
    if (!RequirePerformanceThis(call)) {
      return call.ThrownValue();
    }
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
    SetFunctionLength(clear_marks, 0);
    interface_prototype.object->Set("clearMarks", clear_marks);
  }
  if (clear_measures.IsObject()) {
    SetFunctionLength(clear_measures, 0);
    interface_prototype.object->Set("clearMeasures", clear_measures);
  }

  // `timing`, from whatever the engine has already told this object. Built here
  // rather than lazily on first read because the read that matters is
  // youtube.com's very first inline script, and an accessor that could allocate
  // would be one more thing between a page and its own clock. The other half of
  // the module: see PerformanceTiming.cpp.
  InstallTiming(interpreter, performance);

  const Value to_json = interpreter.NewNativeValue("toJSON", [](NativeCall& call) {
    if (!RequirePerformanceThis(call)) {
      return call.ThrownValue();
    }
    const Value json = call.interpreter.NewObjectValue();
    if (!json.IsObject()) {
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
    SetFunctionLength(to_json, 0);
    interface_prototype.object->Set("toJSON", to_json);
  }

  DefineNonEnumerable(interpreter.Global(), "performance", performance);
  interpreter.GlobalScope()->Declare("performance", performance, false);
  interpreter.Global()->SetHidden("#performance:now", Value::Number(kTimerResolutionMs));
  InstallEntryInterfaces(interpreter);

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

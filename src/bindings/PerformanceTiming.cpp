#include "bindings/Performance.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "bindings/BindingSupport.h"
#include "bindings/PerformanceEntries.h"
#include "js/StructuredClone.h"

// The half of `performance` the *engine* drives: the `navigation` and `resource`
// entries, and the legacy `performance.timing`.
//
// Split from Performance.cpp because they are two kinds of thing that happen at
// two times -- see PerformanceEntries.h for the seam and why it is only two
// functions wide. The short version: everything here is produced by the loader,
// most of it before there is a heap to put it in, and none of it can be asked
// for from script.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using performance_entries::MakeEntry;
using performance_entries::Record;

// The legacy `PerformanceTiming` object. Kept on the global rather than reached
// through `performance`, because a page is free to overwrite `window.performance`
// and the host still has to be able to fill in `loadEventEnd` afterwards.
constexpr const char* kTimingSlot = "#performance:timing";

// The same bound on what is held before there is a heap. A document names its
// subresources, so the list is written from attacker-controlled markup before
// anything can read it.
constexpr std::size_t kMaxPendingEntries = 512;

// The legacy `performance.timing`, created once per document.
//
// Every field is a Unix timestamp in milliseconds, and the whole interface is a
// flat set of them -- which is why this is a plain object with no prototype of
// its own: there is no method on one, and a page reads it by name or spreads it
// into a telemetry payload.
//
// **This browser measures three moments of a navigation**: when it started, when
// the document's bytes were complete, and when each of DOMContentLoaded and load
// fired. It does not separately time the DNS lookup, the connection, the TLS
// handshake or the first byte. Every unmeasured boundary therefore collapses onto
// the enclosing measured one -- `domainLookupStart == domainLookupEnd ==
// connectStart == connectEnd == requestStart == fetchStart` -- which is exactly
// the shape a real browser reports for a phase that did not happen (a resource
// out of the cache reports the same collapse). What it is *not* is an invented
// spread of plausible-looking durations: a page that subtracts two of these gets
// zero, which is a phase it can see was not measured, rather than a number this
// browser made up.
//
// The fields for an event that has not fired yet are zero, which is what a
// browser reports for one -- and is why they are written again as each lands
// rather than all at once.
Value TimingObject(js::Interpreter& interpreter) {
  if (const Value* existing = interpreter.Global()->GetOwn(kTimingSlot)) {
    return *existing;
  }
  const Value timing = interpreter.NewObjectValue();
  if (!timing.IsObject()) {
    return timing;
  }
  // Every field the interface has, so that a page reading one this browser does
  // not measure gets zero rather than `undefined` -- the difference matters,
  // because `undefined` in an arithmetic expression is NaN and NaN spreads
  // silently through a page's own metrics.
  for (const char* field :
       {"navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd",
        "fetchStart", "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd",
        "secureConnectionStart", "requestStart", "responseStart", "responseEnd", "domLoading",
        "domInteractive", "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete",
        "loadEventStart", "loadEventEnd"}) {
    timing.object->Set(field, Value::Number(0.0));
  }
  const Value to_json = interpreter.NewNativeValue("toJSON", [](NativeCall& call) {
    const Value json = call.interpreter.NewObjectValue();
    if (!json.IsObject() || !call.self.IsObject()) {
      return json;
    }
    for (const std::string& key : call.self.object->Keys()) {
      const Value* value = call.self.object->GetOwn(key);
      if (value == nullptr || (value->IsObject() && value->object->IsCallable())) {
        continue;
      }
      json.object->Set(key, *value);
    }
    return json;
  });
  if (to_json.IsObject()) {
    timing.object->Set("toJSON", to_json);
  }
  interpreter.Global()->SetHidden(kTimingSlot, timing);
  return timing;
}

}  // namespace

void Performance::InstallTiming(js::Interpreter& interpreter, const Value& performance) const {
  const Value timing = TimingObject(interpreter);
  if (!timing.IsObject() || !performance.IsObject()) {
    return;
  }
  performance.object->Set("timing", timing);
  // Legacy `PerformanceNavigation`. `type` 0 is TYPE_NAVIGATE; this browser has
  // no reload/back_forward distinct from a new document, so the other codes
  // are not produced. `toJSON` is what performance-tojson.html asks for.
  const Value navigation = interpreter.NewObjectValue();
  if (navigation.IsObject()) {
    navigation.object->Set("type", Value::Number(0.0));
    navigation.object->Set("redirectCount", Value::Number(0.0));
    const Value to_json = interpreter.NewNativeValue("toJSON", [](NativeCall& call) {
      const Value json = call.interpreter.NewObjectValue();
      if (!json.IsObject() || !call.self.IsObject()) {
        return json;
      }
      json.object->Set("type", call.interpreter.GetPropertyValue(call.self, "type"));
      json.object->Set("redirectCount",
                       call.interpreter.GetPropertyValue(call.self, "redirectCount"));
      return json;
    });
    if (to_json.IsObject()) {
      navigation.object->Set("toJSON", to_json);
    }
    performance.object->Set("navigation", navigation);
  }
  WriteDocumentTiming(interpreter);
}

void Performance::WriteDocumentTiming(js::Interpreter& interpreter) const {
  if (navigation_start_wall_ms_ == 0) {
    // No document has been loaded through this object -- an `about:blank` made
    // to hold a script, or a test. Every field stays zero, which is what a
    // browser reports for a document that never navigated.
    return;
  }
  const Value timing = TimingObject(interpreter);
  if (!timing.IsObject()) {
    return;
  }
  const double start = static_cast<double>(navigation_start_wall_ms_);
  // The unmeasured boundaries, collapsed onto the start of the fetch. See
  // TimingObject: this is a phase that was not timed, reported the way a browser
  // reports a phase that did not occur, rather than a duration this invented.
  for (const char* field : {"navigationStart", "fetchStart", "domainLookupStart",
                            "domainLookupEnd", "connectStart", "connectEnd", "requestStart"}) {
    timing.object->Set(field, Value::Number(start));
  }
  // `secureConnectionStart` stays zero, which the specification defines as "not
  // available" and is the truth here: the handshake is not timed apart from the
  // connection. `unloadEvent*` and `redirect*` stay zero too -- there is no
  // unload event in this browser, and a redirect's timing is cross-origin
  // information a browser withholds anyway.
  const double response = start + response_end_ms_;
  timing.object->Set("responseStart", Value::Number(response));
  timing.object->Set("responseEnd", Value::Number(response));
  // Parsing begins when the bytes are complete: this browser has no incremental
  // parse, which is written down in the roadmap rather than hidden here.
  timing.object->Set("domLoading", Value::Number(response));
}

void Performance::SetDocumentTiming(js::Interpreter* interpreter,
                                    std::int64_t navigation_start_wall_ms,
                                    double response_end_ms) {
  navigation_start_wall_ms_ = navigation_start_wall_ms;
  response_end_ms_ = response_end_ms;
  // Held rather than written when there is no heap yet, which is the normal
  // case: the document arrives before its first script runs, by definition.
  if (interpreter != nullptr) {
    WriteDocumentTiming(*interpreter);
  }
}

void Performance::SetNavigationTiming(js::Interpreter* interpreter, double dom_content_loaded_ms,
                                     double load_event_ms, double duration_ms) {
  if (interpreter == nullptr) {
    if (pending_.size() < kMaxPendingEntries) {
      PendingEntry held;
      held.type = "navigation";
      held.dom_content_loaded = dom_content_loaded_ms;
      held.load_event = load_event_ms;
      held.end = duration_ms;
      pending_.push_back(std::move(held));
    }
    return;
  }
  const Value entry = MakeEntry(*interpreter, "navigation", "", 0.0, duration_ms);
  if (!entry.IsObject()) {
    return;
  }
  // The fields a page actually reads off one. `domContentLoadedEventStart` is the
  // one reddit's own perf module keys on, and it reports the metric only when it
  // is non-zero -- so a navigation entry that answered zero would be an entry
  // that silently does nothing.
  entry.object->Set("domContentLoadedEventStart", Value::Number(dom_content_loaded_ms));
  entry.object->Set("domContentLoadedEventEnd", Value::Number(dom_content_loaded_ms));
  entry.object->Set("loadEventStart", Value::Number(load_event_ms));
  entry.object->Set("loadEventEnd", Value::Number(load_event_ms));
  entry.object->Set("responseEnd", Value::Number(dom_content_loaded_ms));
  entry.object->Set("type", Value::String("navigate"));
  entry.object->Set("initiatorType", Value::String("navigation"));
  Record(*interpreter, entry);

  // And the same two moments on the legacy `performance.timing`, as absolute
  // timestamps. The same numbers in two places because they are two interfaces
  // rather than two views: one measures from the start of the navigation, the
  // other from the epoch, and a page reads whichever it was written against.
  if (navigation_start_wall_ms_ == 0) {
    return;
  }
  const Value timing = TimingObject(*interpreter);
  if (!timing.IsObject()) {
    return;
  }
  const double start = static_cast<double>(navigation_start_wall_ms_);
  // `domInteractive` is when the document's own scripts finished, which is the
  // moment DOMContentLoaded is about to fire -- there is no separate parse-done
  // moment here, because parsing and running are one step.
  for (const char* field :
       {"domInteractive", "domContentLoadedEventStart", "domContentLoadedEventEnd"}) {
    timing.object->Set(field, Value::Number(start + dom_content_loaded_ms));
  }
  for (const char* field : {"domComplete", "loadEventStart", "loadEventEnd"}) {
    timing.object->Set(field, Value::Number(start + load_event_ms));
  }
}

void Performance::AddResourceTiming(js::Interpreter* interpreter, std::string_view name,
                                    std::string_view initiator, double start_ms,
                                    double response_end_ms, std::size_t encoded_size,
                                    std::size_t decoded_size) {
  if (interpreter == nullptr) {
    if (pending_.size() < kMaxPendingEntries) {
      PendingEntry held;
      held.type = "resource";
      held.name = std::string(name);
      held.initiator = std::string(initiator);
      held.start = start_ms;
      held.end = response_end_ms;
      held.encoded_size = encoded_size;
      held.decoded_size = decoded_size;
      pending_.push_back(std::move(held));
    }
    return;
  }
  const Value entry = MakeEntry(*interpreter, "resource", name, start_ms,
                                std::max(0.0, response_end_ms - start_ms));
  if (!entry.IsObject()) {
    return;
  }
  entry.object->Set("initiatorType", Value::String(std::string(initiator)));
  entry.object->Set("responseEnd", Value::Number(response_end_ms));
  entry.object->Set("encodedBodySize", Value::Number(static_cast<double>(encoded_size)));
  entry.object->Set("decodedBodySize", Value::Number(static_cast<double>(decoded_size)));
  // `transferSize` is zero for a resource that came out of the cache, which is
  // what a page tests to compute a cache hit rate -- so it is the decoded size
  // when bytes crossed the wire and zero when they did not.
  entry.object->Set("transferSize", Value::Number(static_cast<double>(encoded_size)));
  entry.object->Set("renderBlockingStatus", Value::String("non-blocking"));
  Record(*interpreter, entry);
}

namespace performance_entries {

bool IsPerformanceTimingName(std::string_view name) {
  static constexpr const char* kNames[] = {
      "navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd",
      "fetchStart", "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd",
      "secureConnectionStart", "requestStart", "responseStart", "responseEnd", "domLoading",
      "domInteractive", "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete",
      "loadEventStart", "loadEventEnd"};
  for (const char* field : kNames) {
    if (name == field) {
      return true;
    }
  }
  return false;
}

namespace {

using js::NativeCall;
using js::Value;

const Value* DictMember(const Value& dict, const char* name) {
  const Value* given = dict.IsObject() ? dict.object->Get(name) : nullptr;
  return given == nullptr || given->IsUndefined() ? nullptr : given;
}

std::string StringField(const Value& object, const char* name) {
  if (!object.IsObject()) {
    return {};
  }
  const Value* value = object.object->Get(name);
  return value == nullptr ? std::string() : js::ToString(*value);
}

bool FindMarkStartTime(js::Interpreter& interpreter, std::string_view name, double& out) {
  const Value* entries = interpreter.Global()->GetOwn("#performance:entries");
  if (entries == nullptr || !entries->IsObject()) {
    return false;
  }
  bool found = false;
  for (std::size_t i = 0; i < entries->object->ElementCount(); ++i) {
    const Value entry = entries->object->GetElement(i);
    if (StringField(entry, "name") != name || StringField(entry, "entryType") != "mark") {
      continue;
    }
    const Value* start_time = entry.object->Get("startTime");
    out = start_time == nullptr ? 0.0 : js::ToNumber(*start_time);
    found = true;
  }
  return found;
}

double TimingField(js::Interpreter& interpreter, const std::string& name) {
  const Value* timing = interpreter.Global()->GetOwn("#performance:timing");
  const Value* field =
      timing != nullptr && timing->IsObject() ? timing->object->Get(name) : nullptr;
  return field == nullptr ? 0.0 : js::ToNumber(*field);
}

}  // namespace

bool CloneUserTimingDetail(js::NativeCall& call, const js::Value& value, js::Value& out) {
  if (value.IsNull()) {
    out = js::Value::Null();
    return true;
  }
  const std::optional<js::SerializedValue> serialized =
      js::StructuredSerialize(call.interpreter, value);
  if (!serialized) {
    (void)ThrowDom(call, "DataCloneError", "the value could not be cloned");
    return false;
  }
  out = js::StructuredDeserialize(call.interpreter, *serialized);
  return true;
}

bool ConvertMarkToTimestamp(js::NativeCall& call, const js::Value& mark, double& out) {
  if (mark.IsNumber()) {
    out = mark.number;
    if (out < 0.0) {
      (void)call.Throw("TypeError", "timestamp is negative");
      return false;
    }
    return true;
  }
  const std::string name = js::ToString(mark);
  if (IsPerformanceTimingName(name)) {
    if (call.interpreter.GlobalScope()->Lookup("Window") == nullptr) {
      (void)call.Throw("TypeError", "PerformanceTiming names require a Window");
      return false;
    }
    if (name == "navigationStart") {
      out = 0.0;
      return true;
    }
    const double end = TimingField(call.interpreter, name);
    if (end == 0.0) {
      (void)ThrowDom(call, "InvalidAccessError", "PerformanceTiming." + name + " is 0");
      return false;
    }
    out = end - TimingField(call.interpreter, "navigationStart");
    return true;
  }
  if (!FindMarkStartTime(call.interpreter, name, out)) {
    (void)ThrowDom(call, "SyntaxError", "no such mark: " + name);
    return false;
  }
  return true;
}

bool ConvertNamedMark(js::NativeCall& call, const js::Value& value, double& out) {
  return ConvertMarkToTimestamp(call, js::Value::String(js::ToString(value)), out);
}

bool ConvertMarkOptions(js::NativeCall& call, const js::Value& options, double& start,
                        js::Value& detail) {
  if (options.IsUndefined() || options.IsNull()) {
    return true;
  }
  if (!options.IsObject()) {
    (void)call.Throw("TypeError", "PerformanceMarkOptions must be an object");
    return false;
  }
  if (const js::Value* given = DictMember(options, "startTime")) {
    start = js::ToNumber(*given);
    if (start < 0.0) {
      (void)call.Throw("TypeError", "PerformanceMark startTime is negative");
      return false;
    }
  }
  if (const js::Value* given = DictMember(options, "detail")) {
    return CloneUserTimingDetail(call, *given, detail);
  }
  return true;
}

}  // namespace performance_entries

}  // namespace microbrowser::bindings

#include "bindings/Performance.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "bindings/PerformanceEntries.h"

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

}  // namespace microbrowser::bindings

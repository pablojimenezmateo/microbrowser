#pragma once

#include <string_view>

#include "js/Interpreter.h"

// The two operations both halves of `performance` need, and the reason this
// header exists rather than one file holding everything.
//
// The module has two halves, and they are not the same kind of thing:
//
//   * **What the page measured.** `now`, `mark`, `measure`, the entry list and
//     the queries over it, and `PerformanceObserver` -- all of it driven by the
//     page, from script, at times the page chose. That is Performance.cpp.
//   * **What the engine measured.** The `navigation` and `resource` entries and
//     the legacy `performance.timing`, all of it produced by the loader, most of
//     it *before there is a heap to put it in*. That is PerformanceTiming.cpp.
//
// They meet at exactly two functions: making an entry, and recording one. Those
// are declared here rather than duplicated, because an entry queued on the
// observers by one half and not the other is a `PerformanceObserver` that fires
// for a page's own marks and stays silent for the load -- which is the failure
// mode the whole feature exists to avoid.
//
// Private to the module: deliberately absent from MODULE.deps' `public:` list.

namespace microbrowser::bindings::performance_entries {

// One entry, as the object a page reads. The prototype is PerformanceMark,
// PerformanceMeasure or PerformanceEntry, looked up from the global at creation
// so `instanceof` and `Object.prototype.toString` name the interface. Fields a
// page reads are still data properties; `detail` is the accessor on the
// prototype, because idlharness gets it there.
js::Value MakeEntry(js::Interpreter& interpreter, std::string_view type, std::string_view name,
                    double start, double duration, const js::Value& detail = js::Value::Null());

// Records `entry` on the document's entry list and queues it on every observer
// watching its type. Bounded: the list drops its oldest past a cap, because a
// page can call `mark` in a loop.
void Record(js::Interpreter& interpreter, const js::Value& entry);

// User Timing argument conversion. Lives here because converting a mark name
// to a timestamp reads the same PerformanceTiming fields this half writes.
bool IsPerformanceTimingName(std::string_view name);
bool ConvertMarkOptions(js::NativeCall& call, const js::Value& options, double& start,
                        js::Value& detail);
bool ConvertMarkToTimestamp(js::NativeCall& call, const js::Value& mark, double& out);
bool ConvertNamedMark(js::NativeCall& call, const js::Value& value, double& out);
bool CloneUserTimingDetail(js::NativeCall& call, const js::Value& value, js::Value& out);

}  // namespace microbrowser::bindings::performance_entries

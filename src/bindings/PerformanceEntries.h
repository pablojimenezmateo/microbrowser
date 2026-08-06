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

// One entry, as the object a page reads. A plain object rather than a class with
// a prototype: every field a page touches is a data property, `toJSON` is the
// only method the specification puts on one, and a page that spreads an entry
// into its telemetry payload gets the fields either way.
js::Value MakeEntry(js::Interpreter& interpreter, std::string_view type, std::string_view name,
                    double start, double duration);

// Records `entry` on the document's entry list and queues it on every observer
// watching its type. Bounded: the list drops its oldest past a cap, because a
// page can call `mark` in a loop.
void Record(js::Interpreter& interpreter, const js::Value& entry);

}  // namespace microbrowser::bindings::performance_entries

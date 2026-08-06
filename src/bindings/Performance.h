#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "js/Interpreter.h"

namespace microbrowser::bindings {

// `performance` and `PerformanceObserver`.
//
// Ledger session 50, and it is here because four roadmap sessions in a row had a
// check that needed reddit's own bundle to run and every one of them stopped at
// `ReferenceError: PerformanceObserver is not defined`, in the first `data:`
// module the page evaluates. No ADR sequenced it.
//
// Three decisions, and the first is the one that makes this safe to ship:
//
//   * **`supportedEntryTypes` tells the truth.** It lists exactly the types this
//     browser delivers, and `longtask` is not one of them -- there is no task
//     scheduler to observe. reddit reads this list and only observes `longtask`
//     if it is there, which is the *point* of the list existing: a browser that
//     claimed support and delivered nothing would leave the page waiting on a
//     callback that never comes. Safari does not support `longtask` either, so
//     this is a shape the web already handles.
//   * **The clock is the page's, not the system's.** `performance.now()` is
//     milliseconds since this document started, from the same epoch
//     `AnimationFrames` hands a frame callback -- one clock, because two would
//     let a page's animation and its measurements disagree about when the same
//     moment was. A high-resolution wall clock is a fingerprinting surface and a
//     cross-process timing oracle; this is neither.
//   * **Observers are delivered at the frame**, at the one place a frame is
//     produced, exactly as ADR 0018 §5's IntersectionObserver is -- and for the
//     same reason: an observer a page could make fire from inside its own
//     callback is a re-entrancy a page controls the depth of. A document with no
//     observer costs one pointer comparison per frame.
//
// Time is passed in rather than read from a clock, for the reason the timers take
// one: two decisions inside a turn must not disagree about what time it is, and a
// test must be able to say what time it is.
class Performance {
 public:
  // Declares `performance` and `PerformanceObserver`. `now_ms` is the epoch every
  // number this hands out is measured from.
  void Install(js::Interpreter& interpreter, std::int64_t now_ms);

  // The page's own clock. Shared with AnimationFrames::Timestamp rather than
  // recomputed.
  double Now(std::int64_t now_ms) const { return static_cast<double>(now_ms - origin_ms_); }

  // Publishes the current time to the page, so `performance.now()` answers with
  // it. Called at every boundary the host has a time at -- installing, running
  // the document's scripts, running what is due, and the frame -- because a
  // native cannot ask the system what time it is: that is what keeps the number a
  // duration since this document started rather than a wall clock.
  //
  // The consequence is worth stating rather than hiding: `performance.now()` does
  // not advance *within* a turn. Two reads either side of a loop see the same
  // number, which is what a browser with a coarsened clock does anyway and is the
  // opposite of a timing oracle.
  void Tick(js::Interpreter& interpreter, std::int64_t now_ms);

  // The legacy `performance.timing`, which is a different interface from the
  // `navigation` entry below and not a view of it: every field on it is a Unix
  // timestamp rather than an offset, because a page subtracts one from
  // `Date.now()`.
  //
  // It exists because youtube.com's first inline script is
  // `ytcsi.setStart(w.performance ? w.performance.timing.responseStart : null)`
  // -- unguarded past the `performance` test, because no browser has ever had
  // `performance` without `timing`. It is deprecated, not absent, and a page
  // reads it before anything else it does.
  //
  // Called once per document, before the first script runs, which is why it
  // takes the two moments the engine has by then: when the navigation started
  // and when the document's bytes were complete. `interpreter` may be null,
  // which is the normal case -- see SetNavigationTiming.
  void SetDocumentTiming(js::Interpreter* interpreter, std::int64_t navigation_start_wall_ms,
                         double response_end_ms);

  // The `navigation` entry for this document, from the engine -- which is the
  // only thing that knows when the load started and when its events fired.
  // Added once per document; a second call replaces it, because a document has
  // one navigation.
  // `interpreter` may be null, which is the normal case: the load finishes and
  // its subresources complete before the first script runs, so there is no heap
  // yet. The entry is held as plain data and flushed at Install.
  void SetNavigationTiming(js::Interpreter* interpreter, double dom_content_loaded_ms,
                           double load_event_ms, double duration_ms);

  // A `resource` entry: one subresource, as the loader saw it. `initiator` is the
  // specification's `initiatorType` -- "script", "css", "img" -- and it is what a
  // page groups by.
  void AddResourceTiming(js::Interpreter* interpreter, std::string_view name,
                         std::string_view initiator, double start_ms, double response_end_ms,
                         std::size_t encoded_size, std::size_t decoded_size);

  // Runs the callbacks of every observer with something queued for it, each with
  // the entries queued since the last delivery. True when any ran, which is the
  // caller's signal that the document may have changed under it.
  //
  // False without touching the heap when no observer exists, which is what keeps
  // a page that never made one from paying for the feature.
  bool DeliverObservations(js::Interpreter& interpreter);

 private:
  // Puts `timing` on the `performance` object and fills in what is known.
  // PerformanceTiming.cpp, which is the half of this module the engine drives.
  void InstallTiming(js::Interpreter& interpreter, const js::Value& performance) const;

  // Writes the navigation-start-through-responseEnd half of `performance.timing`
  // from what the engine has said so far. The DOMContentLoaded and load fields
  // are written by SetNavigationTiming, which is the only thing that knows them.
  void WriteDocumentTiming(js::Interpreter& interpreter) const;

  // An entry the engine produced before this page had an interpreter.
  //
  // Every subresource of a document completes *before* its first script runs --
  // that is what "render-blocking" means -- so every `resource` entry exists
  // before there is a heap to put it in. Held as plain data (no `js::Value`, so
  // nothing for the collector to miss) and flushed at Install. Without this the
  // entries a page observes with `buffered: true` are exactly the ones that were
  // dropped, which is a `PerformanceObserver` that appears to work and reports
  // nothing.
  struct PendingEntry {
    std::string type;
    std::string name;
    std::string initiator;
    double start = 0.0;
    double end = 0.0;
    double dom_content_loaded = 0.0;
    double load_event = 0.0;
    std::size_t encoded_size = 0;
    std::size_t decoded_size = 0;
  };

  // The epoch. Everything else that survives -- the entries, the observers, their
  // queued records -- lives in JavaScript objects hung off the global, because
  // the collector cannot see a `js::Value` in a C++ field and a callback it
  // cannot see is one it frees while this still points at it.
  std::int64_t origin_ms_ = 0;
  // What `performance.timing` is built from, held as plain data for the same
  // reason `pending_` is: the engine knows both numbers before there is a heap.
  // Zero navigation start means no document has been loaded through this object
  // -- a `performance.timing` whose fields were all zero is what a browser shows
  // for a document that never navigated, so it is also the honest initial state.
  std::int64_t navigation_start_wall_ms_ = 0;
  double response_end_ms_ = 0.0;
  // Bounded, because a document can name arbitrarily many subresources and this
  // list is written before anything can read it.
  std::vector<PendingEntry> pending_;
};

}  // namespace microbrowser::bindings

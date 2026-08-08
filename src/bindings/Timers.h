#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "js/Interpreter.h"

namespace microbrowser::bindings {

// `setTimeout`, `setInterval`, and the two that cancel them.
//
// The first feature in this browser that legitimately needs a wakeup, and the
// reason it is a queue rather than a poll. The zero-idle-CPU invariant says
// the process sleeps in exactly one place and that work which must happen
// later sets a deadline rather than spinning; this hands the loop the soonest
// deadline it has, the loop sleeps exactly that long, and a page with no timers
// pending hands it nothing and the loop blocks indefinitely.
//
// Time is passed in rather than read from a clock, for the reason the loader
// takes one: two decisions inside a single turn must not disagree about what
// time it is, and a test must be able to say what time it is.
class TimerQueue {
 public:
  // Declares the four globals. `now_ms` is the epoch the deadlines are
  // measured against.
  void Install(js::Interpreter& interpreter, std::int64_t now_ms);

  // Milliseconds until the soonest timer, or nothing when none is pending --
  // which is the answer that lets the loop block rather than wake.
  std::optional<std::uint32_t> NextDelay(std::int64_t now_ms) const;

  // Runs every timer due at `now_ms`, oldest deadline first. True when any
  // ran, which is the caller's signal that the document may have changed.
  bool RunDue(js::Interpreter& interpreter, std::int64_t now_ms);

  // Runs `callback` on a *later turn of the loop*, with no arguments.
  //
  // What the specification calls queueing a task, and this queue is where a
  // task goes in this browser: it is the one thing that already hands the loop
  // a deadline, so a task scheduled here inherits the zero-idle-CPU invariant
  // rather than needing a second mechanism argued for beside it.
  //
  // Not a microtask, and the difference is the whole point. A microtask runs
  // before the current turn ends, so a page using `MessageChannel` to yield to
  // the event loop -- which is exactly what it is for -- would never yield.
  //
  // Delivery calls `Interpreter::BeginTask()` so every macrotask is a fresh
  // host turn (TD-0018). Nested `setTimeout(0)` chains clamp to 4ms after
  // five nestings (HTML), which is what keeps a busy page from spinning the
  // loop at zero delay forever once every task also resets the step budget.
  //
  // `RunDue` also drains newly queued host tasks in the same turn (capped), so
  // stamp slices share one layout rather than one LayoutAndPaint each.
  //
  // Static, and reaches the queue through the interpreter's globals the way
  // `setTimeout` itself does, so a caller that has neither a TimerQueue nor a
  // way to get one can still post a task. False when there is no queue
  // installed, which is a host with no timers rather than a failure.
  static bool QueueTask(js::Interpreter& interpreter, const js::Value& callback);

 private:
  struct Timer {
    double id = 0.0;
    std::int64_t due_ms = 0;
    // Zero for a `setTimeout`, which runs once. A repeating timer reschedules
    // itself by this much.
    std::int64_t interval_ms = 0;
    bool repeating = false;
    // `QueueTask` (MessageChannel): drained in-turn; nesting clamp does not
    // apply (not the timer algorithm).
    bool host_task = false;
    // HTML timer nesting level for this task. Used when *scheduling* the next
    // timeout from inside it.
    std::uint32_t nesting_level = 0;
    // Queued while a CSP-trusted script was running.
    bool trust_scripts = false;
  };

  std::vector<Timer> timers_;
  double next_id_ = 1.0;
  // Nesting level of the timer callback currently running, or 0.
  std::uint32_t active_nesting_ = 0;
};

}  // namespace microbrowser::bindings

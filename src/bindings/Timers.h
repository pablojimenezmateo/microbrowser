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

 private:
  struct Timer {
    double id = 0.0;
    std::int64_t due_ms = 0;
    // Zero for a `setTimeout`, which runs once. A repeating timer reschedules
    // itself by this much.
    std::int64_t interval_ms = 0;
    bool repeating = false;
  };

  std::vector<Timer> timers_;
  double next_id_ = 1.0;
};

}  // namespace microbrowser::bindings

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "js/Interpreter.h"

namespace microbrowser::bindings {

// The budget a callback is told it has when it runs. Not a guarantee -- it is
// the number browsers typically hand out, and it is enough for a page to decide
// whether to do a little work now or ask again.
inline constexpr std::int64_t kIdleCallbackBudgetMs = 50;

// `requestIdleCallback`, and the one that cancels it.
//
// Not a timer and not a frame: a page chooses a timer's deadline, and the
// browser chooses a frame's. An idle callback runs in the gap after those --
// when nothing more urgent is due -- or when an optional timeout expires,
// whichever comes first. ADR 0011's line still holds: nothing pending means
// `NextDelay` answers nothing and the loop may block.
//
// Time is passed in rather than read from a clock, for the reason the timers
// take one: two decisions inside a single turn must not disagree about what
// time it is, and a test must be able to say what time it is.
class IdleCallbacks {
 public:
  void Install(js::Interpreter& interpreter, std::int64_t now_ms);

  // Milliseconds until the soonest idle callback must run, or nothing when
  // none is pending -- which is the answer that lets the loop block.
  std::optional<std::uint32_t> NextDelay(std::int64_t now_ms) const;

  // Runs every callback that is due: either the browser is idle enough or its
  // timeout has passed. True when any ran.
  bool RunDue(js::Interpreter& interpreter, std::int64_t now_ms);

 private:
  struct Entry {
    double id = 0.0;
    // Eligible on the first idle pass at or after this moment.
    std::int64_t idle_after_ms = 0;
    // When set, the callback must run by then even if nothing is idle yet.
    std::int64_t timeout_at_ms = 0;
    bool has_timeout = false;
    bool trust_scripts = false;
  };

  std::vector<Entry> pending_;
  double next_id_ = 1.0;
};

}  // namespace microbrowser::bindings

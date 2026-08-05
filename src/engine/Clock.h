#pragma once

#include <chrono>
#include <cstdint>

namespace microbrowser::engine {

// The engine's two clocks, in one place because the difference between them is
// a decision rather than a detail.
//
// Wall time is what cache entries, cookies and `Access-Control-Max-Age` expire
// against: those are durations a *server* chose, and a clock correction has to
// move them. Steady time is what a timer's delay, an animation frame's boundary
// and a stalled request's deadline are measured in: a page whose `setTimeout`
// fired early because the machine's clock was corrected is a page that broke
// for a reason nobody will ever find.
//
// Private to the module -- deliberately absent from MODULE.deps' `public:`
// list. A caller outside `src/engine` that wanted the time would be a second
// answer to what time it is, and two decisions inside one load disagreeing
// about that is the class of bug both the loader and the timers pass `now` in
// as a parameter to avoid.

inline std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace microbrowser::engine

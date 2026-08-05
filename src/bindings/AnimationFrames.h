#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "js/Interpreter.h"

namespace microbrowser::bindings {

// A frame at 60Hz. Not a configurable rate: matching the display's refresh
// needs the display, and the engine is the half of the seam that does not know
// what machine it is on.
inline constexpr std::int64_t kFrameIntervalMs = 16;

// `requestAnimationFrame`, and the one that cancels it.
//
// **It is not a timer, and the difference is the whole reason this is its own
// class.** A timer is a deadline a page chose; a frame is a deadline the
// browser chose, shared by every callback, and one that only exists while
// something has asked for it. ADR 0011 states the line this file has to hold:
// *a page with no pending animation frame does not schedule a frame at all.*
// That is the point where a browser normally starts burning a core on an idle
// page, by running a 60Hz loop whether or not anything asked for one, and a
// later change that does that has to argue against this comment.
//
// Time is passed in rather than read from a clock, for the reason the timers
// take one: two decisions inside a single turn must not disagree about what
// time it is, and a test must be able to say what time it is.
class AnimationFrames {
 public:
  // Declares the two globals. `now_ms` is the epoch the timestamp handed to a
  // callback is measured from, which is what makes it a duration since the
  // page loaded rather than a number off the system clock -- a high-resolution
  // wall clock is a fingerprinting surface and a cross-process timing oracle.
  void Install(js::Interpreter& interpreter, std::int64_t now_ms);

  // Milliseconds until the next frame boundary, or nothing when no callback is
  // pending. **Nothing is the important answer**: it is what lets the loop
  // block forever on a settled page.
  std::optional<std::uint32_t> NextDelay(std::int64_t now_ms) const;

  // The page's own clock: `now_ms` measured from the epoch this was installed
  // at. The number a callback is handed, and the one an observation record is
  // stamped with -- shared rather than recomputed, because two frame-time
  // sources is how a page's animation and its observer records end up
  // disagreeing about when the same frame happened.
  double Timestamp(std::int64_t now_ms) const {
    return static_cast<double>(now_ms - origin_ms_);
  }

  // Runs every callback registered before this frame, each with the same
  // timestamp -- one frame is one moment, and handing two callbacks two
  // different times is how animations desynchronise. True when any ran.
  bool RunDue(js::Interpreter& interpreter, std::int64_t now_ms);

 private:
  // Ids only. The callables live in the JavaScript heap, for the reason the
  // timers' do: the collector cannot see a `js::Value` in a C++ field, and a
  // callback it cannot see is one it frees while this still points at it.
  std::vector<double> pending_;
  double next_id_ = 1.0;
  std::int64_t origin_ms_ = 0;
  // When the next frame is due. Set when the queue goes from empty to not, so
  // that a page asking for a frame every frame gets an even cadence rather than
  // one that drifts by however long its callback took.
  std::int64_t next_frame_ms_ = 0;
};

}  // namespace microbrowser::bindings

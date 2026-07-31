#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace microbrowser::app {

// How the event loop should wait for its next event this iteration.
//
// This one decision is what makes idle CPU zero. A loop that polls is a loop
// that burns a core doing nothing; a loop that blocks unconditionally never
// animates. The rule is: block unless there is a specific reason not to, and
// make that reason explicit and testable.
//
// Deliberately SDL-free and pure, so the whole policy is exercised in unit
// tests without opening a window — which is the only way a property like "the
// browser does not wake up when nothing is happening" stays true for a year.

enum class IdleWaitMode {
  Poll,         // work is already pending; do not sleep at all
  WaitTimeout,  // sleep until the next scheduled deadline
  Wait,         // nothing scheduled; sleep until the window system says otherwise
};

struct IdleWaitDecision {
  IdleWaitMode mode = IdleWaitMode::Wait;
  std::int32_t timeout_ms = 0;  // only meaningful for WaitTimeout

  friend bool operator==(const IdleWaitDecision&, const IdleWaitDecision&) = default;
};

struct IdleWaitState {
  // A frame is composed but not yet on screen, or damage is queued.
  bool repaint_pending = false;
  // The engine has messages the UI has not consumed, or vice versa. Sleeping
  // with a non-empty queue is how a browser appears to hang.
  bool messages_pending = false;
  // Milliseconds until the soonest scheduled work: a CSS animation tick, a
  // setTimeout, a caret blink, a load-progress update. Absent means nothing is
  // scheduled at all, which is the case that must block.
  std::optional<std::uint32_t> next_deadline_ms;
};

// Clamped to at least 1ms so a zero deadline cannot turn a timed wait into a
// busy spin — a deadline that has already passed still yields one sleep, and
// the work runs on the very next iteration.
inline constexpr std::int32_t kMinimumTimeoutMs = 1;

inline IdleWaitDecision ChooseIdleWait(const IdleWaitState& state) {
  if (state.repaint_pending || state.messages_pending) {
    return IdleWaitDecision{IdleWaitMode::Poll, 0};
  }
  if (state.next_deadline_ms.has_value()) {
    const std::int32_t requested = static_cast<std::int32_t>(
        std::min<std::uint32_t>(*state.next_deadline_ms,
                                static_cast<std::uint32_t>(INT32_MAX)));
    return IdleWaitDecision{IdleWaitMode::WaitTimeout, std::max(kMinimumTimeoutMs, requested)};
  }
  return IdleWaitDecision{IdleWaitMode::Wait, 0};
}

}  // namespace microbrowser::app

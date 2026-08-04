#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

#include "util/WaitDescriptor.h"

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
//
// ADR 0011 added the second input. This used to answer only "how long until the
// soonest deadline"; it now also answers "and which descriptors am I waiting
// on", so a network request in flight is something the loop sleeps *on* rather
// than something it sleeps *through*. The invariant survives for the same
// reason it survived timers: the loop is told what to wait for rather than
// asked to check. A browser with nothing outstanding hands over no descriptors
// and blocks on input alone, exactly as before.

enum class IdleWaitMode {
  Poll,         // work is already pending; do not sleep at all
  WaitTimeout,  // sleep until the next scheduled deadline
  Wait,         // nothing scheduled; sleep until something says otherwise
};

struct IdleWaitDecision {
  IdleWaitMode mode = IdleWaitMode::Wait;
  std::int32_t timeout_ms = 0;  // only meaningful for WaitTimeout
  // The wait must also watch the descriptors the decision was computed from.
  // A separate field rather than a fourth mode because it is orthogonal to the
  // other three: a request in flight while a timer is pending is an ordinary
  // state, and folding them into one enum would mean choosing which of the two
  // the loop is allowed to notice.
  bool watch_descriptors = false;

  friend bool operator==(const IdleWaitDecision&, const IdleWaitDecision&) = default;
};

struct IdleWaitState {
  // A frame is composed but not yet on screen, or damage is queued.
  bool repaint_pending = false;
  // The engine has messages the UI has not consumed, or vice versa. Sleeping
  // with a non-empty queue is how a browser appears to hang.
  bool messages_pending = false;
  // Milliseconds until the soonest scheduled work: a CSS animation tick, a
  // setTimeout, an animation frame, a caret blink. Absent means nothing is
  // scheduled at all, which is the case that must block.
  std::optional<std::uint32_t> next_deadline_ms;
  // Sockets with a request outstanding on them. Borrowed for the length of one
  // wait: the transports that opened them outlive the decision, and the
  // navigation that closes them happens between iterations, never during one.
  std::span<const util::WaitDescriptor> descriptors;
};

// Clamped to at least 1ms so a zero deadline cannot turn a timed wait into a
// busy spin — a deadline that has already passed still yields one sleep, and
// the work runs on the very next iteration.
inline constexpr std::int32_t kMinimumTimeoutMs = 1;

inline IdleWaitDecision ChooseIdleWait(const IdleWaitState& state) {
  if (state.repaint_pending || state.messages_pending) {
    // There is work to do now, so there is nothing to wait for. The descriptors
    // go unwatched on this iteration and do not need watching: the loop comes
    // straight back round and watches them then.
    return IdleWaitDecision{IdleWaitMode::Poll, 0, false};
  }
  const bool watch = !state.descriptors.empty();
  if (state.next_deadline_ms.has_value()) {
    const std::int32_t requested = static_cast<std::int32_t>(
        std::min<std::uint32_t>(*state.next_deadline_ms,
                                static_cast<std::uint32_t>(INT32_MAX)));
    return IdleWaitDecision{IdleWaitMode::WaitTimeout,
                            std::max(kMinimumTimeoutMs, requested), watch};
  }
  // Descriptors and no deadline is still an indefinite block: a socket becoming
  // readable is an event, in exactly the sense a keypress is.
  return IdleWaitDecision{IdleWaitMode::Wait, 0, watch};
}

}  // namespace microbrowser::app

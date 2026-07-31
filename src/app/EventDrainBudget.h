#pragma once

namespace microbrowser::app {

// Event-drain budget for the main loop.
//
// The inner `do { Handle(event) } while (PollEvent(event))` would otherwise
// drain the entire window-system queue before returning to the render step.
// Pointer motion coalesces, but a sustained flood of wheel, key, or resize
// events keeps accumulating damage and delays visible feedback well past one
// frame — which reads to the user as the browser having stopped responding
// while it is in fact working as fast as it can.
//
// Once a repaint is pending, yield to render after this many events; the
// remainder is handled on the next iteration, after a frame is on screen.
// Window-system events are discrete (no multi-event atomic sequence spans a
// single poll), so breaking mid-queue preserves per-event ordering.
inline constexpr int kMaxEventsPerDrain = 512;

inline bool ShouldYieldEventDrain(int events_processed,
                                  bool repaint_pending,
                                  int budget = kMaxEventsPerDrain) {
  return repaint_pending && events_processed >= budget;
}

}  // namespace microbrowser::app

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::util {

// When each thing in a page load first happened, on one clock.
//
// The gap this fills is specific. A ranked scope summary answers "which code
// was expensive", and for a page whose whole cost is round trips it answers
// nothing at all: Hacker News spends 1.21 seconds of a 1.41-second load blocked
// on a socket, and every scope in the table put together accounts for 58ms of
// it. Ranking cannot show that, because the interesting quantity is not a
// duration at all -- it is *when* each milestone happened relative to the
// navigation, and how long the gaps between them are.
//
// So this is a list of stamped moments rather than a set of timers, and it is
// printed in the order they happened rather than ranked. The gap column is the
// answer: the row after a long gap is what the browser was waiting for.
//
// One navigation at a time. `Begin` resets, so a page that navigates twice
// reports the second one -- which is the one the reader just watched. Off
// unless `MICROBROWSER_LOAD_TIMELINE=1`, and one relaxed load when off.
class LoadTimeline {
 public:
  // Starts a navigation, discarding any previous one. `what` is the URL.
  static void Begin(std::string_view what);

  // Stamps a milestone with the time since `Begin`. Cheap enough to call
  // unconditionally: when the timeline is off this is a load and a branch.
  //
  // A milestone that happens more than once -- a subresource arriving, a layout
  // -- is recorded every time and printed with its index, because "the first
  // stylesheet landed at 240ms and the last at 980ms" is exactly the shape of
  // the question this exists to answer.
  static void Mark(std::string_view milestone);

  // As above, with a detail that is only built when the timeline is on.
  static void MarkWith(std::string_view milestone, std::string_view detail);

  static bool Enabled();

  // Writes the table. Idempotent, because the shutdown path and the atexit
  // backstop both call it.
  static void DumpOnce(std::FILE* out);

  struct Entry {
    std::string milestone;
    std::string detail;
    double at_ms = 0.0;
  };
  static std::vector<Entry> Snapshot();
};

}  // namespace microbrowser::util

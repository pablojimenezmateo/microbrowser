#pragma once

#include <cstddef>
#include <vector>

namespace microbrowser::media {

// The set of time ranges a source buffer holds, coalesced.
//
// ADR 0028 §3, session 28. **This is the most load-bearing small type in MSE.** A player reads
// `buffered` every few hundred milliseconds and decides from it what to fetch next: a `buffered` that
// claims a range it does not have makes the player skip a fetch and then stall forever, and one that
// omits a range it does have makes the player re-fetch the same bytes forever. Either way the symptom
// is a video that never plays, and the cause is arithmetic in fifty lines.
//
// So the invariants are stated and a test asserts each of them after every operation:
//
//   * **Sorted by start**, ascending.
//   * **Non-empty**: no range where `end <= start`.
//   * **Disjoint and separated by more than `kJoinTolerance`**: ranges that touch, or that miss each
//     other by less than a microsecond, are one range. The specification's `buffered` is a
//     `TimeRanges`, and two adjacent entries in it mean a *gap* to a player -- which is the thing it
//     will try to fetch across.
//
// Times are seconds as doubles, because that is what the API exposes and converting at the edge is
// what keeps the conversion in one place. The fuzz target asserts the invariants over arbitrary
// sequences of adds and removes, including the ones that produce NaN if written carelessly.
class BufferedRanges {
 public:
  struct Range {
    double start = 0.0;
    double end = 0.0;

    friend bool operator==(const Range&, const Range&) = default;
  };

  // How close two ranges must be to count as one. **This is not a fudge factor, it is the unit the
  // times are actually known to.** A presentation time arrives as an integer tick count over an
  // integer timescale; two segments that abut exactly in ticks can land a few ulps apart in seconds
  // once a `timestampOffset` has been added to both, and a gap of 2e-17 seconds reported to a player
  // is a gap it will spend a request trying to fill. A microsecond is a thousandth of a millisecond
  // and a forty-thousandth of a frame at 25fps: far below anything a real gap can be, far above
  // double-precision noise for a timestamp hours into a stream.
  //
  // The arithmetic that produces the times is exact wherever it can be -- `SourceBufferState` divides
  // `(decode_time + duration)` once rather than dividing each and adding -- and this covers what
  // remains. Both halves are needed: without the exact arithmetic, a tenth of a second of 25fps video
  // split into ten appends produced ten ranges.
  static constexpr double kJoinTolerance = 1e-6;

  // Adds [start, end), coalescing with everything it touches. A non-finite or empty range is
  // ignored rather than stored: `appendBuffer` of a segment whose timestamps did not parse must not
  // be able to put a NaN into the set a page then reads.
  void Add(double start, double end);

  // Removes [start, end), splitting a range it lands inside. This is `SourceBuffer.remove`, and it
  // is also half of an overlapping append -- the coded frame processing algorithm removes what an
  // append overlaps before adding it.
  //
  // A span narrower than `kJoinTolerance` removes nothing, and a remainder narrower than it is
  // dropped. That keeps the set's promise uniform -- and it is a *bound*: without it, a page calling
  // `remove` with sub-microsecond spans in a loop fragments one range into one entry per call, which
  // is unbounded memory reached through the eviction API.
  void Remove(double start, double end);

  void Clear() { ranges_.clear(); }

  const std::vector<Range>& Ranges() const { return ranges_; }
  std::size_t Count() const { return ranges_.size(); }
  bool Empty() const { return ranges_.empty(); }

  // Whether `time` falls in any range. What a player's "do I have this frame?" question is, and what
  // the media element's readiness ladder is driven from.
  bool Contains(double time) const;

  // The largest end, or zero when empty. `duration` for a stream still being appended is derived
  // from this, which is why it is here rather than computed by every caller.
  double LargestEnd() const;

 private:
  std::vector<Range> ranges_;
};

}  // namespace microbrowser::media

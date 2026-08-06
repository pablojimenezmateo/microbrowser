#include "media/BufferedRanges.h"

#include <algorithm>
#include <cmath>

namespace microbrowser::media {

namespace {

// A range is usable only when both ends are finite and it holds something. Checked at the two entry
// points rather than trusted, because both are reachable from a page: `remove(NaN, 5)` is one call
// from script, and a NaN that reaches the vector poisons every comparison afterwards -- a sorted
// container with a NaN in it is not sorted, and `std::sort` on it is undefined.
bool Usable(double start, double end) {
  return std::isfinite(start) && std::isfinite(end) && end > start;
}

}  // namespace

void BufferedRanges::Add(double start, double end) {
  if (!Usable(start, end)) {
    return;
  }
  // Everything this range touches -- including *adjacent* ranges, where `end == other.start`. Two
  // adjacent entries in a `TimeRanges` mean a gap to the player reading it, so they must merge.
  double low = start;
  double high = end;
  std::vector<Range> merged;
  merged.reserve(ranges_.size() + 1);
  bool placed = false;
  for (const Range& range : ranges_) {
    if (range.end < low - kJoinTolerance) {
      merged.push_back(range);
      continue;
    }
    if (range.start > high + kJoinTolerance) {
      if (!placed) {
        merged.push_back(Range{low, high});
        placed = true;
      }
      merged.push_back(range);
      continue;
    }
    low = std::min(low, range.start);
    high = std::max(high, range.end);
  }
  if (!placed) {
    merged.push_back(Range{low, high});
  }
  ranges_ = std::move(merged);
}

void BufferedRanges::Remove(double start, double end) {
  if (!Usable(start, end)) {
    return;
  }
  // **A removal narrower than the join tolerance removes nothing**, and this is a bound rather than a
  // nicety. Without it, `for (i) sb.remove(i * 1e-9, i * 1e-9 + 1e-12)` splits one range into one
  // entry per call -- a page controls both the count and the widths, so `buffered` becomes unbounded
  // memory reached through an eviction API. The fuzzer found it as an invariant violation (two ranges
  // 1e-301 apart), which is the same bug seen from the other side.
  //
  // It is also the honest answer: a coded frame is milliseconds long, so a span shorter than a
  // microsecond contains no frame and removing it removes nothing.
  if (end - start < kJoinTolerance) {
    return;
  }
  std::vector<Range> kept;
  kept.reserve(ranges_.size() + 1);
  for (const Range& range : ranges_) {
    if (range.end <= start || range.start >= end) {
      kept.push_back(range);
      continue;
    }
    // The left remainder, then the right one. **A removal strictly inside a range produces two**,
    // which is the case a naive implementation loses -- and losing it means claiming to hold frames
    // that were evicted, which is worse than claiming to hold none.
    // A remainder narrower than the tolerance is dropped rather than kept, for the same reason: two
    // entries that close together are one to every reader, and keeping them is what would let the
    // fragmentation above happen one range at a time.
    if (range.start < start && start - range.start >= kJoinTolerance) {
      kept.push_back(Range{range.start, start});
    }
    if (range.end > end && range.end - end >= kJoinTolerance) {
      kept.push_back(Range{end, range.end});
    }
  }
  ranges_ = std::move(kept);
}

bool BufferedRanges::Contains(double time) const {
  if (!std::isfinite(time)) {
    return false;
  }
  for (const Range& range : ranges_) {
    if (time >= range.start && time < range.end) {
      return true;
    }
  }
  return false;
}

double BufferedRanges::LargestEnd() const {
  return ranges_.empty() ? 0.0 : ranges_.back().end;
}

}  // namespace microbrowser::media

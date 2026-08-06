#include <cstddef>
#include <cstdint>
#include <cstring>

#include "media/BufferedRanges.h"

// The set of time ranges a `SourceBuffer` holds, under arbitrary sequences of adds and removes.
//
// ADR 0028 §3. The input is a page's: `appendBuffer` and `remove` are both script-callable with
// script-chosen times, and `buffered` is read back by the page's own player logic. So the property
// being fuzzed is not memory safety -- it is that **the set is always well-formed**, because a
// malformed one is not a crash, it is a player that downloads the wrong bytes forever.
//
// Three invariants, checked after every single operation rather than at the end:
//
//   * sorted by start, ascending;
//   * no empty range (`end <= start`);
//   * no two ranges closer than the join tolerance -- two entries a player reads as separate mean a
//     gap between them, and a gap is a fetch.
//
// The doubles are taken straight from the input bytes, so NaN, the infinities, negative zero and the
// denormals all arrive: `remove(NaN, NaN)` is one call from a page, and a NaN inside a sorted vector
// means the vector is not sorted and the next comparison on it is undefined.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using microbrowser::media::BufferedRanges;
  BufferedRanges ranges;
  std::size_t at = 0;
  // Seventeen bytes per operation: one selector plus two doubles. Reading raw bit patterns rather than
  // deriving values from a small integer is the point -- a generator that only produced sensible
  // times would never produce the ones a page can.
  while (at + 17 <= size) {
    const std::uint8_t op = data[at];
    double start = 0.0;
    double end = 0.0;
    std::memcpy(&start, data + at + 1, sizeof(start));
    std::memcpy(&end, data + at + 9, sizeof(end));
    at += 17;
    switch (op % 4) {
      case 0:
        ranges.Add(start, end);
        break;
      case 1:
        ranges.Remove(start, end);
        break;
      case 2:
        // Contains must answer without reading past the end and without asserting, whatever is in it.
        (void)ranges.Contains(start);
        (void)ranges.LargestEnd();
        break;
      default:
        // An add followed by a remove of the same range must leave the set as it was found -- not
        // asserted here (an earlier range may overlap), but the pair exercises the split path with
        // values the two other cases would not reach together.
        ranges.Add(start, end);
        ranges.Remove(start, end);
        break;
    }
    const auto& list = ranges.Ranges();
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (!(list[i].end > list[i].start)) {
        __builtin_trap();  // an empty range
      }
      if (i > 0 && !(list[i].start > list[i - 1].end + BufferedRanges::kJoinTolerance)) {
        __builtin_trap();  // unsorted, overlapping, or too close to be two
      }
    }
    // `LargestEnd` is documented as the largest end, and the last range's end is only that if the set
    // is sorted -- so this is the sortedness invariant stated a second way, from the caller's side.
    if (!list.empty() && ranges.LargestEnd() != list.back().end) {
      __builtin_trap();
    }
  }
  return 0;
}

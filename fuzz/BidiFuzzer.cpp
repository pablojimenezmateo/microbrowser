#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "text/Bidi.h"

// The bidirectional algorithm, on arbitrary code points.
//
// ADR 0025 §3. The input is a page's text, so it is attacker-controlled -- but the reason to fuzz
// this is not memory safety in the usual sense. UAX #9 is a stack machine with three overflow
// counters, a pass that joins level runs across matched isolates, and a reordering that reverses
// stretches of an array in place. Every one of those is an index computed from the text, and the
// specification's own depth limit is 125 with the *overflow* cases specified separately -- which is
// to say the interesting inputs are the malformed ones: unmatched pops, isolates that never close,
// two hundred nested embeddings, a PDI with no initiator.
//
// The invariants are the ones a caller depends on, and each of them is something layout would get
// silently wrong rather than crash on:
//
//   * **One level per code point.** Layout indexes the level array by position.
//   * **The runs cover the text exactly once.** A run list that dropped a position drops text from
//     the screen; one that repeated a position draws it twice, overlapping.
//   * **A run is contiguous and in bounds**, because it is handed to a shaper as a slice.
//   * **`right_to_left` agrees with the level's parity**, which every caller relies on to decide
//     which end of a run to paint first.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  if (size < 2) {
    return 0;
  }
  // The paragraph level out of the first byte: 0, 1, or "ask the text" -- so P2/P3 is exercised too.
  const std::uint8_t selector = data[0] % 3;
  // Code points from pairs of bytes, biased into the ranges that matter. A uniform sample of 21 bits
  // would be almost entirely unassigned Han and would never produce an isolate initiator; this makes
  // the controls, the Hebrew and Arabic blocks and the brackets all reachable in a two-byte draw.
  std::vector<std::uint32_t> text;
  for (std::size_t i = 1; i + 1 < size; i += 2) {
    const std::uint32_t low = data[i];
    switch (data[i + 1] % 6) {
      case 0:
        text.push_back(low);  // ASCII and Latin-1: L, EN, ES, ET, CS, WS, S, B, ON
        break;
      case 1:
        text.push_back(0x0590u + (low % 0x70u));  // Hebrew: R
        break;
      case 2:
        text.push_back(0x0600u + (low % 0x80u));  // Arabic: AL, AN, NSM
        break;
      case 3:
        text.push_back(0x202Au + (low % 6u));  // LRE RLE PDF LRO RLO and one past
        break;
      case 4:
        text.push_back(0x2066u + (low % 5u));  // LRI RLI FSI PDI and one past
        break;
      default:
        text.push_back(0x2000u + low * 0x40u + (low % 0x40u));  // brackets, marks, anything
        break;
    }
  }
  if (text.empty()) {
    return 0;
  }
  const std::uint8_t paragraph_level =
      selector == 2 ? text::ParagraphLevel(text) : static_cast<std::uint8_t>(selector);
  if (paragraph_level > 1) {
    __builtin_trap();  // P2/P3 answers a paragraph level, which is 0 or 1 and nothing else.
  }

  const std::vector<std::uint8_t> levels = text::ResolveLevels(text, paragraph_level);
  if (levels.size() != text.size()) {
    __builtin_trap();
  }

  const std::vector<text::BidiRun> runs = text::ResolveVisualRuns(text, paragraph_level);
  std::vector<std::uint8_t> seen(text.size(), 0);
  std::size_t covered = 0;
  for (const text::BidiRun& run : runs) {
    if (run.length == 0 || run.start >= text.size() || run.start + run.length > text.size()) {
      __builtin_trap();
    }
    if (run.right_to_left != ((run.level & 1) != 0)) {
      __builtin_trap();
    }
    for (std::size_t k = 0; k < run.length; ++k) {
      if (seen[run.start + k] != 0) {
        __builtin_trap();  // this position is in two runs
      }
      seen[run.start + k] = 1;
      // A run is uniform in level, which is what makes it one shaping unit.
      if (levels[run.start + k] != run.level) {
        __builtin_trap();
      }
    }
    covered += run.length;
  }
  if (covered != text.size()) {
    __builtin_trap();  // some position is in no run at all
  }
  return 0;
}

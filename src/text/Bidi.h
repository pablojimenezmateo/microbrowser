#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::text {

// The Unicode Bidirectional Algorithm, UAX #9.
//
// ADR 0025 §3, session 33. Without this, Arabic and Hebrew are stored in logical order and drawn in
// logical order, which is **backwards**: the first character of a Hebrew sentence appears on the left
// where it belongs on the right. There is no partial credit here and no heuristic that helps -- either
// the algorithm runs or right-to-left text is wrong.
//
// **Where this sits is the load-bearing decision.** It runs between line breaking and shaping:
//
//   * *after* line breaking, because rules L1 and L2 reorder per **line**, not per paragraph. A
//     paragraph reordered as a whole and then broken puts the wrong half of a run on each line.
//   * *before* shaping, because a shaped run must be uniform in direction. HarfBuzz shapes an Arabic
//     run correctly when told it is right-to-left; handed a mixed-direction string it produces
//     glyphs from a context that was never right, and reversing glyphs afterwards does not fix a
//     ligature that should not have formed.
//
// So the interface is: give it the text of one line and a paragraph level, get back **visual runs** --
// each one uniform in level, in the order they are painted, left to right.
//
// What is here is the whole algorithm except the three things session 34 owns: mirrored glyphs, the
// two-position caret at a direction boundary, and `dir="auto"`. Rule N0's bracket pairs *are* here,
// because they are part of resolving levels rather than part of painting.

// UAX #9 Table 4. All twenty-three, and none folded -- a bidi class folded to the wrong one reverses
// text, and the rules name classes individually (W1 is about NSM, N0 about brackets), so a missing
// class is a rule that silently never fires.
enum class BidiClass : std::uint8_t {
  L, R, AL,                                 // strong
  EN, ES, ET, AN, CS, NSM, BN,              // weak
  B, S, WS, ON,                             // neutral
  LRE, RLE, LRO, RLO, PDF,                  // explicit embeddings and overrides
  LRI, RLI, FSI, PDI,                       // isolates
};

BidiClass BidiClassOf(std::uint32_t code_point);

// Rule L4: what to paint instead of this character when it sits in a right-to-left run, or the
// character itself when nothing mirrors. `(` in Hebrew text paints as `)` -- the *character* is still
// U+0028 and copying the text still yields U+0028, which is why this is a paint-time substitution and
// not a rewrite of the text.
std::uint32_t MirroredGlyph(std::uint32_t code_point);

// `text` with every mirrorable character replaced, for a right-to-left run. Returns the input
// unchanged -- no allocation, no copy -- when nothing in it mirrors, which is the overwhelmingly
// common case even in Arabic and Hebrew.
std::string MirrorForRightToLeft(std::string_view utf8);

// The paired bracket for rule N0, and whether this code point opens one. Zero when it is not a
// bracket at all.
std::uint32_t PairedBracket(std::uint32_t code_point, bool& opens);

// One run of uniform embedding level, in **visual** order: the first element is painted leftmost.
// `start` and `length` are offsets into the code point sequence that was passed in, so a run is
// always a contiguous slice of the logical text -- which is what makes it shapeable.
struct BidiRun {
  std::size_t start = 0;
  std::size_t length = 0;
  std::uint8_t level = 0;
  // Odd levels are right-to-left. Stated as a field rather than left to `level & 1` at every call
  // site, because getting that parity backwards is a bug that looks like a font problem.
  bool right_to_left = false;
};

// The direction a paragraph is in, which comes from CSS `direction` -- or, when the author said
// nothing, from the text itself (rule P2/P3).
enum class BidiDirection : std::uint8_t { LeftToRight, RightToLeft, Auto };

// Rules P2 and P3: the level of a paragraph whose direction is `Auto`, found from its first strong
// character. `Auto` is what `dir="auto"` and the initial value of an unstyled document mean.
std::uint8_t ParagraphLevel(const std::vector<std::uint32_t>& text);

// The whole algorithm, X1 through L2, for one line of one paragraph.
//
// `paragraph_level` is 0 for a left-to-right paragraph and 1 for a right-to-left one. The result is
// in visual order and covers every code point exactly once -- which a test asserts, because a
// reordering that dropped or duplicated a run would drop or duplicate text on screen.
std::vector<BidiRun> ResolveVisualRuns(const std::vector<std::uint32_t>& text,
                                       std::uint8_t paragraph_level);

// The resolved level per code point, before reordering. Exposed because the caret and selection need
// it (session 34) and because it is the thing worth testing against the UCD's own conformance data:
// BidiTest.txt states expected levels, not expected runs.
std::vector<std::uint8_t> ResolveLevels(const std::vector<std::uint32_t>& text,
                                        std::uint8_t paragraph_level);

// Whether a line needs any of this. **The fast path that keeps bidi from costing every page
// something**: a line with no right-to-left character, no explicit control and no Arabic number is
// already in visual order, and this answers that in one pass with no allocation. Hacker News is
// entirely this case.
bool NeedsBidi(std::string_view utf8);

}  // namespace microbrowser::text

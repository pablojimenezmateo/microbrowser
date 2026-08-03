#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The compiled form of a regular expression, and the matcher that runs it.
//
// Module-private on purpose: a caller wants `RegExp::Exec`, not an instruction
// list. Split from RegExp.cpp because the compiler and the matcher are two
// separate things that only share this vocabulary, and putting both in one
// translation unit puts it over the module's line limit.

namespace microbrowser::js {

// A set of bytes, as a bitmap. Every single-byte test in the matcher goes
// through one of these -- `.`, `\d`, `[a-z]` and a case-folded literal are all
// the same operation once compiled, which is one test rather than four.
struct CharSet {
  std::array<std::uint64_t, 4> bits{};

  void Add(unsigned char c) { bits[c >> 6] |= std::uint64_t{1} << (c & 63); }
  void AddRange(unsigned int low, unsigned int high);
  void Union(const CharSet& other);
  void Negate();
  bool Test(unsigned char c) const { return ((bits[c >> 6] >> (c & 63)) & 1) != 0; }
  bool Empty() const;
};

enum class Op : std::uint8_t {
  // Consumes one byte if it is in `classes[x]`.
  Class,
  // Consumes `x`'s class greedily, between `y` and `z` times, and backtracks
  // by giving bytes back one at a time. The generic Split form below expresses
  // the same thing, but pushes one stack frame per byte consumed -- which is
  // what turns `/\s*/` over a large document into megabytes of backtrack
  // stack. This is the same loop with one frame.
  RepeatClass,
  // Try `x`; on failure, try `y`.
  Split,
  Jump,
  // Records the current position in register `x`. Captures and loop-progress
  // markers are both registers, because both have to be restored on backtrack
  // and one undo log is easier to be right about than two mechanisms.
  Save,
  // Sets registers `x`..`y` to absent. A quantified group's captures reset at
  // the start of every iteration: `/(?:(a)|b)+/.exec("ab")` leaves group 1
  // absent, because the last iteration is the one that did not match it.
  Clear,
  // Fails when the position equals register `x`. This is what stops `(a*)*`
  // from looping forever on a body that can match nothing.
  Progress,
  // `x` is an AssertKind.
  Assert,
  // Re-matches whatever group `x` captured.
  Backref,
  // Runs sub-program `x`. `y` is 1 for a negative assertion, `z` is 1 for a
  // lookbehind.
  Look,
  Match,
};

enum class AssertKind : std::uint8_t { Begin, End, WordBoundary, NotWordBoundary };

struct Instruction {
  Op op = Op::Match;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t z = 0;
};

struct RegExpProgram {
  std::vector<Instruction> code;
  std::vector<CharSet> classes;
  // Lookaround bodies. They share the caller's register file, because the
  // captures a positive lookahead makes are observable after it succeeds.
  std::vector<RegExpProgram> subs;

  std::size_t group_count = 0;
  std::size_t register_count = 0;
  std::vector<std::string> group_names;

  bool ignore_case = false;
  bool multiline = false;

  // The bytes a match can begin with, when that is knowable. Lets the search
  // skip start positions instead of running the program at each one, which is
  // the difference between O(n) and O(n·m) for a pattern that starts with a
  // literal.
  bool has_first_set = false;
  CharSet first_set;
  // True when the pattern can only match at the start of the input, so the
  // search has exactly one start position to try.
  bool anchored_at_start = false;
};

// Everything a match writes to.
//
// One register file and one undo log for the whole match, lookarounds
// included: a lookaround that fails has to leave the captures it made exactly
// as it found them, and so does the outer match when it backtracks past a
// lookaround that succeeded. Two logs would mean two chances to get that
// wrong.
struct MatchState {
  std::vector<std::size_t> registers;
  // (register, value before the write), newest last.
  std::vector<std::pair<std::uint32_t, std::size_t>> log;
  std::size_t steps = 0;
};

// Runs `program` from `start`. `required_end` is `npos` unless the match must
// end at an exact position, which is how a lookbehind is checked.
//
// Returns false for "no match", which is also the answer when a bound is hit.
// A failed run leaves `state` as it found it; a successful one keeps the
// registers it wrote.
bool RunProgram(const RegExpProgram& program, std::string_view input, std::size_t start,
                std::size_t required_end, MatchState& state, std::size_t* end_out);

// The step budget one call to `RegExp::Exec` may spend. Reached only by a
// pattern that backtracks catastrophically, which a page can write by accident
// as easily as on purpose.
inline constexpr std::size_t kMaxMatchSteps = 4'000'000;

}  // namespace microbrowser::js

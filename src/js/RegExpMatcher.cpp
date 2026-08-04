#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "js/RegExp.h"
#include "js/RegExpProgram.h"

// The matcher.
//
// Backtracking, with an explicit stack rather than C++ recursion. That is not
// a style preference: the number of alternatives a pattern opens is chosen by
// whoever wrote the pattern, and a recursive matcher makes that the process's
// stack depth. Here it is a vector with a limit, and hitting the limit is a
// reported failure instead of a segfault. Lookarounds do recurse, but only as
// deep as they are nested in the source, which the parser bounds.

namespace microbrowser::js {

namespace {

constexpr std::size_t kAbsent = RegExpMatch::kAbsent;
constexpr std::size_t kNoEnd = static_cast<std::size_t>(-1);

// Frames, not bytes: the point of the bound is that it exists and is far past
// any real pattern, and 200k frames is a few megabytes.
constexpr std::size_t kMaxStackFrames = 200'000;

bool IsWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsLineTerminator(unsigned char c) { return c == '\n' || c == '\r'; }

unsigned char Fold(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

}  // namespace

bool RunProgram(const RegExpProgram& program, std::string_view input, std::size_t start,
                std::size_t required_end, MatchState& state, std::size_t* end_out) {
  // A choice point. `repeat` frames stand for a whole range of positions --
  // everything a greedy RepeatClass consumed above `floor` -- so that giving
  // back one byte at a time costs one frame rather than one per byte.
  struct Frame {
    std::size_t pc = 0;
    std::size_t pos = 0;
    std::size_t log_size = 0;
    std::size_t floor = 0;
    bool repeat = false;
  };

  std::vector<Frame> stack;
  const std::size_t entry_log = state.log.size();
  const std::size_t size = input.size();
  std::size_t pc = 0;
  std::size_t pos = start;

  const auto write = [&state](std::uint32_t reg, std::size_t value) {
    if (reg >= state.registers.size()) {
      return;  // the compiler sizes the file; this cannot fire, and is not a crash if it does
    }
    state.log.emplace_back(reg, state.registers[reg]);
    state.registers[reg] = value;
  };
  const auto undo_to = [&state](std::size_t mark) {
    while (state.log.size() > mark) {
      const std::pair<std::uint32_t, std::size_t>& entry = state.log.back();
      state.registers[entry.first] = entry.second;
      state.log.pop_back();
    }
  };
  const auto give_up = [&]() {
    undo_to(entry_log);
    return false;
  };

  for (;;) {
    if (++state.steps > kMaxMatchSteps) {
      return give_up();
    }

    bool failed = false;
    const MatchInstruction& instruction = program.code[pc];
    switch (instruction.op) {
      case MatchOp::Class: {
        if (pos < size &&
            program.classes[instruction.x].Test(static_cast<unsigned char>(input[pos]))) {
          ++pos;
          ++pc;
        } else {
          failed = true;
        }
        break;
      }

      case MatchOp::RepeatClass: {
        const CharSet& set = program.classes[instruction.x];
        const std::size_t low = instruction.y;
        const std::size_t high = instruction.z;
        std::size_t count = 0;
        while (count < high && pos < size &&
               set.Test(static_cast<unsigned char>(input[pos]))) {
          ++pos;
          ++count;
        }
        if (count < low) {
          failed = true;
          break;
        }
        // Everything above this position was optional, and is what backtracking
        // gives back.
        const std::size_t floor = pos - (count - low);
        if (pos > floor) {
          if (stack.size() >= kMaxStackFrames) {
            return give_up();
          }
          stack.push_back(Frame{pc + 1, pos - 1, state.log.size(), floor, true});
        }
        ++pc;
        break;
      }

      case MatchOp::Split: {
        if (stack.size() >= kMaxStackFrames) {
          return give_up();
        }
        stack.push_back(Frame{instruction.y, pos, state.log.size(), 0, false});
        pc = instruction.x;
        break;
      }

      case MatchOp::Jump:
        pc = instruction.x;
        break;

      case MatchOp::Save:
        write(instruction.x, pos);
        ++pc;
        break;

      case MatchOp::Clear:
        for (std::uint32_t reg = instruction.x; reg <= instruction.y; ++reg) {
          write(reg, kAbsent);
        }
        ++pc;
        break;

      case MatchOp::Progress:
        // The loop head recorded where this iteration started. Matching
        // nothing means the next iteration would do the same forever.
        if (instruction.x < state.registers.size() &&
            state.registers[instruction.x] == pos) {
          failed = true;
        } else {
          ++pc;
        }
        break;

      case MatchOp::Assert: {
        bool holds = false;
        switch (static_cast<AssertKind>(instruction.x)) {
          case AssertKind::Begin:
            holds = pos == 0 || (program.multiline &&
                                 IsLineTerminator(static_cast<unsigned char>(input[pos - 1])));
            break;
          case AssertKind::End:
            holds = pos == size ||
                    (program.multiline && IsLineTerminator(static_cast<unsigned char>(input[pos])));
            break;
          case AssertKind::WordBoundary:
          case AssertKind::NotWordBoundary: {
            const bool before = pos > 0 && IsWordByte(static_cast<unsigned char>(input[pos - 1]));
            const bool after = pos < size && IsWordByte(static_cast<unsigned char>(input[pos]));
            holds = (before != after) ==
                    (static_cast<AssertKind>(instruction.x) == AssertKind::WordBoundary);
            break;
          }
        }
        if (holds) {
          ++pc;
        } else {
          failed = true;
        }
        break;
      }

      case MatchOp::Backref: {
        const std::size_t begin = state.registers[2 * instruction.x];
        const std::size_t end = state.registers[2 * instruction.x + 1];
        if (begin == kAbsent || end == kAbsent || end < begin) {
          ++pc;  // a group that never participated matches the empty string
          break;
        }
        const std::size_t length = end - begin;
        if (length > size - pos) {
          failed = true;
          break;
        }
        bool same = true;
        for (std::size_t i = 0; i < length; ++i) {
          const unsigned char a = static_cast<unsigned char>(input[begin + i]);
          const unsigned char b = static_cast<unsigned char>(input[pos + i]);
          if (program.ignore_case ? Fold(a) != Fold(b) : a != b) {
            same = false;
            break;
          }
        }
        if (same) {
          pos += length;
          ++pc;
        } else {
          failed = true;
        }
        break;
      }

      case MatchOp::Look: {
        const RegExpProgram& sub = program.subs[instruction.x];
        const bool negated = instruction.y != 0;
        const std::size_t mark = state.log.size();
        bool matched = false;
        if (instruction.z == 0) {
          matched = RunProgram(sub, input, pos, kNoEnd, state, nullptr);
        } else {
          // Lookbehind, without a reversed matcher: ask whether the body
          // matches some earlier position and ends exactly here. The cost is a
          // scan back to the start of the input, which the step budget bounds.
          for (std::size_t from = pos + 1; from-- > 0;) {
            if (RunProgram(sub, input, from, pos, state, nullptr)) {
              matched = true;
              break;
            }
          }
        }
        // A negative assertion keeps nothing it matched, and neither does a
        // positive one that failed.
        if (negated || !matched) {
          undo_to(mark);
        }
        if (matched == negated) {
          failed = true;
        } else {
          ++pc;
        }
        break;
      }

      case MatchOp::Match:
        if (required_end != kNoEnd && pos != required_end) {
          failed = true;
          break;
        }
        if (end_out != nullptr) {
          *end_out = pos;
        }
        return true;
    }

    if (!failed) {
      continue;
    }
    if (stack.empty()) {
      return give_up();
    }
    const Frame frame = stack.back();
    stack.pop_back();
    undo_to(frame.log_size);
    if (frame.repeat && frame.pos > frame.floor) {
      Frame lower = frame;
      lower.pos = frame.pos - 1;
      stack.push_back(lower);
    }
    pc = frame.pc;
    pos = frame.pos;
  }
}

}  // namespace microbrowser::js

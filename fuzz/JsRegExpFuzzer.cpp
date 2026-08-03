#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "js/RegExp.h"

// The regular expression compiler and matcher, fed arbitrary bytes.
//
// A pattern is written by whoever wrote the page, so the compiler's input is
// hostile in the ordinary way. The matcher's is worse: the pattern and the
// subject are *both* attacker-controlled, and the interesting failures are the
// ones that never return rather than the ones that read out of bounds.
//
// The input is split so one corpus entry drives both: the first byte is the
// flags, the rest is `pattern\0subject`.
//
// Four properties, checked rather than merely survived:
//
//   1. Compiling terminates and either succeeds or reports why. A pattern that
//      is refused must leave an error behind, because "invalid but silent" is
//      how a caller ends up matching against an empty pattern.
//   2. Matching terminates. The step budget exists for `(a+)+b`, and a fuzzer
//      finds that shape quickly; a timeout here is the bug.
//   3. Every capture lies inside the subject and is ordered. Capture offsets
//      are handed straight to substr by every caller, so an end before its
//      begin, or past the end of the string, is an out-of-bounds read one
//      frame later.
//   4. A group is either fully present or fully absent. Half a capture would
//      pass the bounds check above and still be nonsense.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) {
    return 0;
  }

  // One byte of flag bits, so the corpus reaches the flag-dependent paths --
  // `i` folds the character sets, `m` changes what `^` means, `s` changes `.`,
  // and `y` changes where the search may start.
  microbrowser::js::RegExpFlags flags;
  flags.global = (data[0] & 1u) != 0;
  flags.ignore_case = (data[0] & 2u) != 0;
  flags.multiline = (data[0] & 4u) != 0;
  flags.dot_all = (data[0] & 8u) != 0;
  flags.sticky = (data[0] & 16u) != 0;
  flags.unicode = (data[0] & 32u) != 0;

  const std::string_view rest(reinterpret_cast<const char*>(data) + 1, size - 1);
  const std::size_t split = rest.find('\0');
  const std::string_view pattern = rest.substr(0, split);
  const std::string_view subject =
      split == std::string_view::npos ? std::string_view() : rest.substr(split + 1);

  std::string error;
  const microbrowser::js::RegExp expression =
      microbrowser::js::RegExp::Compile(pattern, flags, error);
  if (!expression.IsValid()) {
    if (error.empty()) {
      __builtin_trap();  // refused without saying why
    }
    return 0;
  }

  const std::optional<microbrowser::js::RegExpMatch> match =
      expression.Exec(subject, 0, flags.sticky);
  if (!match.has_value()) {
    return 0;
  }
  if (match->GroupCount() != expression.GroupCount() + 1) {
    __builtin_trap();  // one pair per group, plus the whole match
  }
  for (std::size_t group = 0; group < match->GroupCount(); ++group) {
    const std::size_t begin = match->captures[2 * group];
    const std::size_t end = match->captures[2 * group + 1];
    const bool absent = begin == microbrowser::js::RegExpMatch::kAbsent;
    if (absent != (end == microbrowser::js::RegExpMatch::kAbsent)) {
      __builtin_trap();  // half a capture
    }
    if (absent) {
      continue;
    }
    if (begin > end || end > subject.size()) {
      __builtin_trap();
    }
    if (match->Group(subject, group) != subject.substr(begin, end - begin)) {
      __builtin_trap();
    }
  }
  return 0;
}

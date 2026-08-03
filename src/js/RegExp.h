#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::js {

struct RegExpProgram;

// The flags a pattern was written with.
struct RegExpFlags {
  bool global = false;
  bool ignore_case = false;
  bool multiline = false;
  bool dot_all = false;
  bool sticky = false;
  bool unicode = false;
  bool has_indices = false;

  // Canonical order, which is the order `RegExp.prototype.flags` reports.
  std::string Text() const;
  // Null when a character is not a flag, or when one repeats.
  static std::optional<RegExpFlags> Parse(std::string_view text);
};

// Where a match and each of its capture groups landed.
//
// Two entries per group, group 0 being the whole match. `kAbsent` marks a
// group that did not participate, which is a different answer from one that
// matched empty -- `/(a)|(b)/.exec("b")` has to distinguish them.
struct RegExpMatch {
  static constexpr std::size_t kAbsent = static_cast<std::size_t>(-1);

  std::vector<std::size_t> captures;

  std::size_t GroupCount() const { return captures.size() / 2; }
  std::size_t Begin() const { return captures.empty() ? 0 : captures[0]; }
  std::size_t End() const { return captures.size() < 2 ? 0 : captures[1]; }
  bool Participated(std::size_t group) const;
  // Empty when the group did not participate, which `Participated` separates
  // from a group that matched an empty string.
  std::string_view Group(std::string_view input, std::size_t group) const;
};

// A compiled regular expression.
//
// **Byte-oriented, like the rest of the string implementation.** `.` matches
// one byte, `[a-z]` tests one byte, and a pattern written with a non-ASCII
// literal matches that character's UTF-8 bytes in sequence. For ASCII -- which
// is all of a regex's syntax and nearly all of what a page's script matches
// against -- this is exactly right. Above U+007F it is not: a class containing
// a non-ASCII character tests its bytes individually, so `[é]` also matches
// the lead byte of another accented letter. The `u` flag is accepted and
// changes escape syntax, not the unit; making the unit a code point is the
// same change as making a JS string UTF-16, and belongs with it.
//
// A backtracking matcher over a compiled instruction list, bounded in three
// ways because a pattern is attacker-controlled: the program has a size limit,
// so `(a{100}){100}` is a SyntaxError rather than a gigabyte; the backtrack
// stack has a depth limit; and a single match has a step budget, so
// `/(a+)+b/` against a long run of `a` gives up rather than hanging. Reaching
// a bound reports "no match", which is a wrong answer that a page survives --
// unlike the hang, which it does not.
class RegExp {
 public:
  // `error` is set, and the result unusable, when `pattern` is not one.
  static RegExp Compile(std::string_view pattern, RegExpFlags flags, std::string& error);

  bool IsValid() const { return program_ != nullptr; }
  const std::string& Source() const { return source_; }
  const RegExpFlags& Flags() const { return flags_; }
  std::size_t GroupCount() const;
  // Parallel to group number, so `[0]` is the whole match and is never named.
  // Empty for a group written without a name.
  const std::vector<std::string>& GroupNames() const;
  // 0 when no group carries the name, group 0 being the whole match and
  // therefore never nameable.
  std::size_t GroupNamed(std::string_view name) const;

  // The leftmost match beginning at or after `start`. `anchored` requires it
  // to begin exactly at `start`, which is what the sticky flag means.
  std::optional<RegExpMatch> Exec(std::string_view input, std::size_t start,
                                  bool anchored) const;

 private:
  std::string source_;
  RegExpFlags flags_;
  std::shared_ptr<const RegExpProgram> program_;
};

}  // namespace microbrowser::js

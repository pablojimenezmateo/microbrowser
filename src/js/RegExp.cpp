#include "js/RegExp.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js/RegExpProgram.h"
#include "util/StringUtil.h"

// Pattern syntax, and the compiler from it to the instruction list the matcher
// runs.
//
// Parsed into a tree first and compiled second, rather than emitting while
// parsing. A quantifier is written *after* what it repeats, so a
// single-pass emitter has to splice instructions in front of the atom it
// already wrote and then repair every jump that crossed the splice. The tree
// costs one walk and removes that class of bug entirely.

namespace microbrowser::js {

namespace {

// Far past any pattern a page contains, and small enough that a pattern which
// reaches it cannot have cost much to refuse. `(a{100}){100}` is the shape
// this exists for: ten thousand instructions from eight characters.
constexpr std::size_t kMaxProgramSize = 20'000;
// Group nesting. Bounds the parser's recursion, which is otherwise chosen by
// whoever wrote the pattern.
constexpr int kMaxParseDepth = 100;
// `{n,m}` bounds. Clamped rather than rejected, because the program size limit
// is the one that decides whether the pattern is refused.
constexpr std::size_t kMaxRepeatCount = 1'000'000;
constexpr std::size_t kUnbounded = 0xFFFFFFFFu;

// --- The pattern tree ------------------------------------------------------

enum class RxKind : std::uint8_t {
  Empty,
  // A single byte, from a set. A literal, `.`, `\d` and `[a-f]` all land here:
  // they differ only in which bytes are in the set, and collapsing them means
  // the matcher has one instruction to be correct about instead of four.
  Class,
  // A set of code *points*, for `.` under `/u` and for `\p{...}`. Distinct
  // from Class because the two are matched differently -- one byte against a
  // bitmap, one whole UTF-8 sequence against a list of ranges.
  CodeClass,
  Concat,
  Alternate,
  Repeat,
  Group,
  Backref,
  Assert,
  Look,
};

struct RxNode;
using RxPtr = std::unique_ptr<RxNode>;

struct RxNode {
  RxKind kind = RxKind::Empty;
  CharSet set;
  CodeSet code_set;
  std::size_t low = 0;
  std::size_t high = 0;
  bool greedy = true;
  // 0 on a Group means non-capturing; on a Backref it is never 0, because
  // group 0 is the whole match and cannot be referred to.
  std::size_t group = 0;
  AssertKind assertion = AssertKind::Begin;
  bool negated = false;
  bool behind = false;
  std::vector<RxPtr> children;
};

RxPtr MakeNode(RxKind kind) {
  RxPtr node = std::make_unique<RxNode>();
  node->kind = kind;
  return node;
}

// One code point's bytes. Through util rather than written again here, which
// is where every other encoder in the engine went.
std::string EncodeUtf8(unsigned int code) {
  std::string out;
  util::AppendUtf8(out, code);
  return out;
}

// `i` matches either case, and the spec does it by canonicalizing both the
// input byte and the set. Closing the set over case at compile time is the
// same answer with no work at match time. Closure happens *before* negation,
// which is what makes `[^a]` with `i` also reject `A`.
void CloseOverCase(CharSet& set) {
  for (unsigned int c = 'a'; c <= 'z'; ++c) {
    if (set.Test(static_cast<unsigned char>(c))) {
      set.Add(static_cast<unsigned char>(c - 'a' + 'A'));
    }
  }
  for (unsigned int c = 'A'; c <= 'Z'; ++c) {
    if (set.Test(static_cast<unsigned char>(c))) {
      set.Add(static_cast<unsigned char>(c - 'A' + 'a'));
    }
  }
}

CharSet DigitSet() {
  CharSet set;
  set.AddRange('0', '9');
  return set;
}

CharSet WordSet() {
  CharSet set;
  set.AddRange('a', 'z');
  set.AddRange('A', 'Z');
  set.AddRange('0', '9');
  set.Add('_');
  return set;
}

// ASCII whitespace only. The spec's `\s` also covers the Unicode space
// separators, every one of which is multi-byte in UTF-8 and so cannot be a
// single-byte test -- see the note on RegExp about the unit.
CharSet SpaceSet() {
  CharSet set;
  set.Add(' ');
  set.Add('\t');
  set.Add('\n');
  set.Add('\v');
  set.Add('\f');
  set.Add('\r');
  return set;
}

CharSet AnySet(bool dot_all) {
  CharSet set;
  if (dot_all) {
    set.AddRange(0, 255);
    return set;
  }
  // Everything except a line terminator, which is what `.` means without `s`.
  set.Add('\n');
  set.Add('\r');
  set.Negate();
  return set;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// --- The parser ------------------------------------------------------------

// One character-class atom: either a set (`\d`) or a single code point.
struct ClassAtom {
  bool is_set = false;
  CharSet set;
  unsigned int code = 0;
  // Set by `\p{...}`, which names a set of code points rather than of bytes.
  bool is_code_set = false;
  CodeSet code_set;
};

class PatternParser {
 public:
  PatternParser(std::string_view pattern, const RegExpFlags& flags)
      : pattern_(pattern), flags_(flags) {
    Prescan();
    group_names_.assign(group_count_ + 1, std::string());
  }

  // Null when the pattern is not one, with `error` set.
  RxPtr Parse(std::string& error) {
    RxPtr root = ParseDisjunction(0);
    if (root != nullptr && !error_.empty()) {
      root = nullptr;
    }
    if (root != nullptr && at_ != pattern_.size()) {
      // Only a stray `)` gets here: everything else consumes or fails.
      error_ = "unmatched ) in regular expression";
      root = nullptr;
    }
    if (root == nullptr) {
      error = error_.empty() ? "invalid regular expression" : error_;
    }
    return root;
  }

  std::size_t GroupCount() const { return group_count_; }
  const std::vector<std::string>& GroupNames() const { return group_names_; }

 private:
  // Counts capture groups and collects their names before parsing, because a
  // backreference may point forward: `/\1(a)/` and `/\k<n>(?<n>a)/` are both
  // legal, and whether `\1` is a backreference or an octal escape depends on
  // how many groups the *whole* pattern has.
  void Prescan() {
    bool in_class = false;
    for (std::size_t i = 0; i < pattern_.size(); ++i) {
      const char c = pattern_[i];
      if (c == '\\') {
        ++i;
        continue;
      }
      if (in_class) {
        in_class = c != ']';
        continue;
      }
      if (c == '[') {
        in_class = true;
        continue;
      }
      if (c != '(') {
        continue;
      }
      if (i + 1 >= pattern_.size() || pattern_[i + 1] != '?') {
        ++group_count_;
        continue;
      }
      const bool named = i + 3 < pattern_.size() && pattern_[i + 2] == '<' &&
                         pattern_[i + 3] != '=' && pattern_[i + 3] != '!';
      if (!named) {
        continue;
      }
      std::string name;
      std::size_t j = i + 3;
      while (j < pattern_.size() && pattern_[j] != '>') {
        name.push_back(pattern_[j]);
        ++j;
      }
      ++group_count_;
      named_groups_.emplace(std::move(name), group_count_);
    }
  }

  bool AtEnd() const { return at_ >= pattern_.size(); }
  char Peek(std::size_t ahead = 0) const {
    return at_ + ahead < pattern_.size() ? pattern_[at_ + ahead] : '\0';
  }

  RxPtr ParseDisjunction(int depth) {
    if (depth > kMaxParseDepth) {
      error_ = "regular expression is nested too deeply";
      return nullptr;
    }
    RxPtr first = ParseAlternative(depth);
    if (first == nullptr) {
      return nullptr;
    }
    if (Peek() != '|') {
      return first;
    }
    RxPtr alternate = MakeNode(RxKind::Alternate);
    alternate->children.push_back(std::move(first));
    while (Peek() == '|') {
      ++at_;
      RxPtr next = ParseAlternative(depth);
      if (next == nullptr) {
        return nullptr;
      }
      alternate->children.push_back(std::move(next));
    }
    return alternate;
  }

  RxPtr ParseAlternative(int depth) {
    RxPtr sequence = MakeNode(RxKind::Concat);
    while (!AtEnd() && Peek() != '|' && Peek() != ')') {
      RxPtr term = ParseTerm(depth);
      if (term == nullptr) {
        return nullptr;
      }
      sequence->children.push_back(std::move(term));
    }
    if (sequence->children.size() == 1) {
      return std::move(sequence->children.front());
    }
    return sequence;
  }

  RxPtr ParseTerm(int depth) {
    RxPtr atom = ParseAtom(depth);
    if (atom == nullptr) {
      return nullptr;
    }
    std::size_t low = 0;
    std::size_t high = 0;
    if (!ReadQuantifier(low, high)) {
      return error_.empty() ? std::move(atom) : nullptr;
    }
    RxPtr repeat = MakeNode(RxKind::Repeat);
    repeat->low = low;
    repeat->high = high;
    repeat->greedy = true;
    if (Peek() == '?') {
      ++at_;
      repeat->greedy = false;
    }
    repeat->children.push_back(std::move(atom));
    return repeat;
  }

  // False when what follows is not a quantifier, which leaves `at_` alone so a
  // `{` that is not one is parsed as the literal it is.
  bool ReadQuantifier(std::size_t& low, std::size_t& high) {
    switch (Peek()) {
      case '*':
        ++at_;
        low = 0;
        high = kUnbounded;
        return true;
      case '+':
        ++at_;
        low = 1;
        high = kUnbounded;
        return true;
      case '?':
        ++at_;
        low = 0;
        high = 1;
        return true;
      case '{':
        break;
      default:
        return false;
    }

    const std::size_t start = at_;
    std::size_t cursor = at_ + 1;
    std::size_t first = 0;
    bool saw_digit = false;
    while (cursor < pattern_.size() && pattern_[cursor] >= '0' && pattern_[cursor] <= '9') {
      first = std::min(first * 10 + static_cast<std::size_t>(pattern_[cursor] - '0'),
                       kMaxRepeatCount);
      saw_digit = true;
      ++cursor;
    }
    if (!saw_digit) {
      at_ = start;
      return false;
    }
    std::size_t second = first;
    if (cursor < pattern_.size() && pattern_[cursor] == ',') {
      ++cursor;
      second = kUnbounded;
      if (cursor < pattern_.size() && pattern_[cursor] >= '0' && pattern_[cursor] <= '9') {
        second = 0;
        while (cursor < pattern_.size() && pattern_[cursor] >= '0' && pattern_[cursor] <= '9') {
          second = std::min(second * 10 + static_cast<std::size_t>(pattern_[cursor] - '0'),
                            kMaxRepeatCount);
          ++cursor;
        }
      }
    }
    if (cursor >= pattern_.size() || pattern_[cursor] != '}') {
      at_ = start;
      return false;  // `a{x` is three literals, not a malformed quantifier
    }
    if (second != kUnbounded && second < first) {
      error_ = "numbers out of order in {} quantifier";
      return false;
    }
    at_ = cursor + 1;
    low = first;
    high = second;
    return true;
  }

  RxPtr ParseAtom(int depth) {
    if (AtEnd()) {
      return MakeNode(RxKind::Empty);
    }
    const char c = pattern_[at_];
    switch (c) {
      case '^': {
        ++at_;
        RxPtr node = MakeNode(RxKind::Assert);
        node->assertion = AssertKind::Begin;
        return node;
      }
      case '$': {
        ++at_;
        RxPtr node = MakeNode(RxKind::Assert);
        node->assertion = AssertKind::End;
        return node;
      }
      case '.': {
        ++at_;
        // Under `/u`, `.` is one *code point*: an emoji is one match rather
        // than four, which is the whole reason the flag exists. Without it a
        // byte-at-a-time `.` is what every pre-ES6 engine did and what a
        // pattern written without the flag still expects.
        if (flags_.unicode) {
          RxPtr node = MakeNode(RxKind::CodeClass);
          node->code_set.negated = true;
          if (!flags_.dot_all) {
            // Everything except the four line terminators, which is what the
            // negation is for.
            node->code_set.ranges.push_back(CodeRange{'\n', '\n'});
            node->code_set.ranges.push_back(CodeRange{'\r', '\r'});
            node->code_set.ranges.push_back(CodeRange{0x2028, 0x2028});
            node->code_set.ranges.push_back(CodeRange{0x2029, 0x2029});
          }
          return node;
        }
        RxPtr node = MakeNode(RxKind::Class);
        node->set = AnySet(flags_.dot_all);
        return node;
      }
      case '*':
      case '+':
      case '?':
        error_ = "nothing to repeat";
        return nullptr;
      case '[': {
        ++at_;
        RxPtr node = MakeNode(RxKind::Class);
        if (!ParseCharacterClass(node->set)) {
          return nullptr;
        }
        return node;
      }
      case '(':
        return ParseGroup(depth);
      case '\\':
        return ParseAtomEscape();
      default:
        break;
    }
    ++at_;
    return LiteralNode(static_cast<unsigned char>(c));
  }

  // A literal is one byte, so a multi-byte character is a sequence of them.
  RxPtr LiteralByte(unsigned char byte) {
    RxPtr node = MakeNode(RxKind::Class);
    node->set.Add(byte);
    if (flags_.ignore_case) {
      CloseOverCase(node->set);
    }
    return node;
  }

  RxPtr LiteralNode(unsigned char byte) { return LiteralByte(byte); }

  RxPtr LiteralCode(unsigned int code) {
    const std::string bytes = EncodeUtf8(code);
    if (bytes.size() == 1) {
      return LiteralByte(static_cast<unsigned char>(bytes[0]));
    }
    RxPtr sequence = MakeNode(RxKind::Concat);
    for (const char byte : bytes) {
      sequence->children.push_back(LiteralByte(static_cast<unsigned char>(byte)));
    }
    return sequence;
  }

  RxPtr ParseGroup(int depth) {
    ++at_;  // '('
    RxPtr node = MakeNode(RxKind::Group);
    bool lookaround = false;
    if (Peek() == '?') {
      const char kind = Peek(1);
      if (kind == ':') {
        at_ += 2;
      } else if (kind == '=' || kind == '!') {
        at_ += 2;
        lookaround = true;
        node->kind = RxKind::Look;
        node->negated = kind == '!';
      } else if (kind == '<' && (Peek(2) == '=' || Peek(2) == '!')) {
        node->negated = Peek(2) == '!';
        at_ += 3;
        lookaround = true;
        node->kind = RxKind::Look;
        node->behind = true;
      } else if (kind == '<') {
        at_ += 2;
        std::string name;
        while (!AtEnd() && Peek() != '>') {
          name.push_back(pattern_[at_]);
          ++at_;
        }
        if (AtEnd()) {
          error_ = "invalid capture group name";
          return nullptr;
        }
        ++at_;  // '>'
        node->group = ++next_group_;
        if (node->group < group_names_.size()) {
          group_names_[node->group] = std::move(name);
        }
      } else {
        error_ = "invalid group";
        return nullptr;
      }
    } else {
      node->group = ++next_group_;
    }

    RxPtr body = ParseDisjunction(depth + 1);
    if (body == nullptr) {
      return nullptr;
    }
    if (Peek() != ')') {
      error_ = "unterminated group";
      return nullptr;
    }
    ++at_;
    node->children.push_back(std::move(body));
    if (!lookaround && node->group == 0) {
      // Non-capturing: the group node exists only to hold a body, so drop it.
      return std::move(node->children.front());
    }
    return node;
  }

  RxPtr ParseAtomEscape() {
    ++at_;  // '\'
    if (AtEnd()) {
      error_ = "trailing backslash";
      return nullptr;
    }
    const char c = pattern_[at_];
    if (c == 'b' || c == 'B') {
      ++at_;
      RxPtr node = MakeNode(RxKind::Assert);
      node->assertion = c == 'b' ? AssertKind::WordBoundary : AssertKind::NotWordBoundary;
      return node;
    }
    if (c >= '1' && c <= '9') {
      std::size_t number = 0;
      std::size_t cursor = at_;
      while (cursor < pattern_.size() && pattern_[cursor] >= '0' && pattern_[cursor] <= '9' &&
             number <= group_count_) {
        number = number * 10 + static_cast<std::size_t>(pattern_[cursor] - '0');
        ++cursor;
      }
      if (number >= 1 && number <= group_count_) {
        at_ = cursor;
        RxPtr node = MakeNode(RxKind::Backref);
        node->group = number;
        return node;
      }
      // More groups referred to than the pattern has. Annex B reads it as an
      // octal escape, which is what a page written before named groups
      // expects.
      unsigned int value = 0;
      int digits = 0;
      while (digits < 3 && !AtEnd() && Peek() >= '0' && Peek() <= '7') {
        value = value * 8 + static_cast<unsigned int>(Peek() - '0');
        ++at_;
        ++digits;
      }
      if (digits == 0) {
        ++at_;
        return LiteralNode(static_cast<unsigned char>(c));
      }
      return LiteralCode(value & 0xFF);
    }
    if (c == 'k' && !named_groups_.empty()) {
      ++at_;
      if (Peek() != '<') {
        error_ = "invalid named backreference";
        return nullptr;
      }
      ++at_;
      std::string name;
      while (!AtEnd() && Peek() != '>') {
        name.push_back(pattern_[at_]);
        ++at_;
      }
      if (AtEnd()) {
        error_ = "invalid named backreference";
        return nullptr;
      }
      ++at_;
      const auto found = named_groups_.find(name);
      if (found == named_groups_.end()) {
        error_ = "reference to a capture group that does not exist";
        return nullptr;
      }
      RxPtr node = MakeNode(RxKind::Backref);
      node->group = found->second;
      return node;
    }

    ClassAtom atom;
    if (!ReadEscape(atom, false)) {
      return nullptr;
    }
    if (atom.is_code_set) {
      RxPtr node = MakeNode(RxKind::CodeClass);
      node->code_set = std::move(atom.code_set);
      return node;
    }
    if (!atom.is_set) {
      return LiteralCode(atom.code);
    }
    RxPtr node = MakeNode(RxKind::Class);
    node->set = atom.set;
    return node;
  }

  // Shared by both contexts an escape can appear in. `in_class` decides only
  // what `\b` means: a word boundary outside a class, a backspace inside one.
  bool ReadEscape(ClassAtom& atom, bool in_class) {
    const char c = pattern_[at_];
    ++at_;
    switch (c) {
      case 'd':
      case 'D':
        atom.is_set = true;
        atom.set = DigitSet();
        if (c == 'D') {
          atom.set.Negate();
        }
        return true;
      case 'w':
      case 'W':
        atom.is_set = true;
        atom.set = WordSet();
        if (c == 'W') {
          atom.set.Negate();
        }
        return true;
      case 's':
      case 'S':
        atom.is_set = true;
        atom.set = SpaceSet();
        if (c == 'S') {
          atom.set.Negate();
        }
        return true;
      case 'p':
      case 'P': {
        // `\p{...}` is a property escape only under `/u`; without the flag it
        // is the letter p, which is what a pattern written before ES6 meant.
        if (!flags_.unicode) {
          atom.code = static_cast<unsigned char>(c);
          return true;
        }
        if (at_ >= pattern_.size() || pattern_[at_] != '{') {
          error_ = "expected '{' after \\p";
          return false;
        }
        const std::size_t close = pattern_.find('}', at_ + 1);
        if (close == std::string_view::npos) {
          error_ = "unterminated \\p{}";
          return false;
        }
        const std::string_view name = pattern_.substr(at_ + 1, close - at_ - 1);
        std::vector<CodeRange> ranges;
        if (!PropertyRanges(name, ranges)) {
          // Named rather than silently empty: a pattern that matches nothing
          // is a validator that accepts nothing, and a page would find that as
          // a rejected form rather than as a broken regex.
          error_ = "unsupported unicode property";
          return false;
        }
        at_ = close + 1;
        atom.is_code_set = true;
        atom.code_set.ranges = std::move(ranges);
        atom.code_set.negated = c == 'P';
        return true;
      }
      case 'b':
        atom.code = in_class ? 0x08 : 'b';
        return true;
      case 'n': atom.code = '\n'; return true;
      case 'r': atom.code = '\r'; return true;
      case 't': atom.code = '\t'; return true;
      case 'f': atom.code = '\f'; return true;
      case 'v': atom.code = '\v'; return true;
      case '0': atom.code = 0; return true;
      case 'c': {
        if (AtEnd()) {
          atom.code = 'c';
          return true;
        }
        const char letter = Peek();
        const bool is_letter =
            (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
        if (!is_letter) {
          atom.code = 'c';  // `\c` with no control letter is the letter itself
          return true;
        }
        ++at_;
        atom.code = static_cast<unsigned int>(letter) % 32;
        return true;
      }
      case 'x': {
        if (at_ + 1 < pattern_.size()) {
          const int high = HexValue(pattern_[at_]);
          const int low = HexValue(pattern_[at_ + 1]);
          if (high >= 0 && low >= 0) {
            at_ += 2;
            atom.code = static_cast<unsigned int>(high * 16 + low);
            return true;
          }
        }
        atom.code = 'x';
        return true;
      }
      case 'u':
        return ReadUnicodeEscape(atom);
      default:
        atom.code = static_cast<unsigned char>(c);
        return true;
    }
  }

  bool ReadUnicodeEscape(ClassAtom& atom) {
    if (flags_.unicode && Peek() == '{') {
      std::size_t cursor = at_ + 1;
      unsigned int value = 0;
      int digits = 0;
      while (cursor < pattern_.size() && HexValue(pattern_[cursor]) >= 0) {
        value = value * 16 + static_cast<unsigned int>(HexValue(pattern_[cursor]));
        if (value > 0x10FFFF) {
          error_ = "code point out of range in \\u{}";
          return false;
        }
        ++cursor;
        ++digits;
      }
      if (digits == 0 || cursor >= pattern_.size() || pattern_[cursor] != '}') {
        error_ = "invalid \\u{} escape";
        return false;
      }
      at_ = cursor + 1;
      atom.code = value;
      return true;
    }
    if (at_ + 3 >= pattern_.size()) {
      atom.code = 'u';
      return true;
    }
    unsigned int value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const int digit = HexValue(pattern_[at_ + i]);
      if (digit < 0) {
        atom.code = 'u';
        return true;
      }
      value = value * 16 + static_cast<unsigned int>(digit);
    }
    at_ += 4;
    // A surrogate pair is one character, and a page that writes an emoji in a
    // pattern writes it as two escapes.
    if (value >= 0xD800 && value <= 0xDBFF && Peek() == '\\' && Peek(1) == 'u') {
      unsigned int low = 0;
      bool complete = true;
      for (std::size_t i = 0; i < 4; ++i) {
        const int digit = at_ + 2 + i < pattern_.size() ? HexValue(pattern_[at_ + 2 + i]) : -1;
        if (digit < 0) {
          complete = false;
          break;
        }
        low = low * 16 + static_cast<unsigned int>(digit);
      }
      if (complete && low >= 0xDC00 && low <= 0xDFFF) {
        at_ += 6;
        value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
      }
    }
    atom.code = value;
    return true;
  }

  // A code point above U+007F contributes every byte of its encoding, which is
  // the byte-oriented approximation the class comment on RegExp describes.
  void AddAtomTo(CharSet& set, const ClassAtom& atom) {
    if (atom.is_set) {
      set.Union(atom.set);
      return;
    }
    for (const char byte : EncodeUtf8(atom.code)) {
      set.Add(static_cast<unsigned char>(byte));
    }
  }

  void AddRangeTo(CharSet& set, unsigned int low, unsigned int high) {
    set.AddRange(std::min(low, 0xFFu), std::min(high, 0xFFu));
    if (high > 0x7F) {
      // A range reaching above ASCII covers characters whose encodings are
      // multi-byte, and a byte-oriented set cannot express that exactly.
      set.AddRange(0x80, 0xFF);
    }
  }

  bool ParseCharacterClass(CharSet& out) {
    bool negate = false;
    if (Peek() == '^') {
      ++at_;
      negate = true;
    }
    CharSet set;
    for (;;) {
      if (AtEnd()) {
        error_ = "unterminated character class";
        return false;
      }
      if (Peek() == ']') {
        ++at_;
        break;
      }
      ClassAtom first;
      if (!ReadClassAtom(first)) {
        return false;
      }
      if (Peek() != '-' || Peek(1) == ']' || Peek(1) == '\0') {
        AddAtomTo(set, first);
        continue;
      }
      ++at_;  // '-'
      ClassAtom second;
      if (!ReadClassAtom(second)) {
        return false;
      }
      if (first.is_set || second.is_set) {
        // `[\d-z]`. Annex B reads the dash as a literal rather than refusing.
        AddAtomTo(set, first);
        set.Add('-');
        AddAtomTo(set, second);
        continue;
      }
      if (first.code > second.code) {
        error_ = "range out of order in character class";
        return false;
      }
      AddRangeTo(set, first.code, second.code);
    }
    if (flags_.ignore_case) {
      CloseOverCase(set);
    }
    if (negate) {
      set.Negate();
    }
    out = set;
    return true;
  }

  bool ReadClassAtom(ClassAtom& atom) {
    if (Peek() != '\\') {
      atom.code = static_cast<unsigned char>(pattern_[at_]);
      ++at_;
      return true;
    }
    ++at_;
    if (AtEnd()) {
      error_ = "trailing backslash";
      return false;
    }
    return ReadEscape(atom, true);
  }

  std::string_view pattern_;
  RegExpFlags flags_;
  std::size_t at_ = 0;
  std::size_t group_count_ = 0;
  std::size_t next_group_ = 0;
  std::vector<std::string> group_names_;
  std::map<std::string, std::size_t, std::less<>> named_groups_;
  std::string error_;
};

// --- The compiler ----------------------------------------------------------

void CollectGroupRange(const RxNode& node, std::size_t& low, std::size_t& high) {
  if (node.kind == RxKind::Group && node.group != 0) {
    low = std::min(low, node.group);
    high = std::max(high, node.group);
  }
  for (const RxPtr& child : node.children) {
    CollectGroupRange(*child, low, high);
  }
}

class Compiler {
 public:
  explicit Compiler(RegExpProgram& program) : program_(program) {}

  bool Compile(const RxNode& root) {
    next_register_ = 2 * (program_.group_count + 1);
    Emit(MatchOp::Save, 0);
    CompileNode(root);
    Emit(MatchOp::Save, 1);
    Emit(MatchOp::Match);
    program_.register_count = next_register_;
    if (overflow_) {
      return false;
    }
    ComputeFirstSet();
    return true;
  }

 private:
  std::size_t Emit(MatchOp op, std::uint32_t x = 0, std::uint32_t y = 0, std::uint32_t z = 0) {
    program_.code.push_back(MatchInstruction{op, x, y, z});
    if (program_.code.size() > kMaxProgramSize) {
      overflow_ = true;
    }
    return program_.code.size() - 1;
  }

  std::uint32_t AddClass(const CharSet& set) {
    const auto found = classes_.find(set.bits);
    if (found != classes_.end()) {
      return found->second;
    }
    const auto index = static_cast<std::uint32_t>(program_.classes.size());
    program_.classes.push_back(set);
    classes_.emplace(set.bits, index);
    return index;
  }

  // No interning: a pattern has a handful of these at most, and the key would
  // be a vector of ranges rather than the four words a byte set is.
  std::uint32_t AddCodeClass(const CodeSet& set) {
    const auto index = static_cast<std::uint32_t>(program_.code_classes.size());
    program_.code_classes.push_back(set);
    return index;
  }

  void CompileNode(const RxNode& node) {
    if (overflow_) {
      return;
    }
    switch (node.kind) {
      case RxKind::Empty:
        return;
      case RxKind::Class:
        Emit(MatchOp::Class, AddClass(node.set));
        return;
      case RxKind::CodeClass:
        Emit(MatchOp::CodePoint, AddCodeClass(node.code_set));
        return;
      case RxKind::Concat:
        for (const RxPtr& child : node.children) {
          CompileNode(*child);
          if (overflow_) {
            return;
          }
        }
        return;
      case RxKind::Alternate:
        CompileAlternate(node);
        return;
      case RxKind::Repeat:
        CompileRepeat(node);
        return;
      case RxKind::Group:
        Emit(MatchOp::Save, static_cast<std::uint32_t>(2 * node.group));
        CompileNode(*node.children.front());
        Emit(MatchOp::Save, static_cast<std::uint32_t>(2 * node.group + 1));
        return;
      case RxKind::Backref:
        Emit(MatchOp::Backref, static_cast<std::uint32_t>(node.group));
        return;
      case RxKind::Assert:
        Emit(MatchOp::Assert, static_cast<std::uint32_t>(node.assertion));
        return;
      case RxKind::Look:
        CompileLook(node);
        return;
    }
  }

  void CompileAlternate(const RxNode& node) {
    std::vector<std::size_t> exits;
    for (std::size_t i = 0; i + 1 < node.children.size(); ++i) {
      const std::size_t split = Emit(MatchOp::Split);
      program_.code[split].x = static_cast<std::uint32_t>(program_.code.size());
      CompileNode(*node.children[i]);
      exits.push_back(Emit(MatchOp::Jump));
      program_.code[split].y = static_cast<std::uint32_t>(program_.code.size());
      if (overflow_) {
        return;
      }
    }
    CompileNode(*node.children.back());
    const auto end = static_cast<std::uint32_t>(program_.code.size());
    for (const std::size_t exit : exits) {
      program_.code[exit].x = end;
    }
  }

  // Resets the captures inside a quantified body, which the spec does at the
  // start of every iteration: the group a previous iteration matched is not
  // one this iteration matched.
  void ClearGroupsIn(const RxNode& body) {
    std::size_t low = program_.group_count + 1;
    std::size_t high = 0;
    CollectGroupRange(body, low, high);
    if (low <= high) {
      Emit(MatchOp::Clear, static_cast<std::uint32_t>(2 * low),
           static_cast<std::uint32_t>(2 * high + 1));
    }
  }

  void CompileRepeat(const RxNode& node) {
    const RxNode& body = *node.children.front();
    // A repeated single-byte test is the overwhelmingly common case -- `\s*`,
    // `[0-9]+`, `.*` -- and the one instruction for it is what keeps a greedy
    // match over a large document from pushing one backtrack frame per byte.
    if (node.greedy && body.kind == RxKind::Class) {
      Emit(MatchOp::RepeatClass, AddClass(body.set), static_cast<std::uint32_t>(node.low),
           static_cast<std::uint32_t>(node.high));
      return;
    }

    for (std::size_t i = 0; i < node.low && !overflow_; ++i) {
      CompileNode(body);
    }
    if (overflow_) {
      return;
    }

    if (node.high == kUnbounded) {
      const auto marker = static_cast<std::uint32_t>(next_register_++);
      const std::size_t split = Emit(MatchOp::Split);
      const auto body_start = static_cast<std::uint32_t>(program_.code.size());
      Emit(MatchOp::Save, marker);
      ClearGroupsIn(body);
      CompileNode(body);
      Emit(MatchOp::Progress, marker);
      Emit(MatchOp::Jump, static_cast<std::uint32_t>(split));
      const auto end = static_cast<std::uint32_t>(program_.code.size());
      program_.code[split].x = node.greedy ? body_start : end;
      program_.code[split].y = node.greedy ? end : body_start;
      return;
    }

    std::vector<std::size_t> splits;
    for (std::size_t i = node.low; i < node.high && !overflow_; ++i) {
      const std::size_t split = Emit(MatchOp::Split);
      splits.push_back(split);
      const auto body_start = static_cast<std::uint32_t>(program_.code.size());
      (node.greedy ? program_.code[split].x : program_.code[split].y) = body_start;
      ClearGroupsIn(body);
      CompileNode(body);
    }
    const auto end = static_cast<std::uint32_t>(program_.code.size());
    for (const std::size_t split : splits) {
      (node.greedy ? program_.code[split].y : program_.code[split].x) = end;
    }
  }

  void CompileLook(const RxNode& node) {
    RegExpProgram sub;
    sub.ignore_case = program_.ignore_case;
    sub.multiline = program_.multiline;
    sub.group_count = program_.group_count;
    // The body shares the outer register file -- a positive lookahead's
    // captures are observable after it succeeds -- so it is compiled as a bare
    // body. Wrapping it in Save(0)/Save(1) the way a top-level program is
    // would overwrite the enclosing match's own extent.
    Compiler inner(sub);
    inner.next_register_ = next_register_;
    inner.CompileNode(*node.children.front());
    inner.Emit(MatchOp::Match);
    next_register_ = inner.next_register_;
    if (inner.overflow_) {
      overflow_ = true;
      return;
    }
    sub.register_count = next_register_;
    const auto index = static_cast<std::uint32_t>(program_.subs.size());
    program_.subs.push_back(std::move(sub));
    Emit(MatchOp::Look, index, node.negated ? 1u : 0u, node.behind ? 1u : 0u);
  }

  // The bytes a match can start with, when the first thing the program does is
  // demand one. Purely an optimization: getting it wrong would be a wrong
  // answer, so it gives up rather than guessing on anything it cannot read
  // straight off the front of the program.
  void ComputeFirstSet() {
    std::size_t at = 0;
    for (int steps = 0; steps < 64 && at < program_.code.size(); ++steps) {
      const MatchInstruction& instruction = program_.code[at];
      switch (instruction.op) {
        case MatchOp::Save:
        case MatchOp::Clear:
          ++at;
          continue;
        case MatchOp::Assert:
          if (static_cast<AssertKind>(instruction.x) == AssertKind::Begin &&
              !program_.multiline) {
            program_.anchored_at_start = true;
          }
          return;
        case MatchOp::Class:
          program_.has_first_set = true;
          program_.first_set = program_.classes[instruction.x];
          return;
        case MatchOp::RepeatClass:
          if (instruction.y >= 1) {
            program_.has_first_set = true;
            program_.first_set = program_.classes[instruction.x];
          }
          return;
        default:
          return;
      }
    }
  }

  RegExpProgram& program_;
  std::map<std::array<std::uint64_t, 4>, std::uint32_t> classes_;
  std::size_t next_register_ = 0;
  bool overflow_ = false;
};

}  // namespace

// --- CharSet ---------------------------------------------------------------

void CharSet::AddRange(unsigned int low, unsigned int high) {
  for (unsigned int c = low; c <= high && c <= 0xFF; ++c) {
    Add(static_cast<unsigned char>(c));
  }
}

void CharSet::Union(const CharSet& other) {
  for (std::size_t i = 0; i < bits.size(); ++i) {
    bits[i] |= other.bits[i];
  }
}

void CharSet::Negate() {
  for (std::uint64_t& word : bits) {
    word = ~word;
  }
}

bool CharSet::Empty() const {
  for (const std::uint64_t word : bits) {
    if (word != 0) {
      return false;
    }
  }
  return true;
}

// --- Flags and matches -----------------------------------------------------

std::string RegExpFlags::Text() const {
  std::string out;
  if (has_indices) out.push_back('d');
  if (global) out.push_back('g');
  if (ignore_case) out.push_back('i');
  if (multiline) out.push_back('m');
  if (dot_all) out.push_back('s');
  if (unicode) out.push_back('u');
  if (sticky) out.push_back('y');
  return out;
}

std::optional<RegExpFlags> RegExpFlags::Parse(std::string_view text) {
  RegExpFlags flags;
  for (const char c : text) {
    bool* slot = nullptr;
    switch (c) {
      case 'd': slot = &flags.has_indices; break;
      case 'g': slot = &flags.global; break;
      case 'i': slot = &flags.ignore_case; break;
      case 'm': slot = &flags.multiline; break;
      case 's': slot = &flags.dot_all; break;
      case 'u': slot = &flags.unicode; break;
      case 'v': slot = &flags.unicode; break;
      case 'y': slot = &flags.sticky; break;
      default: return std::nullopt;
    }
    if (*slot) {
      return std::nullopt;  // a repeated flag is a SyntaxError, not a no-op
    }
    *slot = true;
  }
  return flags;
}

bool RegExpMatch::Participated(std::size_t group) const {
  return 2 * group + 1 < captures.size() && captures[2 * group] != kAbsent &&
         captures[2 * group + 1] != kAbsent;
}

std::string_view RegExpMatch::Group(std::string_view input, std::size_t group) const {
  if (!Participated(group)) {
    return {};
  }
  const std::size_t begin = captures[2 * group];
  const std::size_t end = captures[2 * group + 1];
  if (begin > end || end > input.size()) {
    return {};
  }
  return input.substr(begin, end - begin);
}

// --- RegExp ----------------------------------------------------------------

RegExp RegExp::Compile(std::string_view pattern, RegExpFlags flags, std::string& error) {
  RegExp result;
  result.source_ = std::string(pattern);
  result.flags_ = flags;

  PatternParser parser(pattern, flags);
  const RxPtr root = parser.Parse(error);
  if (root == nullptr) {
    return result;
  }

  auto program = std::make_shared<RegExpProgram>();
  program->ignore_case = flags.ignore_case;
  program->multiline = flags.multiline;
  program->group_count = parser.GroupCount();
  program->group_names = parser.GroupNames();
  Compiler compiler(*program);
  if (!compiler.Compile(*root)) {
    error = "regular expression is too large";
    return result;
  }
  result.program_ = std::move(program);
  return result;
}

std::size_t RegExp::GroupCount() const {
  return program_ == nullptr ? 0 : program_->group_count;
}

const std::vector<std::string>& RegExp::GroupNames() const {
  static const std::vector<std::string> kNone;
  return program_ == nullptr ? kNone : program_->group_names;
}

std::size_t RegExp::GroupNamed(std::string_view name) const {
  if (program_ == nullptr) {
    return 0;
  }
  for (std::size_t i = 1; i < program_->group_names.size(); ++i) {
    if (program_->group_names[i] == name) {
      return i;
    }
  }
  return 0;
}

std::optional<RegExpMatch> RegExp::Exec(std::string_view input, std::size_t start,
                                        bool anchored) const {
  if (program_ == nullptr || start > input.size()) {
    return std::nullopt;
  }
  const bool single_position = anchored || program_->anchored_at_start;
  const std::size_t captures = 2 * (program_->group_count + 1);

  MatchState state;
  state.registers.assign(program_->register_count, RegExpMatch::kAbsent);

  for (std::size_t at = start; at <= input.size(); ++at) {
    if (program_->has_first_set && !single_position) {
      while (at < input.size() &&
             !program_->first_set.Test(static_cast<unsigned char>(input[at]))) {
        ++at;
      }
      if (at == input.size()) {
        break;  // nothing left that could start a match
      }
    }
    std::fill(state.registers.begin(), state.registers.end(), RegExpMatch::kAbsent);
    state.log.clear();
    if (RunProgram(*program_, input, at, static_cast<std::size_t>(-1), state, nullptr)) {
      RegExpMatch match;
      match.captures.assign(state.registers.begin(),
                            state.registers.begin() + static_cast<std::ptrdiff_t>(captures));
      return match;
    }
    if (single_position || state.steps > kMaxMatchSteps) {
      break;
    }
  }
  return std::nullopt;
}

}  // namespace microbrowser::js

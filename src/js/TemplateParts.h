#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Private to the module: the parser and the interpreter both need to take a
// template literal apart, and this is the one place that knows how.

namespace microbrowser::js {

// One escape sequence, decoded.
//
// `at` points just past the backslash on entry and just past the sequence on
// return, and the decoded text is appended to `out`. Returns false when the
// sequence is malformed -- a `\x` without two hex digits, a `\u{}` past
// U+10FFFF, a legacy octal like `\01` -- which the lexer turns into an invalid
// token. `lines` counts the line terminators a line continuation swallowed, so
// the caller can keep its line number right.
//
// Shared with the template splitter below, because `'a\nb'` and `` `a\nb` ``
// have to mean the same thing. They did not: the template path had its own
// two-line version that pushed the character after the backslash, so a
// template's `\n` was the letter n.
bool DecodeEscape(std::string_view source, std::size_t& at, std::string& out, std::size_t& lines);

// A template literal's raw source, taken apart.
//
// `literals` is one longer than `substitutions` and they interleave:
// literals[0], substitutions[0], literals[1], ... -- so `` `a${x}b` `` is
// {"a", "b"} and {"x"}, and `` `${x}${y}` `` is {"", "", ""} and {"x", "y"}.
// Escapes in the literal parts are already resolved; the substitution parts are
// raw source for the parser to re-parse, which is what keeps the nesting rules
// (a template inside a substitution inside a template) in one place.
struct TemplateParts {
  // The chunks with escapes processed: what `${}` interpolation joins.
  std::vector<std::string> literals;
  // The same chunks with escapes left alone, which is what a tagged template's
  // `.raw` is for -- `String.raw\`a\\nb\`` is five characters, and the whole
  // point of the tag is that it can see the backslash the cooked form ate.
  // One entry per literal, always, so the two index together.
  std::vector<std::string> raws;
  std::vector<std::string_view> substitutions;
};

// Splits the raw text of a template token, backticks included.
//
// One function rather than one walk in the parser and another in the
// interpreter. Those two existed and disagreed: after a substitution the
// parser resumed one character later than the interpreter did, so the `$` of an
// immediately following `${...}` fell in the gap and `` `${a}${b}` `` dropped
// its second substitution entirely. Two walks over the same syntax will drift;
// the fix is for there to be one.
TemplateParts SplitTemplate(std::string_view raw);

// Returns the index just past the closing `}` of a `${` substitution whose `{`
// is at brace_at. Skips string literals and nested templates inside.
std::size_t ScanSubstitutionEnd(std::string_view source, std::size_t brace_at);

}  // namespace microbrowser::js

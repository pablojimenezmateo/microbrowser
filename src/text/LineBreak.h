#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace microbrowser::text {

// Where a line of text may break. UAX #14, ADR 0025 §4.
//
// **The visible bug this fixes: CJK text overflows its box.** Line breaking that only breaks at
// spaces cannot break Japanese or Chinese at all, because they have none -- so a paragraph of CJK is
// one unbreakable word as wide as the paragraph is long. That is not a subtlety; it is a page that
// does not work.
//
// What this implements is the *pair table*: for each ordered pair of line-break classes, whether a
// break is prohibited, allowed, or mandatory. UAX #14 states the algorithm as numbered rules applied
// in order, and the pair table is those rules pre-composed -- which is how every implementation does
// it, because rule order is where a hand-written chain of `if`s goes wrong.
//
// **What is deliberately not here**, each with its cost stated:
//
//   * **No dictionary breaking for `SA`** -- Thai, Lao, Khmer, Myanmar. Those scripts need a
//     dictionary or a model to find word boundaries, and this treats them as ordinary letters, so
//     they break only at spaces and punctuation. A Thai paragraph therefore overflows exactly the way
//     CJK did. That is a known gap with a named blocker rather than an oversight.
//   * **No tailoring by locale.** `CJ` (small kana) is treated as `NS` -- non-starter, the strict
//     Japanese behaviour -- because that is the specification's default and the alternative needs a
//     language to choose from.
//   * **No `word-break` or `line-break` CSS properties.** They tailor this algorithm, and the
//     algorithm has to exist before it can be tailored.
struct BreakOpportunity {
  // The offset *before* which a line may break, in bytes into the text that was measured.
  std::size_t offset = 0;
  // A mandatory break -- after a newline, a paragraph separator, or U+000C. A caller must break here
  // whether or not the line is full, which is the difference between a `<br>` and a wrap.
  bool mandatory = false;
};

// Every place the text may break, in order, excluding position zero -- a break before the first
// character is not an opportunity, it is where the line already started.
//
// The input is UTF-8. Offsets are byte offsets into it, because that is what a layout engine measures
// with and converting to code point indices and back is a second place to be wrong about the same
// number.
std::vector<BreakOpportunity> FindBreakOpportunities(std::string_view utf8);

// Whether a break is allowed between two adjacent classes, ignoring context. The pair table itself,
// exposed because a caller that walks text one character at a time -- an incremental layout, a
// shaper -- needs the same answer without building a vector.
//
// `Prohibited` is the interesting value: it is what stops a line breaking between a digit and a
// comma, or inside a Hangul syllable, or before a closing bracket.
enum class BreakAction : std::uint8_t { Prohibited, Allowed, Mandatory };

BreakAction BreakBetween(std::uint32_t before, std::uint32_t after);

}  // namespace microbrowser::text

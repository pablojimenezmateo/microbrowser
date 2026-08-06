#include "text/UnicodeProperties.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace microbrowser::text {

namespace {

struct LineBreakRange {
  std::uint32_t first;
  std::uint32_t last;
  LineBreakClass value;
};

struct WidthRange {
  std::uint32_t first;
  std::uint32_t last;
  EastAsianWidth value;
};

#include "text/UnicodeTables.inc"

// A binary search over sorted, non-overlapping ranges. The properties are overwhelmingly contiguous
// -- CJK ideographs are one run of 20,992 code points -- so 2,812 ranges cover the whole of Unicode
// and a lookup is twelve comparisons. A per-code-point table would be a megabyte for the same answer.
template <typename Range, typename Value>
Value Lookup(const Range* ranges, std::size_t count, std::uint32_t code_point, Value fallback) {
  std::size_t low = 0;
  std::size_t high = count;
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (code_point < ranges[middle].first) {
      high = middle;
    } else if (code_point > ranges[middle].last) {
      low = middle + 1;
    } else {
      return ranges[middle].value;
    }
  }
  return fallback;
}

}  // namespace

LineBreakClass LineBreakClassOf(std::uint32_t code_point) {
  // `AL` for anything the table does not cover, which is what UAX #14 says an unassigned code point
  // is: an ordinary letter. Wrong in the same direction the specification is wrong, rather than in a
  // direction of this implementation's own.
  return Lookup(kLineBreakRanges, std::size(kLineBreakRanges), code_point, LineBreakClass::AL);
}

EastAsianWidth WidthOf(std::uint32_t code_point) {
  return Lookup(kWideRanges, std::size(kWideRanges), code_point, EastAsianWidth::Neutral);
}

bool IsDoubleWidth(std::uint32_t code_point) {
  const EastAsianWidth width = WidthOf(code_point);
  return width == EastAsianWidth::Wide || width == EastAsianWidth::Fullwidth;
}

}  // namespace microbrowser::text

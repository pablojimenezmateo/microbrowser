#pragma once

// Shared between TreeBuilder.cpp and TreeBuilderTable.cpp, and private to the
// module — it is not on `public:` in MODULE.deps, because nothing outside the
// parser has any business knowing which tags the spec calls table structure.
//
// The split exists because the table insertion modes are half the tree builder
// by line count and are read against a different half of the spec (§13.2.6.4.9
// through §13.2.6.4.17) from the rest of it.

#include <algorithm>
#include <array>
#include <string_view>

namespace microbrowser::html {

inline bool IsWhitespaceOnly(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) {
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
  });
}

inline bool Contains(const auto& list, std::string_view value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

// Everything a table start tag may open, which is also the set that an
// unexpected one of them closes its way out to.
inline constexpr std::array<std::string_view, 9> kTableStructureTags = {
    "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"};

inline constexpr std::array<std::string_view, 8> kSelectTableTags = {
    "caption", "table", "tbody", "tfoot", "thead", "tr", "td", "th"};

// True for the elements whose children must be table structure, and into which
// text and stray elements are therefore never inserted directly.
inline bool IsFosterParent(std::string_view tag_name) {
  return tag_name == "table" || tag_name == "tbody" || tag_name == "tfoot" ||
         tag_name == "thead" || tag_name == "tr";
}

}  // namespace microbrowser::html

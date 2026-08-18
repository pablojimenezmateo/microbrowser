// CSS Text: the `white-space` family.
//
// Its own translation unit for the reason TransformDeclarations.cpp is one -- `white-space` is not
// a keyword switch. CSS Text 4 makes it a **shorthand** over `white-space-collapse` and
// `text-wrap-mode`, whose two components may be written in either order and either of which may be
// omitted, and it has six single-keyword forms on top of that which are not simply pairs of the
// longhand keywords. Folded into ApplyDeclaration's chain it would have been the one branch there
// that has a grammar rather than a list.
//
// The model itself is in ComputedStyle.h: two fields, because one enum of four values could not say
// `preserve-breaks nowrap` and could not say that `normal` and `nowrap` *collapse identically*.

#include <optional>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"

namespace microbrowser::css {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// The value split on whitespace. A local copy rather than Declarations.cpp's, which is in that
// file's anonymous namespace; the alternative is publishing a string utility from a header whose
// subject is the cascade.
std::vector<std::string_view> Words(std::string_view value) {
  std::vector<std::string_view> words;
  std::size_t at = 0;
  while (at < value.size()) {
    while (at < value.size() && IsSpace(value[at])) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < value.size() && !IsSpace(value[at])) {
      ++at;
    }
    if (at > begin) {
      words.push_back(value.substr(begin, at - begin));
    }
  }
  return words;
}

// The two longhands `white-space` is a shorthand over. Separate functions rather than branches
// inside the shorthand, because the shorthand has to ask "is this word a collapse value?" of each
// of its components independently -- the order is free.
std::optional<WhiteSpaceCollapse> ParseWhiteSpaceCollapse(std::string_view value) {
  if (value == "collapse") return WhiteSpaceCollapse::Collapse;
  if (value == "preserve") return WhiteSpaceCollapse::Preserve;
  if (value == "preserve-breaks") return WhiteSpaceCollapse::PreserveBreaks;
  if (value == "preserve-spaces") return WhiteSpaceCollapse::PreserveSpaces;
  if (value == "break-spaces") return WhiteSpaceCollapse::BreakSpaces;
  return std::nullopt;
}

std::optional<TextWrapMode> ParseTextWrapMode(std::string_view value) {
  if (value == "wrap") return TextWrapMode::Wrap;
  if (value == "nowrap") return TextWrapMode::NoWrap;
  return std::nullopt;
}

}  // namespace

bool ApplyTextDeclaration(std::string_view property, std::string_view value,
                          const ComputedStyle& parent, ComputedStyle& style,
                          const MediaContext& context) {
  (void)parent;
  (void)context;

  if (property == "white-space-collapse") {
    const std::optional<WhiteSpaceCollapse> collapse = ParseWhiteSpaceCollapse(value);
    if (!collapse.has_value()) {
      return false;
    }
    style.white_space_collapse = *collapse;
    return true;
  }

  if (property == "text-wrap-mode") {
    const std::optional<TextWrapMode> mode = ParseTextWrapMode(value);
    if (!mode.has_value()) {
      return false;
    }
    style.text_wrap_mode = *mode;
    return true;
  }

  if (property == "white-space") {
    // Two shapes: one of the four keywords that are not simply a pair of longhand values, or the
    // longhand values themselves in either order. `preserve-breaks nowrap` is a pair no keyword
    // names and is still valid, which is why this cannot be a table of six names.
    WhiteSpaceCollapse collapse = WhiteSpaceCollapse::Collapse;
    TextWrapMode mode = TextWrapMode::Wrap;
    if (value == "normal") {
      // The initial pair.
    } else if (value == "pre") {
      collapse = WhiteSpaceCollapse::Preserve;
      mode = TextWrapMode::NoWrap;
    } else if (value == "pre-wrap") {
      collapse = WhiteSpaceCollapse::Preserve;
    } else if (value == "pre-line") {
      collapse = WhiteSpaceCollapse::PreserveBreaks;
    } else {
      const std::vector<std::string_view> parts = Words(value);
      if (parts.empty() || parts.size() > 2) {
        return false;
      }
      bool saw_collapse = false;
      bool saw_mode = false;
      for (const std::string_view part : parts) {
        if (const std::optional<WhiteSpaceCollapse> one = ParseWhiteSpaceCollapse(part)) {
          if (saw_collapse) {
            return false;
          }
          saw_collapse = true;
          collapse = *one;
          continue;
        }
        if (const std::optional<TextWrapMode> one = ParseTextWrapMode(part)) {
          if (saw_mode) {
            return false;
          }
          saw_mode = true;
          mode = *one;
          continue;
        }
        return false;
      }
    }
    style.white_space_collapse = collapse;
    style.text_wrap_mode = mode;
    return true;
  }

  return false;
}

}  // namespace microbrowser::css

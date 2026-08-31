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

#include <cstdlib>
#include <optional>
#include <string>
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

// `text-transform`: a case keyword and two width keywords, in any order, at most one of each.
std::optional<TextTransform> ParseTextTransform(std::string_view value) {
  TextTransform out;
  if (value == "none") {
    return out;
  }
  bool saw_case = false;
  const std::vector<std::string_view> parts = Words(value);
  if (parts.empty() || parts.size() > 3) {
    return std::nullopt;
  }
  for (const std::string_view part : parts) {
    if (part == "capitalize" || part == "uppercase" || part == "lowercase") {
      if (saw_case) {
        return std::nullopt;
      }
      saw_case = true;
      out.letter_case = part == "capitalize" ? TextCase::Capitalize
                        : part == "uppercase" ? TextCase::Uppercase
                                              : TextCase::Lowercase;
      continue;
    }
    if (part == "full-width") {
      if (out.full_width) {
        return std::nullopt;
      }
      out.full_width = true;
      continue;
    }
    if (part == "full-size-kana") {
      if (out.full_size_kana) {
        return std::nullopt;
      }
      out.full_size_kana = true;
      continue;
    }
    return std::nullopt;
  }
  return out;
}

// A bare `<number>`: no unit, no sign required, and it must consume the whole word. `util::ParseInt`
// is the wrong tool twice over -- it rejects a fraction and it accepts trailing text.
std::optional<float> ParseNumber(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  const std::string text(value);
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<float>(parsed);
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

  if (property == "text-wrap") {
    // A shorthand over `text-wrap-mode` and `text-wrap-style`. Only the mode half exists here, so
    // `balance`, `pretty` and `stable` are refused rather than accepted and ignored -- a page that
    // asks `CSS.supports('text-wrap', 'balance')` gets an honest no, and gets a polyfill instead
    // of a wall (ADR 0012).
    const std::optional<TextWrapMode> mode = ParseTextWrapMode(value);
    if (!mode.has_value()) {
      return false;
    }
    style.text_wrap_mode = *mode;
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

  if (property == "text-transform") {
    const std::optional<TextTransform> transform = ParseTextTransform(value);
    if (!transform.has_value()) {
      return false;
    }
    style.text_transform = *transform;
    return true;
  }

  if (property == "word-break") {
    if (value == "normal") {
      style.word_break = WordBreak::Normal;
    } else if (value == "break-all") {
      style.word_break = WordBreak::BreakAll;
    } else if (value == "keep-all") {
      style.word_break = WordBreak::KeepAll;
    } else if (value == "break-word") {
      // The legacy spelling of `overflow-wrap: break-word`. It computes to itself -- the
      // computed value of `word-break` is what the page wrote -- and behaves as the other
      // property, which is why both fields are written here.
      style.word_break = WordBreak::BreakWord;
      style.overflow_wrap = OverflowWrap::BreakWord;
    } else {
      return false;
    }
    return true;
  }

  // `word-wrap` is the original name and is an alias rather than a separate property: the two
  // share one computed value, and a page that sets one and reads the other must see it.
  if (property == "overflow-wrap" || property == "word-wrap") {
    if (value == "normal") {
      style.overflow_wrap = OverflowWrap::Normal;
    } else if (value == "break-word") {
      style.overflow_wrap = OverflowWrap::BreakWord;
    } else if (value == "anywhere") {
      style.overflow_wrap = OverflowWrap::Anywhere;
    } else {
      return false;
    }
    return true;
  }

  if (property == "text-indent") {
    TextIndent indent;
    bool saw_length = false;
    const std::vector<std::string_view> parts = Words(value);
    if (parts.empty() || parts.size() > 3) {
      return false;
    }
    for (const std::string_view part : parts) {
      if (part == "hanging") {
        if (indent.hanging) {
          return false;
        }
        indent.hanging = true;
        continue;
      }
      if (part == "each-line") {
        if (indent.each_line) {
          return false;
        }
        indent.each_line = true;
        continue;
      }
      if (saw_length) {
        return false;
      }
      const std::optional<Length> length = ParseLength(part, context, style.root_font_size);
      // `auto` is a length this parser produces and `text-indent` has no such value.
      if (!length.has_value() || length->IsAuto()) {
        return false;
      }
      saw_length = true;
      indent.length = *length;
    }
    if (!saw_length) {
      return false;  // the length is required; the two keywords are not
    }
    style.text_indent = indent;
    return true;
  }

  if (property == "letter-spacing" || property == "word-spacing") {
    // `normal` is the initial value of both and computes to no extra advance. Spelling it as a zero
    // length rather than a keyword is what lets the used value be one `Resolve` at the point a font
    // is asked for, with no third state for the two call sites to disagree about.
    Length spacing;
    if (value != "normal") {
      const std::optional<Length> length = ParseLength(value, context, style.root_font_size);
      // A percentage is legal on both (css-text-4) and is a fraction of *this element's* font size,
      // which is the basis `FontRequestFor` hands `Used`. `auto` is a length this parser produces
      // and neither property has such a value.
      if (!length.has_value() || length->IsAuto()) {
        return false;
      }
      spacing = *length;
    }
    (property == "letter-spacing" ? style.letter_spacing : style.word_spacing) = spacing;
    return true;
  }

  if (property == "tab-size") {
    if (const std::optional<float> number = ParseNumber(value)) {
      if (*number < 0.0f) {
        return false;
      }
      style.tab_size = TabSize{*number, false};
      return true;
    }
    const std::optional<Length> length = ParseLength(value, context, style.root_font_size);
    if (!length.has_value() || length->IsAuto() || length->IsPercent()) {
      return false;
    }
    const float pixels = length->Resolve(style.font_size, 0.0f);
    if (pixels < 0.0f) {
      return false;
    }
    style.tab_size = TabSize{pixels, true};
    return true;
  }

  return false;
}

}  // namespace microbrowser::css

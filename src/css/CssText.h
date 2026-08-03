#pragma once

#include <algorithm>
#include <string>
#include <string_view>

// The three text operations every CSS value goes through before anything else
// looks at it. Private to the module and inline, because they are shared by the
// declaration parser and the cascade and duplicating them in two translation
// units is how "is this value trimmed yet" gets two answers.

namespace microbrowser::css {

inline char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

// CSS whitespace is a shorter list than the C library's isspace: no vertical
// tab. A tokenizer that accepted one would split a value the parser then reads
// back as a different value.
inline bool IsCssWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

inline std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsCssWhitespace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsCssWhitespace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace microbrowser::css

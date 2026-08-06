#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The text operations every CSS value goes through before anything else looks
// at it. Private to the module and inline, because they are shared by the
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

// `text` lower-cased, without allocating when it already is.
//
// A CSS value is nearly always written in lower case, so the common answer is a
// view of the caller's own bytes and no copy at all. `storage` is where the
// uncommon answer lives, and it must outlive the returned view -- which is why
// it is a parameter rather than something this function owns.
//
// The scan is not free, but it is a pass over bytes already in cache against an
// allocation, a copy and a free. Applying one declaration used to do the second
// of those unconditionally, 393,210 times per load of en.wikipedia.org/wiki/CSS.
inline std::string_view LoweredIfNeeded(std::string_view text, std::string& storage) {
  for (const char c : text) {
    if (c >= 'A' && c <= 'Z') {
      storage = Lowered(text);
      return storage;
    }
  }
  return text;
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

// The space-separated components of a value -- the `10px solid red` of a
// shorthand -- with whitespace *inside* a function or a string left alone.
//
// The nesting rule is what makes `margin: calc(1px + 2px) 0` two components
// rather than five. Every shorthand in this module counts its components and
// rejects the wrong number, so a splitter that cut a `calc()` into pieces did
// not produce a slightly wrong value; it dropped the declaration.
inline std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t depth = 0;
  char quote = '\0';
  std::size_t begin = std::string_view::npos;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    const bool separates = quote == '\0' && depth == 0 && IsCssWhitespace(c);
    if (quote != '\0') {
      quote = c == quote ? '\0' : quote;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && depth > 0) {
      --depth;
    }
    if (separates) {
      if (begin != std::string_view::npos) {
        words.push_back(text.substr(begin, i - begin));
        begin = std::string_view::npos;
      }
    } else if (begin == std::string_view::npos) {
      begin = i;
    }
  }
  return words;
}

}  // namespace microbrowser::css

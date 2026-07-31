#pragma once

#include <cstddef>
#include <string_view>

namespace microbrowser::util {

namespace detail {

constexpr char AsciiToLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool EqualsAsciiCaseInsensitive(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (AsciiToLower(a[i]) != AsciiToLower(b[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

// ASCII-only case folding. Deliberately not locale- or Unicode-aware: every
// caller here is matching a protocol token, an env value, or an HTML/CSS
// keyword, all of which the relevant specs define as ASCII case-insensitive.
// Unicode case folding is a different operation and gets a different function
// when something actually needs it.
[[nodiscard]] constexpr bool EqualsAsciiCaseInsensitive(std::string_view a, std::string_view b) {
  return detail::EqualsAsciiCaseInsensitive(a, b);
}

// The "explicitly falsey" env/config token set.
[[nodiscard]] constexpr bool IsFalseyToken(std::string_view text) {
  if (text.size() > 5) {
    return false;  // no falsey token is longer than "false"
  }
  return detail::EqualsAsciiCaseInsensitive(text, "0") ||
         detail::EqualsAsciiCaseInsensitive(text, "false") ||
         detail::EqualsAsciiCaseInsensitive(text, "no") ||
         detail::EqualsAsciiCaseInsensitive(text, "off");
}

// The matching "explicitly truthy" set. Note this is NOT `!IsFalseyToken`: a
// value like a file path is neither, and callers that accept an arbitrary
// payload need to tell the two apart.
[[nodiscard]] constexpr bool IsTruthyToken(std::string_view text) {
  if (text.size() > 4) {
    return false;
  }
  return detail::EqualsAsciiCaseInsensitive(text, "1") ||
         detail::EqualsAsciiCaseInsensitive(text, "true") ||
         detail::EqualsAsciiCaseInsensitive(text, "yes") ||
         detail::EqualsAsciiCaseInsensitive(text, "on");
}

[[nodiscard]] constexpr bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] constexpr bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Strip leading/trailing ASCII whitespace. Returns a view into `text`.
[[nodiscard]] constexpr std::string_view TrimAscii(std::string_view text) {
  constexpr std::string_view kSpace = " \t\r\n\f\v";
  const std::size_t begin = text.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) {
    return {};
  }
  const std::size_t end = text.find_last_not_of(kSpace);
  return text.substr(begin, end - begin + 1);
}

}  // namespace microbrowser::util

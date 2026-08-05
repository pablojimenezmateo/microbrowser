#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

[[nodiscard]] constexpr bool StartsWithAsciiCaseInsensitive(std::string_view text,
                                                            std::string_view prefix) {
  return text.size() >= prefix.size() &&
         detail::EqualsAsciiCaseInsensitive(text.substr(0, prefix.size()), prefix);
}

// The one ASCII lower-caser. Five modules had written their own -- `gfx` twice,
// `privacy` twice, `bindings` once -- which is five chances to disagree about
// what happens to a byte above 0x7F, and every one of those callers is folding
// something a page supplied. Deliberately ASCII-only for the reason
// EqualsAsciiCaseInsensitive is: a protocol token, a tag name and a CSS keyword
// are all defined as ASCII case-insensitive, and Unicode case folding is a
// different operation that gets a different function when something needs it.
[[nodiscard]] inline std::string AsciiLowerCase(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = detail::AsciiToLower(c);
  }
  return out;
}

[[nodiscard]] constexpr bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool IsAsciiAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr bool IsAsciiAlphanumeric(char c) {
  return IsAsciiDigit(c) || IsAsciiAlpha(c);
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

// One hexadecimal digit's value, or -1. Here because three parsers wanted it.
[[nodiscard]] constexpr int HexDigit(char c) {
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

// --- UTF-8 ------------------------------------------------------------------
//
// Here rather than in each caller. Four files in `js` alone had written their
// own encoder, which is four chances to disagree about what a lone surrogate
// or an over-long sequence means -- and every one of those files is decoding
// bytes a page supplied.

// Appends `code` as UTF-8. Anything outside the Unicode range, and the
// surrogate range itself, becomes U+FFFD: a page can name one, and a lone
// surrogate written straight through is invalid UTF-8 that every consumer
// downstream would then have to cope with.
inline void AppendUtf8(std::string& out, std::uint32_t code) {
  if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
    code = 0xFFFDu;
  }
  if (code < 0x80u) {
    out.push_back(static_cast<char>(code));
    return;
  }
  if (code < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (code >> 6)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    return;
  }
  if (code < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (code >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    return;
  }
  out.push_back(static_cast<char>(0xF0u | (code >> 18)));
  out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
}

// Decodes the sequence beginning at `at`, advancing it past what it consumed.
// False when the bytes there are not a well-formed sequence, in which case
// `at` is left where it was -- the caller decides what a malformed byte means,
// because the answer differs between a lexer and a string method.
inline bool DecodeUtf8(std::string_view text, std::size_t& at, std::uint32_t& code) {
  if (at >= text.size()) {
    return false;
  }
  const auto lead = static_cast<unsigned char>(text[at]);
  std::size_t extra = 0;
  std::uint32_t value = 0;
  if (lead < 0x80u) {
    code = lead;
    ++at;
    return true;
  }
  if ((lead & 0xE0u) == 0xC0u) {
    extra = 1;
    value = lead & 0x1Fu;
  } else if ((lead & 0xF0u) == 0xE0u) {
    extra = 2;
    value = lead & 0x0Fu;
  } else if ((lead & 0xF8u) == 0xF0u) {
    extra = 3;
    value = lead & 0x07u;
  } else {
    return false;  // a continuation byte, or a lead byte no encoding produces
  }
  if (at + extra >= text.size() + 0 && at + extra > text.size() - 1) {
    return false;
  }
  for (std::size_t i = 1; i <= extra; ++i) {
    const auto byte = static_cast<unsigned char>(text[at + i]);
    if ((byte & 0xC0u) != 0x80u) {
      return false;
    }
    value = (value << 6) | (byte & 0x3Fu);
  }
  at += extra + 1;
  code = value;
  return true;
}

}  // namespace microbrowser::util

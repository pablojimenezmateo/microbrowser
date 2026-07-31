#include "util/Parse.h"

#include <charconv>
#include <cmath>

namespace microbrowser::util {

namespace {

template <typename T>
std::optional<T> ParseExact(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  T value{};
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

// Real-number parsing goes through std::from_chars, NOT strto*.
//
// strtod/strtof read the decimal separator from LC_NUMERIC, and the process
// locale is not ours to control: SDL's X11 toolkit and hidapi backends call
// `setlocale(LC_ALL, "")` / `setlocale(LC_CTYPE, "")` behind our back. Under a
// comma-decimal locale every "1.5" in persisted state, a settings file, or an
// LSP/DAP payload would stop parsing at the '.' and be rejected. from_chars is
// defined to be locale-independent, and is also allocation-free — the old path
// copied every token into a null-terminated std::string just to call strto*.
//
// from_chars additionally gives the strictness the old path hand-rolled for
// free: it rejects a leading space, a leading '+', and hex ("0x10" stops at the
// 'x', leaving the parse short of the end).
template <typename T>
std::optional<T> ParseRealExact(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  const char* const begin = text.data();
  const char* const end = begin + text.size();

  T value{};
  const auto [ptr, ec] = std::from_chars(begin, end, value, std::chars_format::general);
  if (ptr != end) {
    return std::nullopt;  // trailing garbage, or nothing consumed
  }
  if (ec == std::errc::result_out_of_range) {
    // from_chars collapses overflow and underflow into one code and leaves
    // `value` untouched, but the two must resolve differently — the strto*
    // behavior this replaced accepted underflow (a legitimately tiny magnitude
    // rounds to a subnormal or ±0) and rejected overflow. Re-parse through the
    // wider type to tell them apart: an underflowing literal narrows to ±0 and
    // an overflowing one narrows to ±inf, which the finiteness check below
    // rejects. Only the out-of-range path pays this; the common case above is
    // parsed directly into T and so stays correctly rounded (no double
    // rounding through long double).
    long double wide{};
    const auto wide_result = std::from_chars(begin, end, wide, std::chars_format::general);
    if (wide_result.ec != std::errc{} || wide_result.ptr != end) {
      return std::nullopt;  // out of range even for long double
    }
    value = static_cast<T>(wide);
  } else if (ec != std::errc{}) {
    return std::nullopt;
  }
  // "inf"/"nan" parse successfully but are not values any caller can use.
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::optional<int> ParseInt(std::string_view text) {
  return ParseExact<int>(text);
}

std::optional<std::int64_t> ParseInt64(std::string_view text) {
  return ParseExact<std::int64_t>(text);
}

std::optional<std::size_t> ParseSize(std::string_view text) {
  return ParseExact<std::size_t>(text);
}

std::optional<float> ParseFloat(std::string_view text) {
  return ParseRealExact<float>(text);
}

std::optional<double> ParseDouble(std::string_view text) {
  return ParseRealExact<double>(text);
}

int ParseIntOr(const std::optional<std::string>& text, int fallback) {
  return text.has_value() ? ParseInt(*text).value_or(fallback) : fallback;
}

}  // namespace microbrowser::util

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::util {

std::optional<int> ParseInt(std::string_view text);
std::optional<std::int64_t> ParseInt64(std::string_view text);
std::optional<std::size_t> ParseSize(std::string_view text);
std::optional<float> ParseFloat(std::string_view text);
std::optional<double> ParseDouble(std::string_view text);

// Parse an optional setting string, returning `fallback` when the value is absent
// or fails to parse. Collapses the common `raw.has_value() ? ParseInt(*raw).value_or(D)
// : D` idiom used by the settings-backed integer knobs.
int ParseIntOr(const std::optional<std::string>& text, int fallback);

// HTML's "rules for parsing non-negative integers", which is a *different* function from
// `ParseInt` and deliberately so: this one skips leading whitespace, accepts a leading `+`, and
// **stops at the first non-digit** rather than rejecting the string. `width="100em"` is a hundred
// in every browser and nothing to `ParseInt`.
//
// Here rather than in `src/bindings`, which owns the reflected-attribute algorithms, because the
// *same* attribute is read twice more outside that module: `src/css` maps `<canvas width>` to a
// presentational hint and `src/engine` sizes the backing store from it, and three transcriptions of
// one paragraph is how `canvas.width` ends up reporting a number the canvas is not.
std::optional<std::int64_t> ParseHtmlNonNegativeInteger(std::string_view text);

}  // namespace microbrowser::util

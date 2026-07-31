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

}  // namespace microbrowser::util

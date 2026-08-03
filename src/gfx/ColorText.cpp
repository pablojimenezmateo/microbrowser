#include "gfx/ColorText.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/Parse.h"

namespace microbrowser::gfx {

namespace {

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsSpace(text[start])) {
      ++start;
    }
    std::size_t end = start;
    while (end < text.size() && !IsSpace(text[end])) {
      ++end;
    }
    if (end > start) {
      words.push_back(text.substr(start, end - start));
    }
    start = end;
  }
  return words;
}

struct NamedColor {
  std::string_view name;
  std::uint32_t argb;
};

// The colours a real page actually uses by name. The full CSS list is 148
// entries of generated data; growing this is a data change.
constexpr std::array<NamedColor, 22> kNamedColors = {{
    {"black", 0xFF000000},   {"white", 0xFFFFFFFF},   {"red", 0xFFFF0000},
    {"green", 0xFF008000},   {"blue", 0xFF0000FF},    {"yellow", 0xFFFFFF00},
    {"cyan", 0xFF00FFFF},    {"magenta", 0xFFFF00FF}, {"gray", 0xFF808080},
    {"grey", 0xFF808080},    {"silver", 0xFFC0C0C0},  {"maroon", 0xFF800000},
    {"olive", 0xFF808000},   {"lime", 0xFF00FF00},    {"aqua", 0xFF00FFFF},
    {"teal", 0xFF008080},    {"navy", 0xFF000080},    {"fuchsia", 0xFFFF00FF},
    {"purple", 0xFF800080},  {"orange", 0xFFFFA500},  {"transparent", 0x00000000},
    {"currentcolor", 0xFF000000},
}};

int HexValue(char c) {
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

std::optional<std::uint8_t> ParseRgbChannel(std::string_view text) {
  if (text.ends_with('%')) {
    const std::optional<double> percent = util::ParseDouble(text.substr(0, text.size() - 1));
    if (!percent.has_value()) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::clamp(*percent, 0.0, 100.0) * 255.0 / 100.0 + 0.5);
  }
  const std::optional<double> value = util::ParseDouble(text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(std::clamp(*value, 0.0, 255.0) + 0.5);
}

std::optional<std::uint8_t> ParseAlphaChannel(std::string_view text) {
  if (text.ends_with('%')) {
    const std::optional<double> percent = util::ParseDouble(text.substr(0, text.size() - 1));
    if (!percent.has_value()) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::clamp(*percent, 0.0, 100.0) * 255.0 / 100.0 + 0.5);
  }
  const std::optional<double> value = util::ParseDouble(text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(std::clamp(*value, 0.0, 1.0) * 255.0 + 0.5);
}

}  // namespace

std::optional<Color> ParseColorText(std::string_view text) {
  const std::string lowered = Lowered(Trim(text));
  if (lowered.empty()) {
    return std::nullopt;
  }

  if (lowered.front() == '#') {
    const std::string_view digits(lowered.data() + 1, lowered.size() - 1);
    const auto nibble = [&digits](std::size_t index) {
      return static_cast<std::uint32_t>(HexValue(digits[index]));
    };
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return HexValue(c) >= 0; })) {
      return std::nullopt;
    }
    if (digits.size() == 3) {
      // `#abc` is `#aabbcc` — each digit doubled, not shifted, so `#fff` is
      // exactly white rather than 0xF0F0F0.
      return Color::Rgb(static_cast<std::uint8_t>(nibble(0) * 17),
                             static_cast<std::uint8_t>(nibble(1) * 17),
                             static_cast<std::uint8_t>(nibble(2) * 17));
    }
    if (digits.size() == 6) {
      return Color::Rgb(static_cast<std::uint8_t>(nibble(0) * 16 + nibble(1)),
                             static_cast<std::uint8_t>(nibble(2) * 16 + nibble(3)),
                             static_cast<std::uint8_t>(nibble(4) * 16 + nibble(5)));
    }
    if (digits.size() == 8) {
      return Color::Rgba(static_cast<std::uint8_t>(nibble(0) * 16 + nibble(1)),
                              static_cast<std::uint8_t>(nibble(2) * 16 + nibble(3)),
                              static_cast<std::uint8_t>(nibble(4) * 16 + nibble(5)),
                              static_cast<std::uint8_t>(nibble(6) * 16 + nibble(7)));
    }
    return std::nullopt;
  }

  if (lowered.rfind("rgb(", 0) == 0 || lowered.rfind("rgba(", 0) == 0) {
    const std::size_t open = lowered.find('(');
    const std::size_t close = lowered.rfind(')');
    if (close == std::string::npos || close < open) {
      return std::nullopt;
    }
    std::string body = lowered.substr(open + 1, close - open - 1);
    std::replace(body.begin(), body.end(), ',', ' ');
    const std::vector<std::string_view> parts = SplitWords(body);
    if (parts.size() != 3 && parts.size() != 4) {
      return std::nullopt;
    }
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t i = 0; i < 3; ++i) {
      const std::optional<std::uint8_t> channel = ParseRgbChannel(parts[i]);
      if (!channel.has_value()) {
        return std::nullopt;
      }
      channels[i] = *channel;
    }
    std::uint8_t alpha = 255;
    if (parts.size() == 4) {
      const std::optional<std::uint8_t> parsed_alpha = ParseAlphaChannel(parts[3]);
      if (!parsed_alpha.has_value()) {
        return std::nullopt;
      }
      alpha = *parsed_alpha;
    }
    return Color::Rgba(channels[0], channels[1], channels[2], alpha);
  }

  for (const NamedColor& named : kNamedColors) {
    if (named.name == lowered) {
      return Color{named.argb};
    }
  }
  return std::nullopt;
}

}  // namespace microbrowser::gfx

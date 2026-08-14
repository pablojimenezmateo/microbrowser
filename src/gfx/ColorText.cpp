#include "gfx/ColorText.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::gfx {

namespace {


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

// --- The colour functions ------------------------------------------------------------------
//
// CSS Color 4 gives `rgb()` and `hsl()` two grammars and they are **not** interchangeable:
//
//   legacy   rgb(1, 2, 3)        rgb(1, 2, 3, 0.5)      commas throughout, no `none`
//   modern   rgb(1 2 3)          rgb(1 2 3 / 0.5)       spaces, a slash before alpha, `none` allowed
//
// Mixing them -- `rgb(1, 2, 3 / 0.5)` -- is invalid, and telling the two apart by whether the body
// contains a comma is what makes that fall out rather than needing a rule of its own. The parser
// used to replace every comma with a space and split, which accepted the mixed form and rejected
// every modern one with an alpha, because `/` came back as a fourth component.

// One component of a colour function: a number, a percentage, or `none`.
//
// `none` is CSS Color 4's missing component. It behaves as zero everywhere this browser can see --
// the difference only shows in interpolation and in the `color()` function's carry-forward rule,
// neither of which exists here -- so it is accepted and read as zero rather than refused.
struct Component {
  double value = 0.0;
  bool is_percent = false;
  bool is_none = false;
};

std::optional<Component> ParseComponent(std::string_view text) {
  Component component;
  if (text == "none") {
    component.is_none = true;
    return component;
  }
  if (text.ends_with('%')) {
    const std::optional<double> percent = util::ParseDouble(text.substr(0, text.size() - 1));
    if (!percent.has_value()) {
      return std::nullopt;
    }
    component.value = *percent;
    component.is_percent = true;
    return component;
  }
  const std::optional<double> value = util::ParseDouble(text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  component.value = *value;
  return component;
}

// A hue: a number of degrees, or an angle with one of the four units. Normalised into [0, 360) here
// rather than at the conversion, because `hsl(-300deg …)` and `hsl(60deg …)` are the same colour and
// a test asserts exactly that.
std::optional<double> ParseHue(std::string_view text) {
  if (text == "none") {
    return 0.0;
  }
  double scale = 1.0;
  if (text.ends_with("deg")) {
    text.remove_suffix(3);
  } else if (text.ends_with("grad")) {
    text.remove_suffix(4);
    scale = 360.0 / 400.0;
  } else if (text.ends_with("rad")) {
    text.remove_suffix(3);
    scale = 180.0 / 3.14159265358979323846;
  } else if (text.ends_with("turn")) {
    text.remove_suffix(4);
    scale = 360.0;
  }
  const std::optional<double> value = util::ParseDouble(text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  double degrees = std::fmod(*value * scale, 360.0);
  if (degrees < 0.0) {
    degrees += 360.0;
  }
  return degrees;
}

std::uint8_t ToByte(double value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0) + 0.5);
}

// CSS Color 4's HSL-to-RGB, written as the specification writes it.
std::array<double, 3> HslToRgb(double hue, double saturation, double lightness) {
  const auto channel = [&](double n) {
    const double k = std::fmod(n + hue / 30.0, 12.0);
    const double a = saturation * std::min(lightness, 1.0 - lightness);
    return lightness - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
  };
  return {channel(0.0) * 255.0, channel(8.0) * 255.0, channel(4.0) * 255.0};
}

// The body of a colour function, split into components and an optional alpha, or nullopt when the
// two grammars were mixed or the shape is wrong.
struct FunctionArguments {
  std::vector<std::string_view> components;
  std::string_view alpha;
  bool has_alpha = false;
  bool legacy = false;
};

std::optional<FunctionArguments> SplitFunction(std::string_view body) {
  FunctionArguments arguments;
  const bool has_comma = body.find(',') != std::string_view::npos;
  const bool has_slash = body.find('/') != std::string_view::npos;
  if (has_comma && has_slash) {
    return std::nullopt;  // the mixed form
  }
  if (has_comma) {
    arguments.legacy = true;
    std::size_t start = 0;
    while (true) {
      const std::size_t comma = body.find(',', start);
      const std::string_view part =
          Trim(body.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                  : comma - start));
      if (part.empty()) {
        return std::nullopt;
      }
      arguments.components.push_back(part);
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
    }
    if (arguments.components.size() == 4) {
      arguments.alpha = arguments.components.back();
      arguments.has_alpha = true;
      arguments.components.pop_back();
    }
    return arguments.components.size() == 3 ? std::optional<FunctionArguments>(arguments)
                                            : std::nullopt;
  }
  const std::size_t slash = body.find('/');
  if (slash != std::string_view::npos) {
    if (body.find('/', slash + 1) != std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view after = Trim(body.substr(slash + 1));
    if (after.empty()) {
      return std::nullopt;
    }
    arguments.alpha = after;
    arguments.has_alpha = true;
    body = body.substr(0, slash);
  }
  arguments.components = SplitWords(body);
  return arguments.components.size() == 3 ? std::optional<FunctionArguments>(arguments)
                                          : std::nullopt;
}

std::optional<std::uint8_t> AlphaFrom(const FunctionArguments& arguments) {
  if (!arguments.has_alpha) {
    return static_cast<std::uint8_t>(255);
  }
  const std::optional<Component> component = ParseComponent(arguments.alpha);
  if (!component.has_value() || (component->is_none && arguments.legacy)) {
    return std::nullopt;
  }
  const double fraction = component->is_percent ? component->value / 100.0 : component->value;
  return static_cast<std::uint8_t>(std::clamp(fraction, 0.0, 1.0) * 255.0 + 0.5);
}

std::optional<Color> ParseRgbFunction(std::string_view body) {
  const std::optional<FunctionArguments> arguments = SplitFunction(body);
  if (!arguments.has_value()) {
    return std::nullopt;
  }
  std::array<std::uint8_t, 3> channels{};
  bool any_percent = false;
  bool any_number = false;
  for (std::size_t i = 0; i < 3; ++i) {
    const std::optional<Component> component = ParseComponent(arguments->components[i]);
    if (!component.has_value() || (component->is_none && arguments->legacy)) {
      return std::nullopt;
    }
    any_percent = any_percent || component->is_percent;
    any_number = any_number || (!component->is_percent && !component->is_none);
    channels[i] = component->is_percent ? ToByte(component->value * 255.0 / 100.0)
                                        : ToByte(component->value);
  }
  // The legacy grammar takes three numbers *or* three percentages, never a mixture. The modern one
  // does allow the mixture, which is the one place the two differ beyond punctuation.
  if (arguments->legacy && any_percent && any_number) {
    return std::nullopt;
  }
  const std::optional<std::uint8_t> alpha = AlphaFrom(*arguments);
  if (!alpha.has_value()) {
    return std::nullopt;
  }
  return Color::Rgba(channels[0], channels[1], channels[2], *alpha);
}

std::optional<Color> ParseHslFunction(std::string_view body) {
  const std::optional<FunctionArguments> arguments = SplitFunction(body);
  if (!arguments.has_value()) {
    return std::nullopt;
  }
  const std::optional<double> hue = ParseHue(arguments->components[0]);
  if (!hue.has_value() || (arguments->components[0] == "none" && arguments->legacy)) {
    return std::nullopt;
  }
  std::array<double, 2> parts{};
  for (std::size_t i = 0; i < 2; ++i) {
    const std::optional<Component> component = ParseComponent(arguments->components[i + 1]);
    if (!component.has_value() || (component->is_none && arguments->legacy)) {
      return std::nullopt;
    }
    // Legacy `hsl()` requires percentages for both; the modern grammar takes a bare number as one.
    if (arguments->legacy && !component->is_percent && !component->is_none) {
      return std::nullopt;
    }
    parts[i] = std::clamp(component->value, 0.0, 100.0) / 100.0;
  }
  const std::optional<std::uint8_t> alpha = AlphaFrom(*arguments);
  if (!alpha.has_value()) {
    return std::nullopt;
  }
  const std::array<double, 3> rgb = HslToRgb(*hue, parts[0], parts[1]);
  return Color::Rgba(ToByte(rgb[0]), ToByte(rgb[1]), ToByte(rgb[2]), *alpha);
}

}  // namespace

std::optional<Color> ParseColorText(std::string_view text) {
  const std::string lowered = util::AsciiLowerCase(Trim(text));
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

  const bool is_rgb = lowered.rfind("rgb(", 0) == 0 || lowered.rfind("rgba(", 0) == 0;
  const bool is_hsl = lowered.rfind("hsl(", 0) == 0 || lowered.rfind("hsla(", 0) == 0;
  if (is_rgb || is_hsl) {
    const std::size_t open = lowered.find('(');
    const std::size_t close = lowered.rfind(')');
    if (close == std::string::npos || close < open || close + 1 != lowered.size()) {
      return std::nullopt;
    }
    const std::string_view body(lowered.data() + open + 1, close - open - 1);
    return is_rgb ? ParseRgbFunction(body) : ParseHslFunction(body);
  }

  for (const NamedColor& named : kNamedColors) {
    if (named.name == lowered) {
      return Color{named.argb};
    }
  }
  return std::nullopt;
}

}  // namespace microbrowser::gfx

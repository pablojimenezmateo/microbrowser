#include "gfx/ColorText.h"

#include <algorithm>
#include <array>
#include <cmath>
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

// Every named colour CSS Color 4 defines, plus `transparent` and
// `currentcolor`, sorted so the lookup can bisect.
//
// This was 22 entries -- "the colours a real page actually uses by name" -- and
// the cost of the other 126 was not a wrong colour. A declaration whose value
// does not parse is *dropped*, so `border: 1px solid limegreen` contributed no
// border at all and every box wearing one was 2px smaller than the page said.
// That is a layout difference, and it is invisible until something else lines
// those boxes up: `css/css-flexbox/flexbox-flex-direction-column.htm` renders
// nine 38px cells where the specification says 40px, and only stopped matching
// its reference once `align-content` had free space to distribute.
constexpr std::array<NamedColor, 150> kNamedColors = {{
    {"aliceblue", 0xFFF0F8FF}, {"antiquewhite", 0xFFFAEBD7}, {"aqua", 0xFF00FFFF},
    {"aquamarine", 0xFF7FFFD4}, {"azure", 0xFFF0FFFF}, {"beige", 0xFFF5F5DC},
    {"bisque", 0xFFFFE4C4}, {"black", 0xFF000000}, {"blanchedalmond", 0xFFFFEBCD},
    {"blue", 0xFF0000FF}, {"blueviolet", 0xFF8A2BE2}, {"brown", 0xFFA52A2A},
    {"burlywood", 0xFFDEB887}, {"cadetblue", 0xFF5F9EA0}, {"chartreuse", 0xFF7FFF00},
    {"chocolate", 0xFFD2691E}, {"coral", 0xFFFF7F50}, {"cornflowerblue", 0xFF6495ED},
    {"cornsilk", 0xFFFFF8DC}, {"crimson", 0xFFDC143C}, {"currentcolor", 0xFF000000},
    {"cyan", 0xFF00FFFF}, {"darkblue", 0xFF00008B}, {"darkcyan", 0xFF008B8B},
    {"darkgoldenrod", 0xFFB8860B}, {"darkgray", 0xFFA9A9A9}, {"darkgreen", 0xFF006400},
    {"darkgrey", 0xFFA9A9A9}, {"darkkhaki", 0xFFBDB76B}, {"darkmagenta", 0xFF8B008B},
    {"darkolivegreen", 0xFF556B2F}, {"darkorange", 0xFFFF8C00}, {"darkorchid", 0xFF9932CC},
    {"darkred", 0xFF8B0000}, {"darksalmon", 0xFFE9967A}, {"darkseagreen", 0xFF8FBC8F},
    {"darkslateblue", 0xFF483D8B}, {"darkslategray", 0xFF2F4F4F}, {"darkslategrey", 0xFF2F4F4F},
    {"darkturquoise", 0xFF00CED1}, {"darkviolet", 0xFF9400D3}, {"deeppink", 0xFFFF1493},
    {"deepskyblue", 0xFF00BFFF}, {"dimgray", 0xFF696969}, {"dimgrey", 0xFF696969},
    {"dodgerblue", 0xFF1E90FF}, {"firebrick", 0xFFB22222}, {"floralwhite", 0xFFFFFAF0},
    {"forestgreen", 0xFF228B22}, {"fuchsia", 0xFFFF00FF}, {"gainsboro", 0xFFDCDCDC},
    {"ghostwhite", 0xFFF8F8FF}, {"gold", 0xFFFFD700}, {"goldenrod", 0xFFDAA520},
    {"gray", 0xFF808080}, {"green", 0xFF008000}, {"greenyellow", 0xFFADFF2F},
    {"grey", 0xFF808080}, {"honeydew", 0xFFF0FFF0}, {"hotpink", 0xFFFF69B4},
    {"indianred", 0xFFCD5C5C}, {"indigo", 0xFF4B0082}, {"ivory", 0xFFFFFFF0},
    {"khaki", 0xFFF0E68C}, {"lavender", 0xFFE6E6FA}, {"lavenderblush", 0xFFFFF0F5},
    {"lawngreen", 0xFF7CFC00}, {"lemonchiffon", 0xFFFFFACD}, {"lightblue", 0xFFADD8E6},
    {"lightcoral", 0xFFF08080}, {"lightcyan", 0xFFE0FFFF}, {"lightgoldenrodyellow", 0xFFFAFAD2},
    {"lightgray", 0xFFD3D3D3}, {"lightgreen", 0xFF90EE90}, {"lightgrey", 0xFFD3D3D3},
    {"lightpink", 0xFFFFB6C1}, {"lightsalmon", 0xFFFFA07A}, {"lightseagreen", 0xFF20B2AA},
    {"lightskyblue", 0xFF87CEFA}, {"lightslategray", 0xFF778899}, {"lightslategrey", 0xFF778899},
    {"lightsteelblue", 0xFFB0C4DE}, {"lightyellow", 0xFFFFFFE0}, {"lime", 0xFF00FF00},
    {"limegreen", 0xFF32CD32}, {"linen", 0xFFFAF0E6}, {"magenta", 0xFFFF00FF},
    {"maroon", 0xFF800000}, {"mediumaquamarine", 0xFF66CDAA}, {"mediumblue", 0xFF0000CD},
    {"mediumorchid", 0xFFBA55D3}, {"mediumpurple", 0xFF9370DB}, {"mediumseagreen", 0xFF3CB371},
    {"mediumslateblue", 0xFF7B68EE}, {"mediumspringgreen", 0xFF00FA9A},
    {"mediumturquoise", 0xFF48D1CC}, {"mediumvioletred", 0xFFC71585},
    {"midnightblue", 0xFF191970}, {"mintcream", 0xFFF5FFFA}, {"mistyrose", 0xFFFFE4E1},
    {"moccasin", 0xFFFFE4B5}, {"navajowhite", 0xFFFFDEAD}, {"navy", 0xFF000080},
    {"oldlace", 0xFFFDF5E6}, {"olive", 0xFF808000}, {"olivedrab", 0xFF6B8E23},
    {"orange", 0xFFFFA500}, {"orangered", 0xFFFF4500}, {"orchid", 0xFFDA70D6},
    {"palegoldenrod", 0xFFEEE8AA}, {"palegreen", 0xFF98FB98}, {"paleturquoise", 0xFFAFEEEE},
    {"palevioletred", 0xFFDB7093}, {"papayawhip", 0xFFFFEFD5}, {"peachpuff", 0xFFFFDAB9},
    {"peru", 0xFFCD853F}, {"pink", 0xFFFFC0CB}, {"plum", 0xFFDDA0DD}, {"powderblue", 0xFFB0E0E6},
    {"purple", 0xFF800080}, {"rebeccapurple", 0xFF663399}, {"red", 0xFFFF0000},
    {"rosybrown", 0xFFBC8F8F}, {"royalblue", 0xFF4169E1}, {"saddlebrown", 0xFF8B4513},
    {"salmon", 0xFFFA8072}, {"sandybrown", 0xFFF4A460}, {"seagreen", 0xFF2E8B57},
    {"seashell", 0xFFFFF5EE}, {"sienna", 0xFFA0522D}, {"silver", 0xFFC0C0C0},
    {"skyblue", 0xFF87CEEB}, {"slateblue", 0xFF6A5ACD}, {"slategray", 0xFF708090},
    {"slategrey", 0xFF708090}, {"snow", 0xFFFFFAFA}, {"springgreen", 0xFF00FF7F},
    {"steelblue", 0xFF4682B4}, {"tan", 0xFFD2B48C}, {"teal", 0xFF008080}, {"thistle", 0xFFD8BFD8},
    {"tomato", 0xFFFF6347}, {"transparent", 0x00000000}, {"turquoise", 0xFF40E0D0},
    {"violet", 0xFFEE82EE}, {"wheat", 0xFFF5DEB3}, {"white", 0xFFFFFFFF},
    {"whitesmoke", 0xFFF5F5F5}, {"yellow", 0xFFFFFF00}, {"yellowgreen", 0xFF9ACD32},
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

// CSS's `<number>` allows a leading `+` and `std::from_chars` does not -- it is documented as not
// accepting one, which is right for `util::ParseDouble`'s other callers and wrong here. Dropped
// exactly once, so `++0` stays invalid.
std::string_view DropLeadingPlus(std::string_view text) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  return text;
}

std::optional<Component> ParseComponent(std::string_view text) {
  Component component;
  if (text == "none") {
    component.is_none = true;
    return component;
  }
  text = DropLeadingPlus(text);
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
  text = DropLeadingPlus(text);
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

std::string SerializeColorText(const Color& color) {
  const std::string channels = std::to_string(static_cast<int>(color.Red())) + ", " +
                               std::to_string(static_cast<int>(color.Green())) + ", " +
                               std::to_string(static_cast<int>(color.Blue()));
  if (color.IsOpaque()) {
    return "rgb(" + channels + ")";
  }
  // The alpha is a fraction with no trailing zeros: `rgba(0, 0, 0, 0.5)`, never `0.500000`. It is
  // computed from the byte rather than remembered, because a Color *is* eight bits per channel and
  // reporting more precision than it holds would be a number no round trip could reproduce.
  std::string alpha = std::to_string(static_cast<double>(color.Alpha()) / 255.0);
  if (alpha.find('.') != std::string::npos) {
    alpha.erase(alpha.find_last_not_of('0') + 1);
    if (!alpha.empty() && alpha.back() == '.') {
      alpha.pop_back();
    }
  }
  return "rgba(" + channels + ", " + (alpha.empty() ? "0" : alpha) + ")";
}

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
    if (digits.size() == 4) {
      // `#abcd` is `#aabbccdd`, the four-digit form with alpha -- doubled rather than shifted, for
      // the reason `#abc` is.
      return Color::Rgba(static_cast<std::uint8_t>(nibble(0) * 17),
                         static_cast<std::uint8_t>(nibble(1) * 17),
                         static_cast<std::uint8_t>(nibble(2) * 17),
                         static_cast<std::uint8_t>(nibble(3) * 17));
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

  const auto found = std::lower_bound(
      kNamedColors.begin(), kNamedColors.end(), std::string_view(lowered),
      [](const NamedColor& named, std::string_view name) { return named.name < name; });
  if (found != kNamedColors.end() && found->name == lowered) {
    return Color{found->argb};
  }
  return std::nullopt;
}

}  // namespace microbrowser::gfx

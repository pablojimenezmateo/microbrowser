#include "css/StyleResolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/CssText.h"
#include "gfx/Font.h"
#include "util/Parse.h"

// Turning one declaration into computed style: parsing the values CSS is
// written in, and applying them.
//
// Split from StyleResolver.cpp because the two answer different questions.
// This file knows what `#1a2b3c`, `1.5em` and `2px solid red` mean; that one
// knows which declarations win and in what order. Neither needs the other's
// details, and together they were over the module's translation unit cap --
// which is the lint saying a module was missing rather than a file being big.

namespace microbrowser::css {

namespace {

// Splits a `font-family` value into its candidates, in order.
//
// Quotes are stripped and do not delimit a candidate on their own -- a quoted
// name may contain a comma, which is the only reason the split is a scan rather
// than a call to something generic. Unquoted names keep their spaces; the
// catalog collapses runs when it normalizes, so `Helvetica  Neue` still matches.
// Empty entries are dropped rather than becoming a request for the default,
// because `Verdana, , serif` should mean what `Verdana, serif` means.
std::vector<std::string> ParseFontFamilyList(std::string_view value) {
  std::vector<std::string> families;
  std::string current;
  char quote = '\0';
  const auto flush = [&families, &current]() {
    const std::string_view trimmed = Trim(current);
    if (!trimmed.empty() && families.size() < gfx::kMaxFontFamilies) {
      families.emplace_back(trimmed);
    }
    current.clear();
  };
  for (const char c : value) {
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else {
        current.push_back(c);
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == ',') {
      flush();
    } else {
      current.push_back(c);
    }
  }
  flush();
  return families;
}

std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsCssWhitespace(text[start])) {
      ++start;
    }
    std::size_t end = start;
    while (end < text.size() && !IsCssWhitespace(text[end])) {
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

float Length::Resolve(float font_size, float fallback) const {
  switch (unit) {
    case Unit::Pixels:
      return value;
    case Unit::Em:
      return value * font_size;
    case Unit::Rem:
      // The root font size is 16 until a root style exists to ask. Carrying the
      // root style through every Resolve call would put a parameter on a
      // function that is called per edge per element per frame.
      return value * 16.0f;
    case Unit::Percent:
    case Unit::Auto:
      return fallback;
  }
  return fallback;
}

std::optional<gfx::Color> ParseColor(std::string_view text) {
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
      return gfx::Color::Rgb(static_cast<std::uint8_t>(nibble(0) * 17),
                             static_cast<std::uint8_t>(nibble(1) * 17),
                             static_cast<std::uint8_t>(nibble(2) * 17));
    }
    if (digits.size() == 6) {
      return gfx::Color::Rgb(static_cast<std::uint8_t>(nibble(0) * 16 + nibble(1)),
                             static_cast<std::uint8_t>(nibble(2) * 16 + nibble(3)),
                             static_cast<std::uint8_t>(nibble(4) * 16 + nibble(5)));
    }
    if (digits.size() == 8) {
      return gfx::Color::Rgba(static_cast<std::uint8_t>(nibble(0) * 16 + nibble(1)),
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
    return gfx::Color::Rgba(channels[0], channels[1], channels[2], alpha);
  }

  for (const NamedColor& named : kNamedColors) {
    if (named.name == lowered) {
      return gfx::Color{named.argb};
    }
  }
  return std::nullopt;
}

std::optional<Length> ParseLength(std::string_view text) {
  const std::string lowered = Lowered(Trim(text));
  if (lowered.empty()) {
    return std::nullopt;
  }
  if (lowered == "auto") {
    return Length::Auto();
  }

  std::size_t at = 0;
  double sign = 1.0;
  if (at < lowered.size() && (lowered[at] == '+' || lowered[at] == '-')) {
    sign = lowered[at] == '-' ? -1.0 : 1.0;
    ++at;
  }
  double value = 0.0;
  bool any_digit = false;
  while (at < lowered.size() && lowered[at] >= '0' && lowered[at] <= '9') {
    value = value * 10.0 + (lowered[at++] - '0');
    any_digit = true;
  }
  if (at < lowered.size() && lowered[at] == '.') {
    ++at;
    double step = 0.1;
    while (at < lowered.size() && lowered[at] >= '0' && lowered[at] <= '9') {
      value += (lowered[at++] - '0') * step;
      step *= 0.1;
      any_digit = true;
    }
  }
  if (!any_digit) {
    return std::nullopt;
  }
  value *= sign;

  const std::string_view unit(lowered.data() + at, lowered.size() - at);
  if (unit.empty()) {
    // A unitless number is a length only when it is zero. `width: 5` is not
    // five pixels, it is an invalid declaration, and treating it as pixels is
    // how a page renders differently here than everywhere else.
    return value == 0.0 ? std::optional<Length>(Length::Pixels(0.0f)) : std::nullopt;
  }
  if (unit == "px") {
    return Length::Pixels(static_cast<float>(value));
  }
  if (unit == "%") {
    return Length{static_cast<float>(value), Length::Unit::Percent};
  }
  if (unit == "em") {
    return Length{static_cast<float>(value), Length::Unit::Em};
  }
  if (unit == "rem") {
    return Length{static_cast<float>(value), Length::Unit::Rem};
  }
  if (unit == "pt") {
    // 1pt is 4/3 px, which is the one absolute unit conversion a browser
    // actually needs.
    return Length::Pixels(static_cast<float>(value * 4.0 / 3.0));
  }
  return std::nullopt;
}

namespace {

bool EdgeLengthAllowed(const Length& length, bool allow_negative, bool allow_auto,
                       bool allow_percent) {
  if (!allow_auto && length.IsAuto()) {
    return false;
  }
  if (!allow_percent && length.IsPercent()) {
    return false;
  }
  return allow_negative || length.value >= 0.0f;
}

bool ApplyEdges(std::string_view value, Edges& edges, bool allow_negative, bool allow_auto,
                bool allow_percent) {
  const std::vector<std::string_view> parts = SplitWords(value);
  if (parts.empty() || parts.size() > 4) {
    return false;
  }
  std::array<Length, 4> lengths;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto length = ParseLength(parts[i]);
    if (!length.has_value() ||
        !EdgeLengthAllowed(*length, allow_negative, allow_auto, allow_percent)) {
      return false;  // one bad component invalidates the whole shorthand
    }
    lengths[i] = *length;
  }
  switch (parts.size()) {
    case 1:
      edges = Edges{lengths[0], lengths[0], lengths[0], lengths[0]};
      return true;
    case 2:
      edges = Edges{lengths[0], lengths[1], lengths[0], lengths[1]};
      return true;
    case 3:
      edges = Edges{lengths[0], lengths[1], lengths[2], lengths[1]};
      return true;
    case 4:
      edges = Edges{lengths[0], lengths[1], lengths[2], lengths[3]};
      return true;
    default:
      return false;
  }
}

bool IsBorderStyleKeyword(std::string_view value) {
  return value == "none" || value == "hidden" || value == "dotted" || value == "dashed" ||
         value == "solid" || value == "double" || value == "groove" || value == "ridge" ||
         value == "inset" || value == "outset";
}

void ApplyBorder(std::string_view value, ComputedStyle& style) {
  const std::vector<std::string_view> parts = SplitWords(value);
  if (parts.empty()) {
    return;
  }

  std::optional<Edges> width;
  std::optional<gfx::Color> color;
  bool saw_style = false;
  bool style_disables_border = false;
  for (const std::string_view part : parts) {
    if (const auto length = ParseLength(part)) {
      if (width.has_value() || !EdgeLengthAllowed(*length, false, false, false)) {
        return;
      }
      width = Edges{*length, *length, *length, *length};
    } else if (const auto parsed_color = ParseColor(part)) {
      if (color.has_value()) {
        return;
      }
      color = *parsed_color;
    } else {
      const std::string lowered = Lowered(part);
      if (!IsBorderStyleKeyword(lowered)) {
        return;
      }
      if (saw_style) {
        return;
      }
      saw_style = true;
      style_disables_border = lowered == "none" || lowered == "hidden";
    }
  }

  if (width.has_value()) {
    style.border_width = *width;
  }
  if (color.has_value()) {
    style.border_color = *color;
  }
  style.has_border = !style_disables_border;
}

}  // namespace

void ApplyDeclaration(const Declaration& declaration, const ComputedStyle& parent,
                      ComputedStyle& style) {
  const std::string& property = declaration.property;
  const std::string value = Lowered(declaration.value);

  if (property == "display") {
    if (value == "block") {
      style.display = Display::Block;
    } else if (value == "inline") {
      style.display = Display::Inline;
    } else if (value == "inline-block") {
      style.display = Display::InlineBlock;
    } else if (value == "list-item") {
      style.display = Display::ListItem;
    } else if (value == "table") {
      style.display = Display::Table;
    } else if (value == "table-caption") {
      style.display = Display::TableCaption;
    } else if (value == "table-column-group") {
      style.display = Display::TableColumnGroup;
    } else if (value == "table-column") {
      style.display = Display::TableColumn;
    } else if (value == "table-header-group") {
      style.display = Display::TableHeaderGroup;
    } else if (value == "table-footer-group") {
      style.display = Display::TableFooterGroup;
    } else if (value == "table-row-group") {
      style.display = Display::TableRowGroup;
    } else if (value == "table-row") {
      style.display = Display::TableRow;
    } else if (value == "table-cell") {
      style.display = Display::TableCell;
    } else if (value == "none") {
      style.display = Display::None;
    }
    return;
  }
  if (property == "color") {
    if (const auto color = ParseColor(declaration.value)) {
      style.color = *color;
    }
    return;
  }
  if (property == "background-color" || property == "background") {
    if (const auto color = ParseColor(declaration.value)) {
      style.background_color = *color;
    }
    return;
  }
  if (property == "font-size") {
    // Resolved to absolute pixels during the cascade, because `em` on every
    // other property is relative to it — and `font-size: 2em` is relative to
    // the *parent's* size, not its own.
    if (value == "smaller") {
      style.font_size = parent.font_size * 0.8f;
      return;
    }
    if (value == "larger") {
      style.font_size = parent.font_size * 1.25f;
      return;
    }
    if (const auto length = ParseLength(declaration.value)) {
      float resolved = -1.0f;
      if (length->unit == Length::Unit::Percent) {
        resolved = parent.font_size * length->value / 100.0f;
      } else if (length->unit == Length::Unit::Em) {
        resolved = parent.font_size * length->value;
      } else if (!length->IsAuto()) {
        resolved = length->Resolve(parent.font_size);
      }
      if (resolved >= 0.0f) {
        style.font_size = std::clamp(resolved, 1.0f, 1000.0f);
      }
    }
    return;
  }
  if (property == "font-weight") {
    if (value == "bold") {
      style.font_weight = 700.0f;
    } else if (value == "normal") {
      style.font_weight = 400.0f;
    } else if (const auto weight = util::ParseInt(value)) {
      if (*weight >= 1 && *weight <= 1000) {
        style.font_weight = static_cast<float>(*weight);
      }
    }
    return;
  }
  if (property == "float") {
    if (value == "left") {
      style.css_float = Float::Left;
    } else if (value == "right") {
      style.css_float = Float::Right;
    } else if (value == "none") {
      style.css_float = Float::None;
    }
    return;
  }
  if (property == "clear") {
    if (value == "left") {
      style.clear = Clear::Left;
    } else if (value == "right") {
      style.clear = Clear::Right;
    } else if (value == "both") {
      style.clear = Clear::Both;
    } else if (value == "none") {
      style.clear = Clear::None;
    }
    return;
  }
  if (property == "font-style") {
    if (value == "italic" || value == "oblique") {
      style.font_style = FontStyle::Italic;
    } else if (value == "normal") {
      style.font_style = FontStyle::Normal;
    }
    return;
  }
  if (property == "font-family") {
    // A value that parses to nothing (`font-family: ,`) leaves the inherited
    // list alone rather than clearing it, which is what an invalid declaration
    // is supposed to do.
    if (std::vector<std::string> families = ParseFontFamilyList(declaration.value);
        !families.empty()) {
      style.font_family = std::move(families);
    }
    return;
  }
  if (property == "line-height") {
    if (value == "normal") {
      style.line_height = 0.0f;
      return;
    }
    if (const auto length = ParseLength(declaration.value)) {
      const float resolved = length->unit == Length::Unit::Percent
                                 ? style.font_size * length->value / 100.0f
                                 : length->Resolve(style.font_size, -1.0f);
      if (resolved >= 0.0f) {
        style.line_height = resolved;
      }
    } else if (const auto multiple = util::ParseDouble(value)) {
      if (*multiple >= 0.0) {
        style.line_height = static_cast<float>(*multiple) * style.font_size;
      }
    }
    return;
  }
  if (property == "text-align") {
    // Every value also settles whether block children are centred, so that
    // `text-align: left` after a <center> undoes all of it rather than half.
    style.centers_block_children = false;
    if (value == "center") {
      style.text_align = TextAlign::Center;
    } else if (value == "right") {
      style.text_align = TextAlign::Right;
    } else if (value == "justify") {
      style.text_align = TextAlign::Justify;
    } else if (value == "left") {
      style.text_align = TextAlign::Left;
    } else if (value == "-microbrowser-center") {
      // What <center> means, and what no standard value expresses. See the
      // note on ComputedStyle::centers_block_children.
      style.text_align = TextAlign::Center;
      style.centers_block_children = true;
    }
    return;
  }
  if (property == "white-space") {
    if (value == "pre") {
      style.white_space = WhiteSpace::Pre;
    } else if (value == "nowrap") {
      style.white_space = WhiteSpace::NoWrap;
    } else if (value == "pre-wrap") {
      style.white_space = WhiteSpace::PreWrap;
    } else if (value == "normal") {
      style.white_space = WhiteSpace::Normal;
    }
    return;
  }
  if (property == "margin") {
    ApplyEdges(declaration.value, style.margin, true, true, true);
    return;
  }
  if (property == "padding") {
    ApplyEdges(declaration.value, style.padding, false, false, true);
    return;
  }
  if (property == "width" || property == "height") {
    if (const auto length = ParseLength(declaration.value);
        length.has_value() && EdgeLengthAllowed(*length, false, true, true)) {
      (property == "width" ? style.width : style.height) = *length;
    }
    return;
  }
  if (property == "border-color") {
    if (const auto color = ParseColor(declaration.value)) {
      style.border_color = *color;
      style.has_border = true;
    }
    return;
  }
  if (property == "border-width") {
    if (ApplyEdges(declaration.value, style.border_width, false, false, false)) {
      style.has_border = true;
    }
    return;
  }
  if (property == "border") {
    ApplyBorder(declaration.value, style);
    return;
  }

  // Individual edge properties. Written as a loop rather than sixteen branches.
  static constexpr std::array<std::string_view, 4> kSides = {"top", "right", "bottom", "left"};
  for (std::size_t i = 0; i < kSides.size(); ++i) {
    const std::string margin_name = "margin-" + std::string(kSides[i]);
    const std::string padding_name = "padding-" + std::string(kSides[i]);
    if (property == margin_name || property == padding_name) {
      const auto length = ParseLength(declaration.value);
      const bool is_margin = property == margin_name;
      if (!length.has_value() ||
          !EdgeLengthAllowed(*length, is_margin, is_margin, true)) {
        return;
      }
      Edges& edges = is_margin ? style.margin : style.padding;
      (&edges.top)[i] = *length;
      return;
    }
  }
  // Anything else is a property we do not implement. Ignored rather than
  // guessed at.
}

}  // namespace microbrowser::css

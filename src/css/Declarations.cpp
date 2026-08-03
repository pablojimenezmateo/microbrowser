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
#include "gfx/ColorText.h"
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

// Splits on `separator`, but not inside a function's parentheses or a string.
// `url(a,b), red` is two layers, not three: a comma inside `url()` belongs to
// the url. A splitter that did not track nesting would produce a fragment that
// parses as neither a url nor a colour and would silently drop the layer.
std::vector<std::string_view> SplitTopLevel(std::string_view value, char separator) {
  std::vector<std::string_view> parts;
  std::size_t depth = 0;
  char quote = '\0';
  std::size_t begin = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      depth -= depth > 0 ? 1 : 0;
    } else if (c == separator && depth == 0) {
      parts.push_back(value.substr(begin, i - begin));
      begin = i + 1;
    }
  }
  parts.push_back(value.substr(begin));
  return parts;
}

// The contents of a `url(...)`, unquoted, or empty when `value` does not
// contain one. Not a general function parser: `url()` is the only CSS function
// this renderer fetches anything for.
std::string ParseUrlFunction(std::string_view value) {
  const std::string_view trimmed = Trim(value);
  const std::size_t open = Lowered(trimmed).find("url(");
  if (open == std::string::npos) {
    return {};
  }
  const std::size_t close = trimmed.find(')', open);
  if (close == std::string_view::npos) {
    return {};
  }
  std::string_view inner = Trim(trimmed.substr(open + 4, close - open - 4));
  if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'') &&
      inner.back() == inner.front()) {
    inner = inner.substr(1, inner.size() - 2);
  }
  return std::string(inner);
}

std::optional<BackgroundRepeat> ParseBackgroundRepeat(std::string_view word) {
  const std::string lowered = Lowered(Trim(word));
  if (lowered == "repeat") {
    return BackgroundRepeat::Repeat;
  }
  if (lowered == "repeat-x") {
    return BackgroundRepeat::RepeatX;
  }
  if (lowered == "repeat-y") {
    return BackgroundRepeat::RepeatY;
  }
  if (lowered == "no-repeat") {
    return BackgroundRepeat::NoRepeat;
  }
  return std::nullopt;
}

// A `background-position` component. The keywords are percentages of the space
// the image does not fill, which is what makes `center` centre rather than
// offset by half the box.
Length ParseBackgroundPosition(std::string_view word, bool horizontal) {
  const std::string lowered = Lowered(Trim(word));
  if (lowered == "center") {
    return Length{50.0f, Length::Unit::Percent};
  }
  if (lowered == (horizontal ? "left" : "top")) {
    return Length::Pixels(0.0f);
  }
  if (lowered == (horizontal ? "right" : "bottom")) {
    return Length{100.0f, Length::Unit::Percent};
  }
  return ParseLength(word).value_or(Length::Pixels(0.0f));
}

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

// Delegates: the syntax is shared with SVG, so the implementation lives with
// the type it produces. See gfx/ColorText.h.
std::optional<gfx::Color> ParseColor(std::string_view text) {
  return gfx::ParseColorText(text);
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
    } else if (value == "flex") {
      style.display = Display::Flex;
    } else if (value == "inline-flex") {
      style.display = Display::InlineFlex;
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
  if (property == "background-color") {
    if (const auto color = ParseColor(declaration.value)) {
      style.background_color = *color;
    }
    return;
  }
  if (property == "background") {
    // The shorthand, read for the parts this renderer has. Every longhand it
    // sets is reset first, because a shorthand sets *all* of its longhands --
    // a `background: red` after a `background-image` must clear the image, and
    // one that only assigned what it recognised would leave it behind.
    style.background_color = gfx::Color::Transparent();
    style.background.image.clear();
    style.background.repeat = BackgroundRepeat::Repeat;
    if (const auto color = ParseColor(declaration.value)) {
      style.background_color = *color;
      return;
    }
    // A layer list: `url(a), linear-gradient(...)`. The first layer that is an
    // image this renderer can fetch wins; a gradient layer is skipped rather
    // than approximated, and a value that is nothing but gradients leaves the
    // element with no background at all -- which is what it looks like here,
    // and is more honest than a flat colour nobody chose.
    for (const std::string_view layer : SplitTopLevel(declaration.value, ',')) {
      if (std::string url = ParseUrlFunction(layer); !url.empty()) {
        style.background.image = std::move(url);
        break;
      }
    }
    for (const std::string_view word : SplitWords(declaration.value)) {
      if (const std::optional<BackgroundRepeat> repeat = ParseBackgroundRepeat(word)) {
        style.background.repeat = *repeat;
      } else if (const auto color = ParseColor(word)) {
        style.background_color = *color;
      }
    }
    return;
  }
  if (property == "background-image") {
    style.background.image.clear();
    for (const std::string_view layer : SplitTopLevel(declaration.value, ',')) {
      if (std::string url = ParseUrlFunction(layer); !url.empty()) {
        style.background.image = std::move(url);
        break;
      }
    }
    return;
  }
  if (property == "background-repeat") {
    // Two values are the per-axis form; one applies to both. Only the first is
    // read, because `repeat no-repeat` is spelled `repeat-x` far more often
    // and the pair adds a second way to say the same four things.
    const std::vector<std::string_view> words = SplitWords(declaration.value);
    if (!words.empty()) {
      if (const std::optional<BackgroundRepeat> repeat = ParseBackgroundRepeat(words.front())) {
        style.background.repeat = *repeat;
      }
    }
    return;
  }
  if (property == "background-size") {
    // `contain` and `cover` are not lengths and are not supported: both need
    // the image's aspect ratio, which the cascade does not have. A single
    // length applies to the width and leaves the height automatic, which is
    // what keeps an icon's proportions.
    const std::vector<std::string_view> words = SplitWords(declaration.value);
    if (words.empty() || words.size() > 2) {
      return;
    }
    const std::optional<Length> first = ParseLength(words.front());
    if (!first.has_value()) {
      return;
    }
    style.background.size_x = *first;
    style.background.size_y =
        words.size() == 2 ? ParseLength(words[1]).value_or(Length::Auto()) : Length::Auto();
    return;
  }
  if (property == "background-position") {
    const std::vector<std::string_view> words = SplitWords(declaration.value);
    if (words.empty() || words.size() > 2) {
      return;
    }
    // A single value sets the horizontal position and centres the vertical,
    // which is what CSS says and is the difference between an icon on the left
    // edge and one halfway down it.
    style.background.position_x = ParseBackgroundPosition(words.front(), true);
    style.background.position_y =
        words.size() == 2 ? ParseBackgroundPosition(words[1], false)
                          : Length{50.0f, Length::Unit::Percent};
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
  // The flex properties and the sizing bounds, in their own translation unit.
  // Not a stylistic split: this file is at its module's line cap, and the cap
  // means a missing module rather than a bigger file. These are the properties
  // that arrived together and are read together, so they are the ones that
  // move.
  if (ApplyBoxDeclaration(property, value, parent, style)) {
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

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

#include "css/Calc.h"
#include "css/CssText.h"
#include "css/MediaQuery.h"
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

void InheritInto(const ComputedStyle& parent, ComputedStyle& child, bool with_custom_properties) {
  child.color = parent.color;
  child.font_size = parent.font_size;
  child.root_font_size = parent.root_font_size;
  child.font_weight = parent.font_weight;
  child.font_style = parent.font_style;
  child.font_family = parent.font_family;
  child.line_height = parent.line_height;
  child.text_align = parent.text_align;
  child.direction = parent.direction;
  child.unicode_bidi = parent.unicode_bidi;
  child.white_space_collapse = parent.white_space_collapse;
  child.text_wrap_mode = parent.text_wrap_mode;
  child.text_transform = parent.text_transform;
  child.word_break = parent.word_break;
  child.overflow_wrap = parent.overflow_wrap;
  child.text_indent = parent.text_indent;
  child.tab_size = parent.tab_size;
  child.visibility = parent.visibility;
  child.pointer_events = parent.pointer_events;
  if (with_custom_properties) {
    // Custom properties inherit, which is the entire basis of how a modern stylesheet is written:
    // set on `:root` once, referenced everywhere below.
    child.custom_properties = parent.custom_properties;
  }
}


namespace {

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
std::optional<Length> ParseBackgroundPosition(std::string_view word, bool horizontal,
                                              const MediaContext& context, float root_font_size) {
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
  return ParseLength(word, context, root_font_size);
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
      return value + offset;
    case Unit::Em:
      return value * font_size + offset;
    case Unit::Rem:
      // The root font size is a constant until a root style exists to ask; see
      // kRootFontSize, which the `calc()` evaluator folds a `rem` with too.
      return value * kRootFontSize + offset;
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

std::optional<Length> ParseLength(std::string_view text, const MediaContext& context,
                                  float root_font_size) {
  const std::string lowered = Lowered(Trim(text));
  if (lowered.empty()) {
    return std::nullopt;
  }
  if (lowered == "auto") {
    return Length::Auto();
  }
  // Every property that takes a length takes a math function of one, so the
  // funnel is here rather than at each of them. A calc/min/max/clamp this
  // engine cannot represent returns nullopt and drops its declaration, exactly
  // as an unknown unit does. Top-level `max(...)` is what wikipedia writes
  // alongside `calc(max(...))`.
  if (lowered.compare(0, 5, "calc(") == 0 || lowered.compare(0, 4, "min(") == 0 ||
      lowered.compare(0, 4, "max(") == 0 || lowered.compare(0, 6, "clamp(") == 0) {
    return ParseCalc(lowered, context, root_font_size);
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
  // `ex` and `ch` are font-relative and this cascade has a font *size* but no font *metrics* --
  // the face is chosen in `src/gfx` at paint time and the cascade cannot see it. CSS Values 4 says
  // what to do about exactly that: when the x-height (or the advance of `0`) cannot be determined,
  // assume 0.5em. That is a specified fallback rather than a guess, and it is the difference
  // between `width: 50ch` being half a wrong width and being no width at all.
  if (unit == "ex" || unit == "ch") {
    return Length{static_cast<float>(value * 0.5), Length::Unit::Em};
  }
  if (unit == "rem") {
    // Absolutized here, at computed-value time, which is where CSS Values says a font-relative
    // length becomes an absolute one. Carrying `Unit::Rem` into layout would mean layout had to
    // know the root's font size, and layout does not see the cascade.
    return Length::Pixels(static_cast<float>(value) * root_font_size);
  }
  if (unit == "pt") {
    // 1pt is 4/3 px, which is the one absolute unit conversion a browser
    // actually needs.
    return Length::Pixels(static_cast<float>(value * 4.0 / 3.0));
  }
  if (const std::optional<float> absolute = AbsoluteLengthFromUnit(value, unit, context)) {
    return Length::Pixels(*absolute);
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
                bool allow_percent, const MediaContext& context, float root_font_size) {
  const std::vector<std::string_view> parts = SplitWords(value);
  if (parts.empty() || parts.size() > 4) {
    return false;
  }
  std::array<Length, 4> lengths;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto length = ParseLength(parts[i], context, root_font_size);
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

bool ApplyBorder(std::string_view value, ComputedStyle& style, const MediaContext& context) {
  const float root_font_size = style.root_font_size;
  const std::vector<std::string_view> parts = SplitWords(value);
  if (parts.empty()) {
    return false;
  }

  std::optional<Edges> width;
  std::optional<gfx::Color> color;
  bool saw_style = false;
  bool style_disables_border = false;
  for (const std::string_view part : parts) {
    if (const auto length = ParseLength(part, context, root_font_size)) {
      if (width.has_value() || !EdgeLengthAllowed(*length, false, false, false)) {
        return false;
      }
      width = Edges{*length, *length, *length, *length};
    } else if (const auto parsed_color = ParseColor(part)) {
      if (color.has_value()) {
        return false;
      }
      color = *parsed_color;
    } else {
      const std::string lowered = Lowered(part);
      if (!IsBorderStyleKeyword(lowered)) {
        return false;
      }
      if (saw_style) {
        return false;
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
  return true;
}

}  // namespace

bool ApplyDeclaration(std::string_view property, std::string_view raw_value,
                      const ComputedStyle& parent, ComputedStyle& style,
                      const MediaContext& context) {
  // Lowered only when it has to be. A CSS value is nearly always already
  // lower case, and this used to allocate and copy one per applied declaration
  // -- 393,210 of them on en.wikipedia.org/wiki/CSS, for a string that in the
  // overwhelming majority of cases came back identical.
  std::string lowered;
  const std::string_view value = LoweredIfNeeded(raw_value, lowered);

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
    } else {
      // `grid`, `inline-grid`, `contents`, `table-caption`'s missing cousins.
      // Answering no here is what makes `@supports (display: grid)` false, and
      // it is why this chain has no default case that shrugs.
      return false;
    }
    return true;
  }
  if (property == "content") {
    // Only the forms `::before`/`::after` need for the aspect-ratio hack and
    // "generate nothing". Quoted empty string is Empty; `none`/`normal` suppress
    // the box. Anything richer is refused rather than approximated (ADR 0012).
    if (value == "none" || value == "normal") {
      style.content = ComputedStyle::Content::None;
      return true;
    }
    if (value == "\"\"" || value == "''") {
      style.content = ComputedStyle::Content::Empty;
      return true;
    }
    return false;
  }
  if (property == "color") {
    const auto color = ParseColor(raw_value);
    if (!color.has_value()) {
      return false;
    }
    style.color = *color;
    return true;
  }
  if (property == "background-color") {
    const auto color = ParseColor(raw_value);
    if (!color.has_value()) {
      return false;
    }
    style.background_color = *color;
    return true;
  }
  if (property == "background") {
    // The shorthand, read for the parts this renderer has. Every longhand it
    // sets is reset first, because a shorthand sets *all* of its longhands --
    // a `background: red` after a `background-image` must clear the image, and
    // one that only assigned what it recognised would leave it behind.
    style.background_color = gfx::Color::Transparent();
    style.background.image.clear();
    style.background.repeat = BackgroundRepeat::Repeat;
    if (const auto color = ParseColor(raw_value)) {
      style.background_color = *color;
      return true;
    }
    // A layer list: `url(a), linear-gradient(...)`. The first layer that is an
    // image this renderer can fetch wins; a gradient layer is skipped rather
    // than approximated, and a value that is nothing but gradients leaves the
    // element with no background at all -- which is what it looks like here,
    // and is more honest than a flat colour nobody chose.
    bool understood = false;
    for (const std::string_view layer : SplitTopLevel(raw_value, ',')) {
      if (std::string url = ParseUrlFunction(layer); !url.empty()) {
        style.background.image = std::move(url);
        understood = true;
        break;
      }
    }
    for (const std::string_view word : SplitWords(raw_value)) {
      if (const std::optional<BackgroundRepeat> repeat = ParseBackgroundRepeat(word)) {
        style.background.repeat = *repeat;
        understood = true;
      } else if (const auto color = ParseColor(word)) {
        style.background_color = *color;
        understood = true;
      }
    }
    // A value made entirely of things this renderer does not have -- a
    // `linear-gradient()`, say -- is reported as unsupported rather than as an
    // element with no background, because that is the honest answer and because
    // it is the one a page's `@supports` fallback is waiting for.
    return understood;
  }
  if (property == "background-image") {
    style.background.image.clear();
    for (const std::string_view layer : SplitTopLevel(raw_value, ',')) {
      if (std::string url = ParseUrlFunction(layer); !url.empty()) {
        style.background.image = std::move(url);
        return true;
      }
    }
    return Lowered(Trim(raw_value)) == "none";
  }
  if (property == "background-repeat") {
    // Two values are the per-axis form; one applies to both. Only the first is
    // read, because `repeat no-repeat` is spelled `repeat-x` far more often
    // and the pair adds a second way to say the same four things.
    const std::vector<std::string_view> words = SplitWords(raw_value);
    if (words.empty()) {
      return false;
    }
    const std::optional<BackgroundRepeat> repeat = ParseBackgroundRepeat(words.front());
    if (!repeat.has_value()) {
      return false;
    }
    style.background.repeat = *repeat;
    return true;
  }
  if (property == "background-size") {
    // `contain` and `cover` are not lengths and are not supported: both need
    // the image's aspect ratio, which the cascade does not have. A single
    // length applies to the width and leaves the height automatic, which is
    // what keeps an icon's proportions.
    const std::vector<std::string_view> words = SplitWords(raw_value);
    if (words.empty() || words.size() > 2) {
      return false;
    }
    const std::optional<Length> first = ParseLength(words.front(), context, style.root_font_size);
    if (!first.has_value()) {
      return false;  // `contain` and `cover` land here, and are honestly a no
    }
    const std::optional<Length> second =
        words.size() == 2 ? ParseLength(words[1], context, style.root_font_size) : std::optional<Length>(Length::Auto());
    if (!second.has_value()) {
      return false;
    }
    style.background.size_x = *first;
    style.background.size_y = *second;
    return true;
  }
  if (property == "background-position") {
    const std::vector<std::string_view> words = SplitWords(raw_value);
    if (words.empty() || words.size() > 2) {
      return false;
    }
    // A single value sets the horizontal position and centres the vertical,
    // which is what CSS says and is the difference between an icon on the left
    // edge and one halfway down it.
    const std::optional<Length> x = ParseBackgroundPosition(words.front(), true, context, style.root_font_size);
    const std::optional<Length> y =
        words.size() == 2 ? ParseBackgroundPosition(words[1], false, context, style.root_font_size)
                          : std::optional<Length>(Length{50.0f, Length::Unit::Percent});
    if (!x.has_value() || !y.has_value()) {
      return false;
    }
    style.background.position_x = *x;
    style.background.position_y = *y;
    return true;
  }
  if (property == "font-size") {
    // Resolved to absolute pixels during the cascade, because `em` on every
    // other property is relative to it — and `font-size: 2em` is relative to
    // the *parent's* size, not its own.
    if (value == "smaller") {
      style.font_size = parent.font_size * 0.8f;
      return true;
    }
    if (value == "larger") {
      style.font_size = parent.font_size * 1.25f;
      return true;
    }
    const auto length = ParseLength(raw_value, context, parent.root_font_size);
    if (!length.has_value()) {
      return false;
    }
    float resolved = -1.0f;
    if (length->unit == Length::Unit::Percent) {
      resolved = parent.font_size * length->value / 100.0f + length->offset;
    } else if (length->unit == Length::Unit::Em) {
      resolved = parent.font_size * length->value + length->offset;
    } else if (!length->IsAuto()) {
      resolved = length->Resolve(parent.font_size);
    }
    if (resolved < 0.0f) {
      return false;
    }
    style.font_size = std::clamp(resolved, 1.0f, 1000.0f);
    return true;
  }
  if (property == "font-weight") {
    if (value == "bold") {
      style.font_weight = 700.0f;
      return true;
    }
    if (value == "normal") {
      style.font_weight = 400.0f;
      return true;
    }
    const auto weight = util::ParseInt(value);
    if (!weight.has_value() || *weight < 1 || *weight > 1000) {
      return false;  // `bolder` and `lighter` are relative to the parent, and are not done
    }
    style.font_weight = static_cast<float>(*weight);
    return true;
  }
  // The flex properties and the sizing bounds, in their own translation unit.
  // Not a stylistic split: this file is at its module's line cap, and the cap
  // means a missing module rather than a bigger file. These are the properties
  // that arrived together and are read together, so they are the ones that
  // move.
  if (ApplyTransitionDeclaration(property, value, style)) {
    return true;
  }
  if (ApplyAnimationDeclaration(property, value, style)) {
    return true;
  }
  if (ApplyTransformDeclaration(property, value, parent, style)) {
    return true;
  }
  if (ApplyBoxDeclaration(property, value, parent, style, context)) {
    return true;
  }

  if (property == "float") {
    if (value == "left") {
      style.css_float = Float::Left;
    } else if (value == "right") {
      style.css_float = Float::Right;
    } else if (value == "none") {
      style.css_float = Float::None;
    } else {
      return false;
    }
    return true;
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
    } else {
      return false;
    }
    return true;
  }
  if (property == "font-style") {
    if (value == "italic" || value == "oblique") {
      style.font_style = FontStyle::Italic;
    } else if (value == "normal") {
      style.font_style = FontStyle::Normal;
    } else {
      return false;
    }
    return true;
  }
  if (property == "font-family") {
    // A value that parses to nothing (`font-family: ,`) leaves the inherited
    // list alone rather than clearing it, which is what an invalid declaration
    // is supposed to do.
    std::vector<std::string> families = ParseFontFamilyList(raw_value);
    if (families.empty()) {
      return false;
    }
    style.font_family = std::move(families);
    return true;
  }
  if (property == "line-height") {
    if (value == "normal") {
      style.line_height = 0.0f;
      return true;
    }
    if (const auto length = ParseLength(raw_value, context, style.root_font_size)) {
      const float resolved = length->unit == Length::Unit::Percent
                                 ? style.font_size * length->value / 100.0f + length->offset
                                 : length->Resolve(style.font_size, -1.0f);
      if (resolved < 0.0f) {
        return false;
      }
      style.line_height = resolved;
      return true;
    }
    const auto multiple = util::ParseDouble(value);
    if (!multiple.has_value() || *multiple < 0.0) {
      return false;
    }
    style.line_height = static_cast<float>(*multiple) * style.font_size;
    return true;
  }
  if (property == "text-align") {
    // Every *valid* value also settles whether block children are centred, so
    // that `text-align: left` after a <center> undoes all of it rather than
    // half. An invalid one settles nothing: it is not a declaration.
    if (value == "center") {
      style.text_align = TextAlign::Center;
    } else if (value == "right") {
      style.text_align = TextAlign::Right;
    } else if (value == "justify") {
      style.text_align = TextAlign::Justify;
    } else if (value == "left") {
      style.text_align = TextAlign::Left;
    } else if (value == "start") {
      style.text_align = TextAlign::Start;
    } else if (value == "end") {
      style.text_align = TextAlign::End;
    } else if (value == "justify-all") {
      // `justify` plus `text-align-all`, which this engine does not have. The alignment of every
      // line but the last is the half it can honour, and that half is `justify`.
      style.text_align = TextAlign::Justify;
    } else if (value == "match-parent") {
      // Computes to the parent's value, with `start`/`end` resolved against the *parent's*
      // direction -- which is the whole point of the keyword: it is `inherit` for an element whose
      // own direction differs.
      const bool parent_rtl = parent.direction == Direction::Rtl;
      style.text_align = parent.text_align == TextAlign::Start
                             ? (parent_rtl ? TextAlign::Right : TextAlign::Left)
                         : parent.text_align == TextAlign::End
                             ? (parent_rtl ? TextAlign::Left : TextAlign::Right)
                             : parent.text_align;
    } else if (value == "-microbrowser-center") {
      // What <center> means, and what no standard value expresses. See the
      // note on ComputedStyle::centers_block_children.
      style.text_align = TextAlign::Center;
      style.centers_block_children = true;
      return true;
    } else {
      return false;
    }
    style.centers_block_children = false;
    return true;
  }
  if (property == "direction") {
    // ADR 0025 §3. Inherited, so setting it on <html> is what makes a whole document right-to-left --
    // which is how every real Arabic and Hebrew page does it.
    if (value == "ltr") {
      style.direction = Direction::Ltr;
    } else if (value == "rtl") {
      style.direction = Direction::Rtl;
    } else {
      return false;
    }
    return true;
  }
  if (property == "unicode-bidi") {
    if (value == "normal") {
      style.unicode_bidi = UnicodeBidi::Normal;
    } else if (value == "embed") {
      style.unicode_bidi = UnicodeBidi::Embed;
    } else if (value == "isolate") {
      style.unicode_bidi = UnicodeBidi::Isolate;
    } else if (value == "bidi-override") {
      style.unicode_bidi = UnicodeBidi::BidiOverride;
    } else if (value == "isolate-override") {
      style.unicode_bidi = UnicodeBidi::IsolateOverride;
    } else if (value == "plaintext") {
      style.unicode_bidi = UnicodeBidi::Plaintext;
    } else {
      return false;
    }
    return true;
  }
  // The white-space family and the rest of CSS Text lives in TextDeclarations.cpp, for the reason
  // `transform` has its own translation unit: `white-space` is a shorthand with two orthogonal
  // longhands and an order-free grammar, which is not the shape of the keyword switches here.
  if (ApplyTextDeclaration(property, value, parent, style, context)) {
    return true;
  }
  if (property == "margin") {
    return ApplyEdges(raw_value, style.margin, true, true, true, context, style.root_font_size);
  }
  if (property == "padding") {
    return ApplyEdges(raw_value, style.padding, false, false, true, context, style.root_font_size);
  }
  if (property == "width" || property == "height") {
    const auto length = ParseLength(raw_value, context, style.root_font_size);
    if (!length.has_value() || !EdgeLengthAllowed(*length, false, true, true)) {
      return false;
    }
    (property == "width" ? style.width : style.height) = *length;
    return true;
  }
  if (property == "border-color") {
    const auto color = ParseColor(raw_value);
    if (!color.has_value()) {
      return false;
    }
    style.border_color = *color;
    style.has_border = true;
    return true;
  }
  if (property == "border-width") {
    if (!ApplyEdges(raw_value, style.border_width, false, false, false, context, style.root_font_size)) {
      return false;
    }
    style.has_border = true;
    return true;
  }
  if (property == "border") {
    return ApplyBorder(raw_value, style, context);
  }

  // Individual edge properties. Written as a loop rather than sixteen branches.
  static constexpr std::array<std::string_view, 4> kSides = {"top", "right", "bottom", "left"};
  for (std::size_t i = 0; i < kSides.size(); ++i) {
    const std::string margin_name = "margin-" + std::string(kSides[i]);
    const std::string padding_name = "padding-" + std::string(kSides[i]);
    if (property == margin_name || property == padding_name) {
      const auto length = ParseLength(raw_value, context, style.root_font_size);
      const bool is_margin = property == margin_name;
      if (!length.has_value() ||
          !EdgeLengthAllowed(*length, is_margin, is_margin, true)) {
        return false;
      }
      Edges& edges = is_margin ? style.margin : style.padding;
      (&edges.top)[i] = *length;
      return true;
    }
  }
  // Anything else is a property we do not implement. Ignored rather than
  // guessed at -- and said so, which is the whole of what `@supports` reads.
  return false;
}

bool ApplyDeclaration(const Declaration& declaration, const ComputedStyle& parent,
                      ComputedStyle& style, const MediaContext& context) {
  return ApplyDeclaration(declaration.property, declaration.value, parent, style, context);
}

bool SupportsDeclaration(std::string_view property, std::string_view value,
                         const MediaContext& context) {
  static constexpr MediaContext kProbe{100.0f, 100.0f, 1.0f};
  const MediaContext& resolved =
      context.viewport_width == 0.0f && context.viewport_height == 0.0f ? kProbe : context;
  const std::string name = Lowered(Trim(property));
  if (name.rfind("--", 0) == 0) {
    // A custom property has no grammar to fail: any token stream is a legal
    // value, so `@supports (--x: anything)` is true wherever they exist at all.
    return !Trim(value).empty();
  }
  const ComputedStyle initial;
  ComputedStyle scratch;
  return ApplyDeclaration(name, Trim(value), initial, scratch, resolved);
}

}  // namespace microbrowser::css

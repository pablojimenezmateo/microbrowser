#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/CssText.h"
#include "css/StyleSheet.h"
#include "util/Parse.h"

// The flex properties, the sizing bounds, and the border.
//
// Split from Declarations.cpp because that file reached its module's line cap,
// and the cap is written to mean a missing module rather than a bigger file.
// These are the right ones to move: they arrived together, they are read
// together by flex layout, and none of the older properties refers to them.
//
// The border moved here for the same reason and is the second thing in the file
// rather than the first: it is twenty-one properties -- three shorthands, three
// four-value shorthands and twelve longhands -- and every one of them is the
// same three parsers behind a different set of sides.

namespace microbrowser::css {

namespace {

// `justify-content` and `align-content` take the same six values, and the two
// spellings of the CSS Box Alignment module are accepted alongside the flexbox
// ones: a page writes `start` as often as `flex-start`.
std::optional<Distribution> ParseDistribution(std::string_view value) {
  if (value == "flex-start" || value == "start" || value == "left" || value == "normal") {
    return Distribution::FlexStart;
  }
  if (value == "flex-end" || value == "end" || value == "right") {
    return Distribution::FlexEnd;
  }
  if (value == "center") {
    return Distribution::Center;
  }
  if (value == "space-between") {
    return Distribution::SpaceBetween;
  }
  if (value == "space-around") {
    return Distribution::SpaceAround;
  }
  if (value == "space-evenly") {
    return Distribution::SpaceEvenly;
  }
  if (value == "stretch") {
    return Distribution::Stretch;
  }
  return std::nullopt;
}

std::optional<Alignment> ParseAlignment(std::string_view value) {
  if (value == "auto") {
    return Alignment::Auto;
  }
  if (value == "stretch" || value == "normal") {
    return Alignment::Stretch;
  }
  if (value == "flex-start" || value == "start" || value == "self-start") {
    return Alignment::FlexStart;
  }
  if (value == "flex-end" || value == "end" || value == "self-end") {
    return Alignment::FlexEnd;
  }
  if (value == "center") {
    return Alignment::Center;
  }
  if (value == "baseline") {
    return Alignment::Baseline;
  }
  return std::nullopt;
}

// `border-width`'s three keywords, which are lengths with names. The pixel figures are not in the
// specification -- it says only that they are increasing and that `medium` is the initial value --
// and 1/3/5 is what every engine ships.
std::optional<Length> ParseBorderWidth(std::string_view word, const MediaContext& context,
                                       float root_font_size) {
  if (word == "thin") {
    return Length::Pixels(1.0f);
  }
  if (word == "medium") {
    return Length::Pixels(3.0f);
  }
  if (word == "thick") {
    return Length::Pixels(5.0f);
  }
  const std::optional<Length> length = ParseLength(word, context, root_font_size);
  if (!length.has_value() || length->IsAuto() || length->IsPercent() || length->value < 0.0f) {
    // A border width is a non-negative length and never a percentage: `border-width: 50%` is not a
    // half-width border, it is an invalid declaration.
    return std::nullopt;
  }
  return length;
}

std::optional<BorderStyle> ParseBorderStyle(std::string_view word) {
  static constexpr std::pair<std::string_view, BorderStyle> kStyles[] = {
      {"none", BorderStyle::None},     {"hidden", BorderStyle::Hidden},
      {"dotted", BorderStyle::Dotted}, {"dashed", BorderStyle::Dashed},
      {"solid", BorderStyle::Solid},   {"double", BorderStyle::Double},
      {"groove", BorderStyle::Groove}, {"ridge", BorderStyle::Ridge},
      {"inset", BorderStyle::Inset},   {"outset", BorderStyle::Outset},
  };
  for (const auto& [name, style] : kStyles) {
    if (word == name) {
      return style;
    }
  }
  return std::nullopt;
}

// A `border`/`border-<side>` shorthand: up to one width, one style and one colour, in any order.
//
// **A component the declaration leaves out is reset to its initial value**, which is what makes
// `border: 1in` paint nothing: it sets a width and resets the style to `none`. Getting that wrong
// is not a subtlety -- css/CSS2/borders is full of `border-bottom: 1in` followed by
// `border-bottom-style: solid`, and a shorthand that left the style alone would paint the first
// one too.
bool ApplyBorderShorthand(std::string_view value, ComputedStyle& style, const MediaContext& context,
                          std::size_t first_side, std::size_t side_count) {
  const std::vector<std::string_view> parts = SplitWords(value);
  if (parts.empty() || parts.size() > 3) {
    return false;
  }
  std::optional<Length> width;
  std::optional<BorderStyle> line;
  std::optional<gfx::Color> color;
  bool saw_color = false;
  for (const std::string_view part : parts) {
    if (const std::optional<BorderStyle> as_style = ParseBorderStyle(part)) {
      if (line.has_value()) {
        return false;
      }
      line = as_style;
    } else if (const std::optional<Length> as_width =
                   ParseBorderWidth(part, context, style.root_font_size)) {
      if (width.has_value()) {
        return false;
      }
      width = as_width;
    } else if (const std::optional<gfx::Color> as_color = ParseColor(part)) {
      if (saw_color) {
        return false;
      }
      saw_color = true;
      color = as_color;
    } else {
      return false;  // one bad component invalidates the whole declaration
    }
  }
  for (std::size_t i = first_side; i < first_side + side_count; ++i) {
    style.border_width[i] = width.value_or(Length::Pixels(3.0f));
    style.border_style[i] = line.value_or(BorderStyle::None);
    style.border_color[i] = color;
  }
  return true;
}

// One of the three four-value shorthands: `border-width`, `border-style`, `border-color`. One to
// four components in the CSS box order, and `parse` says which of the three this is.
template <typename Sides, typename Parse>
bool ApplyBorderSides(std::string_view value, Sides& sides, Parse parse) {
  using T = std::decay_t<decltype(sides[0])>;
  const std::vector<std::string_view> parts = SplitWords(value);
  if (parts.empty() || parts.size() > 4) {
    return false;
  }
  T parsed[4];
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto one = parse(parts[i]);
    if (!one.has_value()) {
      return false;
    }
    parsed[i] = *one;
  }
  // `a` / `a b` / `a b c` / `a b c d` -> top right bottom left.
  const T& top = parsed[0];
  const T& right = parts.size() >= 2 ? parsed[1] : parsed[0];
  const T& bottom = parts.size() >= 3 ? parsed[2] : parsed[0];
  const T& left = parts.size() >= 4 ? parsed[3] : right;
  sides[0] = top;
  sides[1] = right;
  sides[2] = bottom;
  sides[3] = left;
  return true;
}

constexpr std::string_view kSideNames[4] = {"top", "right", "bottom", "left"};

// Every `border*` property, or false when `property` is not one.
bool ApplyBorderDeclaration(std::string_view property, std::string_view value,
                            ComputedStyle& style, const MediaContext& context) {
  if (property.rfind("border", 0) != 0) {
    return false;
  }
  if (property == "border") {
    return ApplyBorderShorthand(value, style, context, 0, 4);
  }
  if (property == "border-width") {
    return ApplyBorderSides(value, style.border_width,
                            [&](std::string_view word) {
                              return ParseBorderWidth(word, context, style.root_font_size);
                            });
  }
  if (property == "border-style") {
    return ApplyBorderSides(value, style.border_style, ParseBorderStyle);
  }
  if (property == "border-color") {
    return ApplyBorderSides(value, style.border_color,
                            [](std::string_view word) -> std::optional<std::optional<gfx::Color>> {
                              // `currentColor` is the initial value and stays unresolved: the
                              // element's `color` may be set by a later declaration in the same
                              // rule, and the painter asks for it then.
                              if (word == "currentcolor") {
                                return std::optional<gfx::Color>{};
                              }
                              if (const std::optional<gfx::Color> parsed = ParseColor(word)) {
                                return std::optional<gfx::Color>{*parsed};
                              }
                              return std::nullopt;
                            });
  }
  for (std::size_t side = 0; side < 4; ++side) {
    const std::string prefix = "border-" + std::string(kSideNames[side]);
    if (property == prefix) {
      return ApplyBorderShorthand(value, style, context, side, 1);
    }
    if (property == prefix + "-width") {
      const std::optional<Length> width = ParseBorderWidth(value, context, style.root_font_size);
      if (!width.has_value()) {
        return false;
      }
      style.border_width[side] = *width;
      return true;
    }
    if (property == prefix + "-style") {
      const std::optional<BorderStyle> line = ParseBorderStyle(value);
      if (!line.has_value()) {
        return false;
      }
      style.border_style[side] = *line;
      return true;
    }
    if (property == prefix + "-color") {
      if (value == "currentcolor") {
        style.border_color[side] = std::nullopt;
        return true;
      }
      const std::optional<gfx::Color> color = ParseColor(value);
      if (!color.has_value()) {
        return false;
      }
      style.border_color[side] = *color;
      return true;
    }
  }
  return false;
}

}  // namespace

bool ApplyBoxDeclaration(std::string_view property, std::string_view value,
                         const ComputedStyle& parent, ComputedStyle& style,
                         const MediaContext& context) {
  if (ApplyBorderDeclaration(property, value, style, context)) {
    return true;
  }
  if (property == "position") {
    if (value == "static") {
      style.position = Position::Static;
    } else if (value == "relative") {
      style.position = Position::Relative;
    } else if (value == "absolute") {
      style.position = Position::Absolute;
    } else if (value == "fixed") {
      style.position = Position::Fixed;
    } else if (value == "sticky") {
      style.position = Position::Sticky;
    } else {
      return false;
    }
    return true;
  }
  if (property == "z-index") {
    if (value == "auto") {
      style.z_index = std::nullopt;
      return true;
    }
    // An integer, and only an integer: `z-index: 1.5` is invalid, and rounding it
    // would put a box in a layer the author did not write.
    const std::optional<int> parsed = util::ParseInt(value);
    if (!parsed.has_value()) {
      return false;
    }
    style.z_index = *parsed;
    return true;
  }
  if (property == "visibility") {
    if (value == "visible") {
      style.visibility = Visibility::Visible;
    } else if (value == "hidden" || value == "collapse") {
      // `collapse` is table-specific; until rows collapse it is `hidden`.
      style.visibility = Visibility::Hidden;
    } else if (value == "inherit") {
      style.visibility = parent.visibility;
    } else {
      return false;
    }
    return true;
  }
  if (property == "opacity") {
    if (value == "inherit") {
      style.opacity = parent.opacity;
      return true;
    }
    // Number or percentage, clamped to [0, 1]. `opacity: 2` is valid CSS and
    // means 1; junk is refused so `@supports` stays honest.
    std::string_view number = value;
    float scale = 1.0f;
    if (!number.empty() && number.back() == '%') {
      number.remove_suffix(1);
      scale = 0.01f;
    }
    const std::optional<double> parsed = util::ParseDouble(number);
    if (!parsed.has_value()) {
      return false;
    }
    style.opacity =
        static_cast<float>(std::clamp(*parsed * static_cast<double>(scale), 0.0, 1.0));
    return true;
  }
  if (property == "pointer-events") {
    if (value == "auto" || value == "visiblepainted" || value == "visiblefill" ||
        value == "visiblestroke" || value == "visible" || value == "painted" ||
        value == "fill" || value == "stroke" || value == "all") {
      // HTML hit-testing only distinguishes none vs not-none here.
      style.pointer_events = PointerEvents::Auto;
    } else if (value == "none") {
      style.pointer_events = PointerEvents::None;
    } else if (value == "inherit") {
      style.pointer_events = parent.pointer_events;
    } else {
      return false;
    }
    return true;
  }
  if (property == "overflow" || property == "overflow-x" || property == "overflow-y") {
    Overflow parsed = Overflow::Visible;
    if (value == "hidden" || value == "clip") {
      parsed = Overflow::Hidden;
    } else if (value == "scroll") {
      parsed = Overflow::Scroll;
    } else if (value == "auto") {
      parsed = Overflow::Auto;
    } else if (value != "visible") {
      return false;  // an unrecognized value is a dropped declaration
    }
    if (property != "overflow-y") {
      style.overflow_x = parsed;
    }
    if (property != "overflow-x") {
      style.overflow_y = parsed;
    }
    return true;
  }
  if (property == "top" || property == "right" || property == "bottom" ||
      property == "left") {
    // `auto` is the initial value and has to be settable back, because a later
    // rule undoing an earlier one is ordinary cascade.
    Length parsed = Length::Auto();
    if (value != "auto") {
      const std::optional<Length> length = ParseLength(value, context, style.root_font_size);
      if (!length.has_value()) {
        return false;
      }
      parsed = *length;
    }
    if (property == "top") {
      style.inset.top = parsed;
    } else if (property == "right") {
      style.inset.right = parsed;
    } else if (property == "bottom") {
      style.inset.bottom = parsed;
    } else {
      style.inset.left = parsed;
    }
    return true;
  }
  if (property == "inset") {
    // One to four values, in the order every other edge shorthand uses.
    const std::vector<std::string_view> parts = SplitWords(value);
    static constexpr const char* kSides[] = {"top", "right", "bottom", "left"};
    if (parts.empty() || parts.size() > 4) {
      return false;
    }
    bool applied = true;
    for (std::size_t side = 0; side < 4; ++side) {
      const std::size_t at = parts.size() == 1   ? 0
                             : parts.size() == 2 ? side % 2
                             : parts.size() == 3 ? (side == 3 ? 1 : side)
                                                 : side;
      applied = ApplyBoxDeclaration(kSides[side], std::string(parts[at]), parent, style, context) &&
                applied;
    }
    return applied;
  }
  if (property == "min-width" || property == "max-width" || property == "min-height" ||
      property == "max-height") {
    // `none` is the maximum's way of saying unbounded, and `auto` is the
    // minimum's way of saying "whatever the content needs" -- which for a
    // block is zero. Both land on Length::Auto, which is what the clamp reads
    // as "no bound".
    css::Length parsed = Length::Auto();
    if (value != "none" && value != "auto") {
      const std::optional<Length> length = ParseLength(value, context, style.root_font_size);
      if (!length.has_value()) {
        return false;
      }
      parsed = *length;
    }
    if (property == "min-width") {
      style.min_width = parsed;
    } else if (property == "max-width") {
      style.max_width = parsed;
    } else if (property == "min-height") {
      style.min_height = parsed;
    } else {
      style.max_height = parsed;
    }
    return true;
  }
  if (property == "box-sizing") {
    if (value == "border-box") {
      style.box_sizing = BoxSizing::BorderBox;
      return true;
    }
    if (value == "content-box") {
      style.box_sizing = BoxSizing::ContentBox;
      return true;
    }
    return false;
  }

  if (property == "aspect-ratio") {
    // `auto`, one number, or `w / h`. The `auto <ratio>` form of the spec --
    // a replaced element's own ratio, falling back to the stated one -- is not
    // here: nothing yet asks an image for its ratio separately from its size,
    // and accepting the spelling while ignoring the `auto` half would be a
    // value that means something different from what it says.
    if (value == "auto") {
      style.aspect_ratio = 0.0f;
      return true;
    }
    // `16/9` is one word and `16 / 9` is three, because the tokenizer keeps the
    // whitespace it was written with. Joining the words first means the two
    // spellings reach the same parser rather than two that can disagree.
    std::string joined;
    for (const std::string_view part : SplitWords(value)) {
      joined += part;
    }
    const std::size_t slash = joined.find('/');
    const std::string_view numerator(joined.data(), std::min(slash, joined.size()));
    const std::optional<double> first = util::ParseDouble(numerator);
    if (!first.has_value() ||
        (slash != std::string::npos && joined.find('/', slash + 1) != std::string::npos)) {
      return false;
    }
    const std::optional<double> second =
        slash == std::string::npos
            ? std::optional<double>(1.0)
            : util::ParseDouble(std::string_view(joined).substr(slash + 1));
    if (!second.has_value()) {
      return false;
    }
    const double width = *first;
    const double height = *second;
    // A degenerate ratio is not a ratio. Zero on either side is `auto` per the
    // specification, and a negative one is invalid; both come out here as "no
    // preferred ratio", which is what the layout reads a zero as.
    if (!(width > 0.0) || !(height > 0.0) || !(width / height < 1e6)) {
      return width >= 0.0 && height >= 0.0;
    }
    style.aspect_ratio = static_cast<float>(width / height);
    return true;
  }

  // --- Flex -----------------------------------------------------------------
  // The container's five, the item's five, and the two shorthands that set
  // three properties at once.
  if (property == "flex-direction") {
    if (value == "row") {
      style.flex.direction = FlexDirection::Row;
    } else if (value == "row-reverse") {
      style.flex.direction = FlexDirection::RowReverse;
    } else if (value == "column") {
      style.flex.direction = FlexDirection::Column;
    } else if (value == "column-reverse") {
      style.flex.direction = FlexDirection::ColumnReverse;
    } else {
      return false;
    }
    return true;
  }
  if (property == "flex-wrap") {
    if (value == "nowrap") {
      style.flex.wrap = FlexWrap::NoWrap;
    } else if (value == "wrap") {
      style.flex.wrap = FlexWrap::Wrap;
    } else if (value == "wrap-reverse") {
      style.flex.wrap = FlexWrap::WrapReverse;
    } else {
      return false;
    }
    return true;
  }
  if (property == "flex-flow") {
    // Direction and wrap in either order, which is all this shorthand is.
    // Each part is offered to both, and the one it is not a value for ignores
    // it -- which is why neither needs to know what the other accepts.
    const std::vector<std::string_view> parts = SplitWords(value);
    if (parts.empty()) {
      return false;
    }
    bool applied = true;
    for (const std::string_view part : parts) {
      const bool direction = ApplyBoxDeclaration("flex-direction", std::string(part), parent,
                                                 style, context);
      const bool wrap = ApplyBoxDeclaration("flex-wrap", std::string(part), parent, style, context);
      applied = (direction || wrap) && applied;
    }
    return applied;
  }
  if (property == "justify-content" || property == "align-content") {
    const std::optional<Distribution> parsed = ParseDistribution(value);
    if (!parsed.has_value()) {
      return false;
    }
    if (property == "justify-content") {
      style.flex.justify_content = *parsed;
    } else {
      style.flex.align_content = *parsed;
    }
    return true;
  }
  if (property == "align-items" || property == "align-self") {
    const std::optional<Alignment> parsed = ParseAlignment(value);
    if (!parsed.has_value()) {
      return false;
    }
    if (property == "align-items") {
      // `auto` is only meaningful on align-self; on align-items it is not a
      // value at all, so it is dropped rather than stored.
      if (*parsed == Alignment::Auto) {
        return false;
      }
      style.flex.align_items = *parsed;
    } else {
      style.flex.align_self = *parsed;
    }
    return true;
  }
  if (property == "flex-grow" || property == "flex-shrink") {
    const std::optional<double> number = util::ParseDouble(value);
    if (!number.has_value() || *number < 0.0) {
      return false;  // negative is invalid, and an invalid declaration is dropped
    }
    (property == "flex-grow" ? style.flex.grow : style.flex.shrink) =
        static_cast<float>(*number);
    return true;
  }
  if (property == "flex-basis") {
    if (value == "auto" || value == "content") {
      style.flex.basis = Length::Auto();
    } else if (const std::optional<Length> length = ParseLength(value, context, style.root_font_size)) {
      style.flex.basis = *length;
    } else {
      return false;
    }
    return true;
  }
  if (property == "flex") {
    // `flex: 1` is grow 1, shrink 1, basis 0 -- and the basis is the part
    // people forget, which is why `flex: 1` fills its container and
    // `flex-grow: 1` does not.
    if (value == "none") {
      style.flex.grow = 0.0f;
      style.flex.shrink = 0.0f;
      style.flex.basis = Length::Auto();
      return true;
    }
    if (value == "initial") {
      style.flex = ComputedStyle::FlexStyle{};
      return true;
    }
    if (value == "auto") {
      style.flex.grow = 1.0f;
      style.flex.shrink = 1.0f;
      style.flex.basis = Length::Auto();
      return true;
    }
    const std::vector<std::string_view> parts = SplitWords(value);
    if (parts.empty() || parts.size() > 3) {
      return false;
    }
    bool applied = true;
    bool saw_basis = false;
    int numbers = 0;
    // A bare number is grow then shrink; anything with a unit is the basis.
    // `flex: 1 100px` and `flex: 100px 1` both mean what they look like.
    for (const std::string_view part : parts) {
      const bool numeric = util::ParseDouble(part).has_value();
      if (numeric && numbers < 2) {
        applied = ApplyBoxDeclaration(numbers == 0 ? "flex-grow" : "flex-shrink",
                                    std::string(part), parent, style, context) &&
                  applied;
        ++numbers;
        continue;
      }
      // Unitless zero as a flex-basis in the shorthand is `0%` (§7.1), so
      // `flex: 1 1 0` matches `flex: 1`.
      if (part == "0" || part == "+0" || part == "-0") {
        style.flex.basis = Length{0.0f, Length::Unit::Percent};
        saw_basis = true;
        continue;
      }
      applied = ApplyBoxDeclaration("flex-basis", std::string(part), parent, style, context) && applied;
      saw_basis = true;
    }
    if (numbers > 0 && !saw_basis) {
      // The one-value form: `flex: 1` sets the basis to `0%` (CSS Flexbox
      // §7.1.1), not `0px`. A percentage basis against an indefinite main size
      // is treated as `auto` during layout; absolute zero is not.
      style.flex.basis = Length{0.0f, Length::Unit::Percent};
    }
    if (numbers == 1) {
      style.flex.shrink = 1.0f;
    }
    return applied;
  }
  if (property == "gap" || property == "row-gap" || property == "column-gap") {
    const std::vector<std::string_view> parts = SplitWords(value);
    const auto pixels = [&style, &context](std::string_view text) -> std::optional<float> {
      const std::optional<Length> length = ParseLength(text, context, style.root_font_size);
      if (!length.has_value() || length->IsAuto() || length->IsPercent()) {
        return std::nullopt;  // a percentage gap resolves against a size we do not have yet
      }
      return length->Resolve(style.font_size);
    };
    if (parts.empty() || parts.size() > 2) {
      return false;
    }
    const std::optional<float> first = pixels(parts[0]);
    if (!first.has_value()) {
      return false;
    }
    if (property == "row-gap" || property == "column-gap") {
      (property == "row-gap" ? style.flex.row_gap : style.flex.column_gap) = *first;
      return parts.size() == 1;
    }
    // `gap: a` sets both; `gap: a b` is row then column.
    style.flex.row_gap = *first;
    style.flex.column_gap = *first;
    if (parts.size() == 2) {
      const std::optional<float> column = pixels(parts[1]);
      if (!column.has_value()) {
        return false;
      }
      style.flex.column_gap = *column;
    }
    return true;
  }
  if (property == "order") {
    const std::optional<int> number = util::ParseInt(value);
    if (!number.has_value()) {
      return false;
    }
    style.flex.order = *number;
    return true;
  }

  return false;
}

}  // namespace microbrowser::css

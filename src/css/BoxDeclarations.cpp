#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/CssText.h"
#include "css/StyleSheet.h"
#include "util/Parse.h"

// The flex properties and the sizing bounds.
//
// Split from Declarations.cpp because that file reached its module's line cap,
// and the cap is written to mean a missing module rather than a bigger file.
// These are the right ones to move: they arrived together, they are read
// together by flex layout, and none of the older properties refers to them.

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

}  // namespace

bool ApplyBoxDeclaration(const std::string& property, const std::string& value,
                         const ComputedStyle& parent, ComputedStyle& style) {
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
      const std::optional<Length> length = ParseLength(value);
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
      applied = ApplyBoxDeclaration(kSides[side], std::string(parts[at]), parent, style) &&
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
      const std::optional<Length> length = ParseLength(value);
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
                                                 style);
      const bool wrap = ApplyBoxDeclaration("flex-wrap", std::string(part), parent, style);
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
    } else if (const std::optional<Length> length = ParseLength(value)) {
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
                                      std::string(part), parent, style) &&
                  applied;
        ++numbers;
        continue;
      }
      applied = ApplyBoxDeclaration("flex-basis", std::string(part), parent, style) && applied;
      saw_basis = true;
    }
    if (numbers > 0 && !saw_basis) {
      // The one-value form: `flex: 1` sets the basis to zero, which is what
      // makes it fill rather than merely absorb.
      style.flex.basis = Length::Pixels(0.0f);
    }
    if (numbers == 1) {
      style.flex.shrink = 1.0f;
    }
    return applied;
  }
  if (property == "gap" || property == "row-gap" || property == "column-gap") {
    const std::vector<std::string_view> parts = SplitWords(value);
    const auto pixels = [&style](std::string_view text) -> std::optional<float> {
      const std::optional<Length> length = ParseLength(text);
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

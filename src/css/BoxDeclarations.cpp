#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
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

std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n')) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < text.size() && text[at] != ' ' && text[at] != '\t' && text[at] != '\n') {
      ++at;
    }
    if (at > begin) {
      words.push_back(text.substr(begin, at - begin));
    }
  }
  return words;
}

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
      // Sticky is relative until it would scroll out of view, and there is no
      // scroll position here to compare against. Relative is what it looks
      // like before it sticks, which is the right half to be wrong about.
      style.position = Position::Relative;
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
      return true;  // an unrecognized value is a dropped declaration
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
        return true;
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
    for (std::size_t side = 0; side < 4 && !parts.empty(); ++side) {
      const std::size_t at = parts.size() == 1   ? 0
                             : parts.size() == 2 ? side % 2
                             : parts.size() == 3 ? (side == 3 ? 1 : side)
                                                 : side;
      ApplyBoxDeclaration(kSides[side], std::string(parts[at]), parent, style);
    }
    return true;
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
        return true;
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
    }
    return true;
  }
  if (property == "flex-flow") {
    // Direction and wrap in either order, which is all this shorthand is.
    // Each part is offered to both, and the one it is not a value for ignores
    // it -- which is why neither needs to know what the other accepts.
    for (const std::string_view part : SplitWords(value)) {
      ApplyBoxDeclaration("flex-direction", std::string(part), parent, style);
      ApplyBoxDeclaration("flex-wrap", std::string(part), parent, style);
    }
    return true;
  }
  if (property == "justify-content" || property == "align-content") {
    const std::optional<Distribution> parsed = ParseDistribution(value);
    if (!parsed.has_value()) {
      return true;
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
      return true;
    }
    if (property == "align-items") {
      // `auto` is only meaningful on align-self; on align-items it is not a
      // value at all, so it is dropped rather than stored.
      if (*parsed != Alignment::Auto) {
        style.flex.align_items = *parsed;
      }
    } else {
      style.flex.align_self = *parsed;
    }
    return true;
  }
  if (property == "flex-grow" || property == "flex-shrink") {
    const std::optional<double> number = util::ParseDouble(value);
    if (!number.has_value() || *number < 0.0) {
      return true;  // negative is invalid, and an invalid declaration is dropped
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
    if (parts.empty()) {
      return true;
    }
    bool saw_basis = false;
    int numbers = 0;
    // A bare number is grow then shrink; anything with a unit is the basis.
    // `flex: 1 100px` and `flex: 100px 1` both mean what they look like.
    for (const std::string_view part : parts) {
      const bool numeric = util::ParseDouble(part).has_value();
      if (numeric && numbers < 2) {
        ApplyBoxDeclaration(numbers == 0 ? "flex-grow" : "flex-shrink", std::string(part),
                            parent, style);
        ++numbers;
        continue;
      }
      ApplyBoxDeclaration("flex-basis", std::string(part), parent, style);
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
    return true;
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
    if (property == "row-gap" || property == "column-gap") {
      if (const std::optional<float> size = parts.empty() ? std::nullopt : pixels(parts[0])) {
        (property == "row-gap" ? style.flex.row_gap : style.flex.column_gap) = *size;
      }
      return true;
    }
    // `gap: a` sets both; `gap: a b` is row then column.
    if (!parts.empty()) {
      if (const std::optional<float> row = pixels(parts[0])) {
        style.flex.row_gap = *row;
        style.flex.column_gap = *row;
      }
    }
    if (parts.size() >= 2) {
      if (const std::optional<float> column = pixels(parts[1])) {
        style.flex.column_gap = *column;
      }
    }
    return true;
  }
  if (property == "order") {
    if (const std::optional<long long> number = util::ParseInt(value)) {
      style.flex.order = static_cast<int>(*number);
    }
    return true;
  }

  return false;
}

}  // namespace microbrowser::css

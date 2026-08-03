#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/Color.h"

namespace microbrowser::css {

enum class Display : std::uint8_t {
  Inline,
  Block,
  InlineBlock,
  ListItem,
  Table,
  TableCaption,
  TableColumnGroup,
  TableColumn,
  TableHeaderGroup,
  TableFooterGroup,
  TableRowGroup,
  TableRow,
  TableCell,
  None,
};
enum class FontStyle : std::uint8_t { Normal, Italic };
enum class TextAlign : std::uint8_t { Left, Right, Center, Justify };
enum class WhiteSpace : std::uint8_t { Normal, Pre, NoWrap, PreWrap };

// Taken out of the normal flow and shifted to one side, with the following
// line boxes shortened around it. Not a display value: a float is a
// block-level box wherever it came from, which is why `float: left` on a span
// makes it a block.
enum class Float : std::uint8_t { None, Left, Right };

// Moves a box below the floats on the named side. `Both` is not the union of
// two decisions -- it is one decision about the lowest of them -- which is why
// it is a value here rather than two booleans.
enum class Clear : std::uint8_t { None, Left, Right, Both };

// A CSS length, resolved as far as it can be without a layout context.
//
// Percentages cannot be resolved here — they need a containing block, which
// does not exist until layout runs — so they are carried rather than
// collapsed. A style system that resolved them early would have to guess at the
// containing block, and the guess is wrong for every element inside a float.
struct Length {
  enum class Unit : std::uint8_t { Pixels, Percent, Em, Rem, Auto };

  float value = 0.0f;
  Unit unit = Unit::Pixels;

  static Length Auto() { return Length{0.0f, Unit::Auto}; }
  static Length Pixels(float value) { return Length{value, Unit::Pixels}; }

  bool IsAuto() const { return unit == Unit::Auto; }
  bool IsPercent() const { return unit == Unit::Percent; }

  // Absolute pixels, given the font size this length is relative to. Percentage
  // and auto have no answer without a containing block, so they return the
  // fallback the caller supplies rather than silently becoming zero.
  float Resolve(float font_size, float fallback = 0.0f) const;

  friend bool operator==(const Length&, const Length&) = default;
};

struct Edges {
  Length top;
  Length right;
  Length bottom;
  Length left;

  friend bool operator==(const Edges&, const Edges&) = default;
};

// The style of one element, after the cascade.
//
// Every property is resolved to a value — there is no "unset" state to check at
// use time. That is what makes layout able to read a style without asking
// whether each field was ever set, and it is why inheritance happens here
// rather than being a lookup that walks the tree on every read.
struct ComputedStyle {
  Display display = Display::Inline;
  gfx::Color color = gfx::Color::Rgb(0, 0, 0);
  gfx::Color background_color = gfx::Color::Transparent();

  // Inherited. Absolute pixels: font-size is the one length that must be
  // resolved during the cascade, because `em` on every other property is
  // relative to it.
  float font_size = 16.0f;
  float font_weight = 400.0f;
  FontStyle font_style = FontStyle::Normal;
  // The families the stylesheet named, best first. A list rather than a name
  // because that is what CSS says: `font-family: Verdana, Geneva, sans-serif`
  // asks for three fonts and settles for the first that exists. Which of them
  // exists is a property of the machine, so the choice cannot be made here --
  // the whole list travels to the font provider, which is the only thing that
  // knows. Bounded at gfx::kMaxFontFamilies where it is parsed.
  std::vector<std::string> font_family{"sans-serif"};
  // Zero means "normal", which is a multiple of the font size rather than a
  // length, and is resolved by layout.
  float line_height = 0.0f;

  TextAlign text_align = TextAlign::Left;
  WhiteSpace white_space = WhiteSpace::Normal;
  Float css_float = Float::None;
  Clear clear = Clear::None;

  Edges margin;
  Edges padding;
  Edges border_width;
  gfx::Color border_color = gfx::Color::Rgb(0, 0, 0);
  bool has_border = false;

  Length width = Length::Auto();
  Length height = Length::Auto();

  bool IsFloating() const { return css_float != Float::None; }

  bool IsInlineLevel() const {
    // A float is block-level whatever it was declared as: `float: left` on a
    // span makes it a block, per CSS 2.1 s9.7. Answering that here rather than
    // at each call site is what keeps the rule from being applied in three
    // places and forgotten in a fourth.
    return !IsFloating() && (display == Display::Inline || display == Display::InlineBlock);
  }
  bool GeneratesBox() const { return display != Display::None; }

  friend bool operator==(const ComputedStyle&, const ComputedStyle&) = default;
};

// Parses a colour: named, `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`.
// Nullopt for anything unrecognized, which is how an invalid declaration is
// dropped rather than turning an element transparent.
std::optional<gfx::Color> ParseColor(std::string_view text);

// Parses a length. Nullopt when the text is not one.
std::optional<Length> ParseLength(std::string_view text);

}  // namespace microbrowser::css

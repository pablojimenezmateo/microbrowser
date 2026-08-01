#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "gfx/Color.h"

namespace microbrowser::css {

enum class Display : std::uint8_t { Inline, Block, InlineBlock, ListItem, None };
enum class FontStyle : std::uint8_t { Normal, Italic };
enum class TextAlign : std::uint8_t { Left, Right, Center, Justify };
enum class WhiteSpace : std::uint8_t { Normal, Pre, NoWrap, PreWrap };

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
  std::string font_family = "sans-serif";
  // Zero means "normal", which is a multiple of the font size rather than a
  // length, and is resolved by layout.
  float line_height = 0.0f;

  TextAlign text_align = TextAlign::Left;
  WhiteSpace white_space = WhiteSpace::Normal;

  Edges margin;
  Edges padding;
  Edges border_width;
  gfx::Color border_color = gfx::Color::Rgb(0, 0, 0);
  bool has_border = false;

  Length width = Length::Auto();
  Length height = Length::Auto();

  bool IsInlineLevel() const {
    return display == Display::Inline || display == Display::InlineBlock;
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

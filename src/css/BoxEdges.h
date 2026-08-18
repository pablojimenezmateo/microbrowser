#pragma once

// The four-sided box properties: one value per edge, in CSS's top/right/bottom/left order.
//
// Their own header because ComputedStyle.h reached the module's file cap when the border became
// four sides rather than one. These four types are the part of it that nothing else in that file
// depends on, and a translation unit over its cap means a missing module rather than a bigger file.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "css/Length.h"
#include "gfx/Color.h"

namespace microbrowser::css {

// The three structs below each name their four sides `top`/`right`/`bottom`/`left` and each
// carries an `operator[]` so a shorthand can reach one by number. The indexer is a chain of
// comparisons rather than `(&top)[i]`: taking the address of one member and walking it to the
// next is undefined behaviour however the four happen to be laid out, and gcc diagnoses it as a
// write into a region of size zero.
struct Edges {
  Length top;
  Length right;
  Length bottom;
  Length left;


  // Side by number, in CSS order. See the note above this struct.
  Length& operator[](std::size_t side) { return side == 0 ? top : side == 1 ? right : side == 2 ? bottom : left; }
  const Length& operator[](std::size_t side) const {
    return side == 0 ? top : side == 1 ? right : side == 2 ? bottom : left;
  }

  friend bool operator==(const Edges&, const Edges&) = default;
};

// `border-style`, per side. `None` and `Hidden` are different values with the same paint -- they
// differ only in border conflict resolution on a collapsed table -- and every other value here is
// painted, so a side with a width and no style is a side that draws nothing. That is the rule the
// initial value encodes: `border: 1in` sets a width, leaves the style at `none`, and paints
// nothing until something says `solid`.
enum class BorderStyle : std::uint8_t {
  None,
  Hidden,
  Dotted,
  Dashed,
  Solid,
  Double,
  Groove,
  Ridge,
  Inset,
  Outset,
};

// The style and colour of the four sides, in the `top right bottom left` order `Edges` uses -- so
// ``sides[i]` indexes both the same way, which is what the shorthand parsers rely on.
//
// The colour is optional because the *initial* value of `border-color` is `currentColor`, and a
// computed style cannot fold that at parse time: `<div style="color:red;border:1px solid">` has a
// red border and the declaration never mentions red. Empty means "ask `color` at paint time",
// which is one branch in the painter and no second copy of the cascade.
struct BorderSides {
  BorderStyle top = BorderStyle::None;
  BorderStyle right = BorderStyle::None;
  BorderStyle bottom = BorderStyle::None;
  BorderStyle left = BorderStyle::None;


  // Side by number, in CSS order. See the note above this struct.
  BorderStyle& operator[](std::size_t side) { return side == 0 ? top : side == 1 ? right : side == 2 ? bottom : left; }
  const BorderStyle& operator[](std::size_t side) const {
    return side == 0 ? top : side == 1 ? right : side == 2 ? bottom : left;
  }

  friend bool operator==(const BorderSides&, const BorderSides&) = default;
};

struct BorderColors {
  std::optional<gfx::Color> top;
  std::optional<gfx::Color> right;
  std::optional<gfx::Color> bottom;
  std::optional<gfx::Color> left;


  // Side by number, in CSS order. See the note above this struct.
  std::optional<gfx::Color>& operator[](std::size_t side) { return side == 0 ? top : side == 1 ? right : side == 2 ? bottom : left; }
  const std::optional<gfx::Color>& operator[](std::size_t side) const {
    return side == 0 ? top : side == 1 ? right : side == 2 ? bottom : left;
  }

  friend bool operator==(const BorderColors&, const BorderColors&) = default;
};

}  // namespace microbrowser::css

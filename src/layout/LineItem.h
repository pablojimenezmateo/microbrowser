#pragma once

#include "css/ComputedStyle.h"

#include <cstdint>
#include <vector>

namespace microbrowser::layout {

class Box;
class TextMeasurer;

// One item on the current line: a slice of a text box, or a whole replaced
// box. Both are rectangles hung from a baseline; that is the only thing line
// layout needs to know about either.
//
// At file scope rather than inside the function because the bidi reorder below
// takes a whole line, and a type local to one function cannot appear in another
// one's signature.
struct LineItem {
  Box* box = nullptr;
  bool is_text = false;
  // An atomic inline: laid out inside like a block, so its final position is
  // not something to *write* into its geometry but something to lay it out
  // *at*. Writing the rectangle would move the box and leave every descendant
  // where the measuring pass put it -- geometry here is absolute, so a box
  // that moves takes its whole subtree with it or it takes none of it.
  bool is_atomic = false;
  std::uint32_t begin = 0;
  std::uint32_t length = 0;
  float x = 0.0f;
  float width = 0.0f;
  float above = 0.0f;  // from the baseline up
  float below = 0.0f;  // from the baseline down
  // `vertical-align`, as the one number line placement can use: how far *up* from the line's
  // baseline this item's own baseline sits. Every value of the property that can be resolved before
  // the line box exists is folded into this at push time (CSS 2.1 §10.8.1); `top` and `bottom`
  // cannot be, so they arrive as `edge` and become a shift in `finish_line` once the line's height
  // is known. Zero and `Baseline` is what every item on an ordinary line carries, and the placement
  // code below is written so that pair reproduces exactly what it did before the property existed.
  float shift = 0.0f;
  css::VerticalAlign edge = css::VerticalAlign::Baseline;
  // Which inline box asked for that edge. `top` and `bottom` align the box that carries them, not
  // each thing inside it -- so every item descended from one `<span style="vertical-align: top">`
  // is *one* thing to place, and `line`'s items are the pieces it was flattened into. Aligning them
  // individually moves a short word up to the top of a line its own tall sibling defines, which is
  // css/CSS2/linebox/anonymous-inline-inherit-001.html exactly.
  const css::ComputedStyle* group = nullptr;
  // Set by the bidi reorder, and false for every item on a line that never needed it.
  bool right_to_left = false;
};

// Rewrites `line` into visual order, and re-assigns every `x` from `left`. UAX #9 L1 and L2.
// Its own translation unit because InlineLayout.cpp reached the module's file cap when
// `vertical-align` and the white-space model landed in the same week, and the bidi reorder is
// the half of that file that shares nothing with the rest of it but this type.
void ReorderLineForBidi(std::vector<LineItem>& line, css::Direction direction,
                        const TextMeasurer& measurer, float left);

}  // namespace microbrowser::layout

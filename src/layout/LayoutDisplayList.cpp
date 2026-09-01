#include "layout/LayoutEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "html/FormControl.h"
#include "layout/Stacking.h"
#include "text/Bidi.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::layout {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Multiply a colour's alpha by a paint-time opacity. Group compositing (offscreen
// + blend) is the spec answer for overlapping translucent descendants; this is
// the SVG-style approximation and enough for youtube's opacity:0 fills and the
// consent backdrop's translucent black.
gfx::Color WithPaintOpacity(gfx::Color color, float opacity) {
  if (opacity >= 1.0f) {
    return color;
  }
  if (opacity <= 0.0f || color.IsFullyTransparent()) {
    return gfx::Color::Transparent();
  }
  const float alpha = static_cast<float>(color.Alpha()) * opacity + 0.5f;
  return color.WithAlpha(
      static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 255.0f)));
}

// The user agent's own media controls.
//
// ADR 0028 §1 puts these in ADR 0018's category: boxes the *user agent* creates inside a page,
// not widgets `src/ui` owns. That has two consequences worth stating, and both are why this is
// twenty lines in the display-list builder rather than a control class somewhere:
//
//   * They are painted in the page's coordinate space, so a transformed or clipped `<video>`
//     transforms and clips its controls -- which a widget layer above the page could not do.
//   * A page that styles the element styles them: they take its colour, and a page that sets
//     `display: none` gets no controls because it gets no box.
//
// What they are *not* is interactive yet: hit testing does not know about them, so this is a
// drawn control bar rather than a working one. That is the honest state -- a bar that looked
// clickable and did nothing would be worse -- and it is what a page sees today when it writes
// `<video controls>`.
void PaintMediaControls(const Box& box, gfx::DisplayList& out, gfx::FloatPoint offset) {
  const dom::Element* element = box.Origin();
  if (element == nullptr || (element->TagName() != "video" && element->TagName() != "audio") ||
      !element->HasAttribute("controls")) {
    return;
  }
  const gfx::FloatRect content = box.Geometry().content;
  const gfx::FloatRect area{content.x + offset.x, content.y + offset.y, content.width,
                            content.height};
  // The bar's height is the specification's nothing -- there is no specified size -- so it is
  // the number every browser lands near, and it is clamped to the element: a 20px-tall video
  // gets a 20px bar rather than a bar taller than the video.
  const float bar_height = std::min(area.height, 32.0f);
  if (area.width <= 0.0f || bar_height <= 0.0f) {
    return;
  }
  const gfx::FloatRect bar{area.x, area.Bottom() - bar_height, area.width, bar_height};
  gfx::Path panel;
  panel.AddRect(bar);
  out.FillPath(panel, gfx::Color::Rgba(0x20, 0x20, 0x20, 0xC0));

  // A play triangle, and it points right because nothing is playing: this builder has no access
  // to the state machine (layout may not see `media`), so what it draws is the *element*, and
  // a pause bar would be a claim about state it cannot check.
  const float glyph = std::min(bar_height * 0.5f, 12.0f);
  const float centre_y = bar.y + bar_height * 0.5f;
  const float left = bar.x + 10.0f;
  gfx::Path play;
  play.MoveTo(gfx::FloatPoint{left, centre_y - glyph * 0.5f});
  play.LineTo(gfx::FloatPoint{left + glyph * 0.9f, centre_y});
  play.LineTo(gfx::FloatPoint{left, centre_y + glyph * 0.5f});
  play.Close();
  out.FillPath(play, gfx::Color::Rgb(0xF0, 0xF0, 0xF0));

  // The scrubber track, empty. Drawing a filled portion would be a claim about a position
  // nothing here can read.
  const float track_left = left + glyph * 0.9f + 10.0f;
  const float track_right = bar.Right() - 10.0f;
  if (track_right > track_left) {
    gfx::Path track;
    track.AddRect(gfx::FloatRect{track_left, centre_y - 1.5f, track_right - track_left, 3.0f});
    out.FillPath(track, gfx::Color::Rgba(0xF0, 0xF0, 0xF0, 0x60));
  }
}

void PaintCheckedInputIndicator(const Box& box, gfx::DisplayList& out, gfx::FloatPoint offset) {
  const dom::Element* element = box.Origin();
  if (element == nullptr || element->TagName() != "input" || !element->HasAttribute("checked")) {
    return;
  }

  const css::ComputedStyle& style = box.Style();
  const gfx::FloatRect content = box.Geometry().content;
  const gfx::FloatRect control{content.x + offset.x, content.y + offset.y, content.width,
                               content.height};
  const float side = std::min(control.width, control.height);
  if (side <= 0.0f) {
    return;
  }

  if (html::IsCheckboxInput(*element)) {
    gfx::Path mark;
    mark.MoveTo(gfx::FloatPoint{control.x + side * 0.25f, control.y + side * 0.55f});
    mark.LineTo(gfx::FloatPoint{control.x + side * 0.45f, control.y + side * 0.75f});
    mark.LineTo(gfx::FloatPoint{control.x + side * 0.78f, control.y + side * 0.30f});
    gfx::StrokeStyle stroke;
    stroke.width = std::max(1.0f, side * 0.12f);
    out.StrokePath(mark, stroke, style.color);
  } else if (html::IsRadioInput(*element)) {
    gfx::Path dot;
    const float inset = side * 0.32f;
    dot.AddEllipse(gfx::FloatRect{control.x + inset, control.y + inset,
                                  std::max(0.0f, side - inset * 2.0f),
                                  std::max(0.0f, side - inset * 2.0f)});
    out.FillPath(dot, style.color);
  }
}

// The most tiles one background may emit.
//
// A one-pixel image repeated across a full-page element is millions of draw
// commands, every one of which is serialized, diffed and replayed. The bound
// is high enough that no design hits it and low enough that a hostile page
// cannot turn a 40-byte GIF into a gigabyte of display list.
constexpr int kMaxBackgroundTiles = 4096;

// Where one tile of a background image goes, in the element's own coordinates.
//
// `background-size` decides the tile; `auto` on an axis means the image's own
// size there, which is what keeps an icon's proportions when only one axis is
// given. A percentage is a percentage of the box, which is what CSS says and is
// the difference between a background that scales with the element and one that
// does not.
gfx::FloatRect BackgroundTile(const css::ComputedStyle& style, const gfx::Image& image,
                              const gfx::FloatRect& box) {
  const auto axis = [&style](const css::Length& length, float box_extent, float intrinsic) {
    if (length.IsAuto()) {
      return intrinsic;
    }
    return length.IsPercent() ? length.Used(box_extent, style.font_size)
                              : length.Resolve(style.font_size, intrinsic);
  };
  const float intrinsic_width = static_cast<float>(image.Width());
  const float intrinsic_height = static_cast<float>(image.Height());
  float width = axis(style.background.size_x, box.width, intrinsic_width);
  float height = axis(style.background.size_y, box.height, intrinsic_height);
  // `auto` on one axis with a length on the other keeps the image's proportions
  // rather than taking its own size on that axis. `background-size: 10px` on a
  // 32-pixel-square icon means a 10-pixel square, and a browser that used 10 by
  // 32 would stretch every icon on the page.
  if (style.background.size_x.IsAuto() != style.background.size_y.IsAuto() &&
      intrinsic_width > 0.0f && intrinsic_height > 0.0f) {
    if (style.background.size_x.IsAuto()) {
      width = height * intrinsic_width / intrinsic_height;
    } else {
      height = width * intrinsic_height / intrinsic_width;
    }
  }
  if (!(width > 0.0f) || !(height > 0.0f)) {
    return gfx::FloatRect{};
  }

  // A percentage position is a fraction of the space the image does *not*
  // fill, which is why `50%` centres rather than offsetting by half the box.
  const auto place = [&style](const css::Length& length, float box_extent, float tile_extent) {
    return length.Used(box_extent - tile_extent, style.font_size);
  };
  return gfx::FloatRect{box.x + place(style.background.position_x, box.width, width),
                        box.y + place(style.background.position_y, box.height, height), width,
                        height};
}

// One side of a border, as the four-cornered shape it actually is.
//
// A border is not a stroked rectangle -- it was one here, at `border-width.top`, in one colour --
// and the difference is visible the moment two sides differ: the corner between a 10px red top and
// a 2px blue left is a mitre, and each half of it belongs to a different side. `depth_from` and
// `depth_to` are fractions of the side's thickness, which is what lets `double` be two calls with
// the same mitre rather than a second geometry.
gfx::Path BorderSideQuad(const gfx::FloatRect& box, const float (&width)[4], std::size_t side,
                         float depth_from, float depth_to) {
  // Outer and inner insets on each edge, at the two depths this band spans.
  const auto inset = [&width](int edge, float depth) { return width[edge] * depth; };
  const float left_from = inset(3, depth_from);
  const float left_to = inset(3, depth_to);
  const float right_from = inset(1, depth_from);
  const float right_to = inset(1, depth_to);
  const float top_from = inset(0, depth_from);
  const float top_to = inset(0, depth_to);
  const float bottom_from = inset(2, depth_from);
  const float bottom_to = inset(2, depth_to);
  const float x0 = box.x;
  const float x1 = box.Right();
  const float y0 = box.y;
  const float y1 = box.Bottom();
  const float near = width[side] * depth_from;
  const float far = width[side] * depth_to;
  gfx::Path path;
  switch (side) {
    case 0:  // top
      path.MoveTo({x0 + left_from, y0 + near});
      path.LineTo({x1 - right_from, y0 + near});
      path.LineTo({x1 - right_to, y0 + far});
      path.LineTo({x0 + left_to, y0 + far});
      break;
    case 1:  // right
      path.MoveTo({x1 - near, y0 + top_from});
      path.LineTo({x1 - near, y1 - bottom_from});
      path.LineTo({x1 - far, y1 - bottom_to});
      path.LineTo({x1 - far, y0 + top_to});
      break;
    case 2:  // bottom
      path.MoveTo({x0 + left_from, y1 - near});
      path.LineTo({x0 + left_to, y1 - far});
      path.LineTo({x1 - right_to, y1 - far});
      path.LineTo({x1 - right_from, y1 - near});
      break;
    default:  // left
      path.MoveTo({x0 + near, y0 + top_from});
      path.LineTo({x0 + far, y0 + top_to});
      path.LineTo({x0 + far, y1 - bottom_to});
      path.LineTo({x0 + near, y1 - bottom_from});
      break;
  }
  path.Close();
  return path;
}

// The four sides, each with its own width, style and colour (CSS 2.1 §8.5).
//
// `dotted` and `dashed` are painted solid: the rasterizer has no dash, and a side that vanished
// would be further from the specification than one that is continuous. `groove`, `ridge`, `inset`
// and `outset` are painted solid in the declared colour rather than in the two shades CSS derives,
// for the same reason a reftest reference is drawn by this same code -- a self-consistent
// approximation costs nothing where an absent border costs the whole box.
void PaintBorders(const css::ComputedStyle& style, const gfx::FloatRect& border_box,
                  float paint_opacity, gfx::DisplayList& out) {
  const css::Edges used = style.UsedBorderWidths();
  float width[4] = {
      std::max(0.0f, used.top.Resolve(style.font_size)),
      std::max(0.0f, used.right.Resolve(style.font_size)),
      std::max(0.0f, used.bottom.Resolve(style.font_size)),
      std::max(0.0f, used.left.Resolve(style.font_size)),
  };
  // Opposite sides that together exceed the box would cross over and paint each other's half.
  // CSS has no rule for it because the used widths cannot exceed the border box; ours can, because
  // a declared width is not clamped against a box whose size came from somewhere else.
  const float horizontal = width[1] + width[3];
  if (horizontal > border_box.width && horizontal > 0.0f) {
    width[1] *= border_box.width / horizontal;
    width[3] *= border_box.width / horizontal;
  }
  const float vertical = width[0] + width[2];
  if (vertical > border_box.height && vertical > 0.0f) {
    width[0] *= border_box.height / vertical;
    width[2] *= border_box.height / vertical;
  }
  // A border whose four sides agree is *one* shape, and drawing it as four is not just wasteful:
  // two antialiased polygons meeting along a mitre leave a visible seam down the diagonal at every
  // corner, which is what the paint-pipeline golden caught. Nonzero winding turns an outer contour
  // and a reversed inner one into a ring.
  const bool uniform = width[0] > 0.0f && width[0] == width[1] && width[1] == width[2] &&
                       width[2] == width[3] && style.border_style.top == style.border_style.right &&
                       style.border_style.right == style.border_style.bottom &&
                       style.border_style.bottom == style.border_style.left &&
                       style.BorderColorFor(0) == style.BorderColorFor(1) &&
                       style.BorderColorFor(1) == style.BorderColorFor(2) &&
                       style.BorderColorFor(2) == style.BorderColorFor(3) &&
                       style.border_style.top != css::BorderStyle::Double;
  if (uniform) {
    const gfx::Color color = WithPaintOpacity(style.BorderColorFor(0), paint_opacity);
    if (color.IsFullyTransparent()) {
      return;
    }
    const float inset = width[0];
    gfx::Path ring;
    ring.AddRect(border_box);
    ring.MoveTo({border_box.x + inset, border_box.y + inset});
    ring.LineTo({border_box.x + inset, border_box.Bottom() - inset});
    ring.LineTo({border_box.Right() - inset, border_box.Bottom() - inset});
    ring.LineTo({border_box.Right() - inset, border_box.y + inset});
    ring.Close();
    out.FillPath(ring, color);
    return;
  }
  for (std::size_t side = 0; side < 4; ++side) {
    if (width[side] <= 0.0f) {
      continue;
    }
    const gfx::Color color = WithPaintOpacity(style.BorderColorFor(side), paint_opacity);
    if (color.IsFullyTransparent()) {
      continue;
    }
    if (style.border_style[side] == css::BorderStyle::Double) {
      // Three equal bands, the middle one left empty. A one- or two-pixel border has no room for
      // that, so it stays solid rather than becoming a gap.
      if (width[side] >= 3.0f) {
        out.FillPath(BorderSideQuad(border_box, width, side, 0.0f, 1.0f / 3.0f), color);
        out.FillPath(BorderSideQuad(border_box, width, side, 2.0f / 3.0f, 1.0f), color);
        continue;
      }
    }
    out.FillPath(BorderSideQuad(border_box, width, side, 0.0f, 1.0f), color);
  }
}

void PaintBackgroundImage(const Box& box, const gfx::FloatRect& border_box, gfx::DisplayList& out) {
  const std::shared_ptr<const gfx::Image>& image = box.BackgroundImage();
  if (image == nullptr || !image->IsValid() || border_box.IsEmpty()) {
    return;
  }
  const css::ComputedStyle& style = box.Style();
  const gfx::FloatRect first = BackgroundTile(style, *image, border_box);
  if (first.width <= 0.0f || first.height <= 0.0f) {
    return;
  }

  const bool repeat_x = style.background.repeat == css::BackgroundRepeat::Repeat ||
                        style.background.repeat == css::BackgroundRepeat::RepeatX;
  const bool repeat_y = style.background.repeat == css::BackgroundRepeat::Repeat ||
                        style.background.repeat == css::BackgroundRepeat::RepeatY;

  // Tiling starts at the first tile and walks *backwards* as well as forwards:
  // a positioned background repeats in both directions from where it sits, and
  // one that only walked forwards would leave the space above and to the left
  // of the origin bare.
  const auto span = [](bool repeat, float origin, float tile, float box_low, float box_high,
                       float& start) {
    if (!repeat) {
      start = origin;
      return 1;
    }
    const float steps_back = std::ceil((origin - box_low) / tile);
    start = origin - steps_back * tile;
    // How many tiles from `start` it takes to reach the far edge -- not one
    // more. An edge that lands exactly on a tile boundary needs no extra tile,
    // and adding one anyway draws a whole invisible column outside the clip on
    // every background on the page.
    return std::max(1, static_cast<int>(std::ceil((box_high - start) / tile)));
  };
  float start_x = first.x;
  float start_y = first.y;
  const int columns = span(repeat_x, first.x, first.width, border_box.x, border_box.Right(),
                           start_x);
  const int rows = span(repeat_y, first.y, first.height, border_box.y, border_box.Bottom(),
                        start_y);
  if (columns <= 0 || rows <= 0 || columns > kMaxBackgroundTiles ||
      rows > kMaxBackgroundTiles || columns * rows > kMaxBackgroundTiles) {
    // Too many to be a design. Draw the single tile so the element is not
    // simply blank, and stop -- which is legible, unlike either extreme.
    out.PushClip(gfx::EnclosingIntRect(border_box));
    out.DrawImage(image, gfx::EnclosingIntRect(first));
    out.PopClip();
    return;
  }

  // Clipped to the element, because the last tile in each direction runs past
  // its edge by design -- that is what makes a repeat reach the corner.
  out.PushClip(gfx::EnclosingIntRect(border_box));
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      out.DrawImage(image, gfx::EnclosingIntRect(gfx::FloatRect{
                               start_x + static_cast<float>(column) * first.width,
                               start_y + static_cast<float>(row) * first.height, first.width,
                               first.height}));
    }
  }
  out.PopClip();
}

// Backgrounds on inline boxes paint behind each line fragment of their content,
// not behind an empty border box. An Inline box never receives geometry of its
// own -- fragments live on Text descendants -- which is why a spoiler span's
// grey bar was invisible text on white rather than a bar.
void PaintInlineBackground(const Box& box, gfx::FloatPoint offset, float paint_opacity,
                           gfx::DisplayList& out) {
  const css::ComputedStyle& style = box.Style();
  const gfx::Color fill = WithPaintOpacity(style.background_color, paint_opacity);
  if (style.visibility != css::Visibility::Visible || fill.IsFullyTransparent()) {
    return;
  }
  const float font_size = style.font_size;
  const css::Edges borders = style.UsedBorderWidths();
  // The background of an inline box fills its *border* box, so the inflation is padding plus
  // border on each side -- the same two amounts inline layout reserved on the line. They have to
  // be the same two, or the colour lands beside the space it was given rather than in it.
  const float lead = style.padding.left.Resolve(font_size) + borders.left.Resolve(font_size);
  const float trail = style.padding.right.Resolve(font_size) + borders.right.Resolve(font_size);
  const float above = style.padding.top.Resolve(font_size) + borders.top.Resolve(font_size);
  const float below = style.padding.bottom.Resolve(font_size) + borders.bottom.Resolve(font_size);

  // An inline box that wraps is several fragments, and its horizontal edges belong to the ends of
  // the *box* rather than to the ends of each line: the start edge on the first fragment, the end
  // edge on the last, nothing in between (CSS 2.1 §8.1). Painting both on every fragment is what
  // put `padding-left: 50px` fifty pixels to the *left* of the text on every line, including the
  // ones that were not the beginning of anything. The vertical pair is on every fragment, because
  // each fragment has a top and a bottom of its own.
  //
  // Two passes because "the last one" is not known until the walk is over, and the walk is over a
  // subtree rather than a list.
  std::vector<gfx::FloatRect> rects;
  box.ForEachDescendant([&](const Box& desc) {
    if (desc.GetKind() == Box::Kind::Text) {
      for (const TextFragment& fragment : desc.Fragments()) {
        rects.push_back(fragment.rect);
      }
    } else if (desc.IsAtomicInline()) {
      rects.push_back(desc.Geometry().BorderBox());
    }
  });

  for (std::size_t i = 0; i < rects.size(); ++i) {
    const float start = i == 0 ? lead : 0.0f;
    const float end = i + 1 == rects.size() ? trail : 0.0f;
    gfx::FloatRect rect = rects[i];
    rect.x -= start;
    rect.y -= above;
    rect.width += start + end;
    rect.height += above + below;
    rect.x += offset.x;
    rect.y += offset.y;
    if (rect.IsEmpty()) {
      continue;
    }
    gfx::Path path;
    path.AddRect(rect);
    out.FillPath(path, fill);
  }
}

// Where a box's descendants are measured against when they are pinned rather
// than laid out: the nearest scrollport, and the containing block a sticky box
// may not escape. Both in painted coordinates, which is the coordinate system
// everything in this file is already in by the time it is used.
struct PaintFrame {
  gfx::FloatRect scrollport;
  gfx::FloatRect containing_block;
};

// How far a `position: sticky` box is displaced, per axis.
//
// It is `relative` until the edge it names would leave the scrollport, then it
// stays at that edge, and it never leaves its containing block -- which is why
// a sticky section header is pushed off the top by the next section rather than
// piling up. Two clamps and no state; ADR 0018 §2 is exactly this arithmetic,
// and the reason it could not be written before is that `scrollport` had no
// value to take.
float StickyShift(float low_inset, bool has_low, float high_inset, bool has_high, float box_low,
                  float box_high, float port_low, float port_high, float block_low,
                  float block_high) {
  float shift = 0.0f;
  if (has_low) {
    shift = std::max(shift, port_low + low_inset - box_low);
  }
  if (has_high && port_high > port_low) {
    shift = std::min(shift, port_high - high_inset - box_high);
  }
  // Inside the containing block, both ways. A box that started inside it and
  // was pushed out is the one visible symptom of getting this wrong.
  //
  // Skipped when the containing block is degenerate -- an inline parent, or a
  // caller that did not say how big its viewport is. Clamping against an empty
  // rectangle would drag every sticky box on the page to its origin, which is a
  // far worse answer than not sticking.
  if (block_high > block_low) {
    shift = std::min(shift, block_high - box_high);
    shift = std::max(shift, block_low - box_low);
  }
  return shift;
}

gfx::FloatPoint StickyOffset(const Box& box, const gfx::FloatRect& border_box,
                             const PaintFrame& frame) {
  const css::ComputedStyle& style = box.Style();
  const css::Edges& inset = style.inset;
  const float font_size = style.font_size;
  const auto used = [font_size](const css::Length& length, float extent) {
    return length.Used(extent, font_size);
  };
  const gfx::FloatRect& port = frame.scrollport;
  const gfx::FloatRect& block = frame.containing_block;
  return gfx::FloatPoint{
      StickyShift(used(inset.left, port.width), !inset.left.IsAuto(),
                  used(inset.right, port.width), !inset.right.IsAuto(), border_box.x,
                  border_box.Right(), port.x, port.Right(), block.x, block.Right()),
      StickyShift(used(inset.top, port.height), !inset.top.IsAuto(),
                  used(inset.bottom, port.height), !inset.bottom.IsAuto(), border_box.y,
                  border_box.Bottom(), port.y, port.Bottom(), block.y, block.Bottom()),
  };
}

}  // namespace

void BuildDisplayList(const Box& root, gfx::DisplayList& out, gfx::FloatPoint document_offset,
                      gfx::FloatSize viewport) {
  // The document's own scrollport, in painted coordinates. Its origin is (0,0)
  // by construction: `offset` is the negated scroll position, so the top-left
  // of the window is exactly where the scrolled document's viewport edge lands.
  const PaintFrame root_frame{gfx::FloatRect{0.0f, 0.0f, viewport.width, viewport.height},
                              gfx::FloatRect{0.0f, 0.0f, viewport.width, viewport.height}};

  // Backgrounds and borders paint before content, which is what makes a child
  // draw on top of its parent's background rather than under it.
  // `collects` is "this box is a stacking context": it orders the units beneath it
  // rather than painting them where the tree walk reached them. The root always is,
  // which is what makes a page with one positioned box work without one.
  const auto paint = [&out](const Box& box, gfx::FloatPoint offset, const PaintFrame& frame,
                            bool collects, float opacity_mul, auto& self) -> void {
    const css::ComputedStyle& style = box.Style();
    // Only element boxes establish `opacity`. Anonymous wrappers copy the
    // parent's style (same trap as `transform`), so multiplying again would
    // square the alpha.
    float paint_opacity = opacity_mul;
    if (box.Origin() != nullptr) {
      paint_opacity *= style.opacity;
    }
    // `opacity: 0` paints nothing in the subtree. Unlike `visibility: hidden`,
    // visible descendants are also skipped — CSS treats opacity as a group.
    if (paint_opacity <= 0.0f) {
      return;
    }
    // A fixed box is positioned against the viewport, so it drops every scroll
    // translation above it -- which is what makes it stay put while the page
    // moves under it, and what lets the presenter blit a scroll and then repaint
    // only the strip these boxes cover.
    if (style.position == css::Position::Fixed) {
      offset = gfx::FloatPoint{};
    }
    if (style.position == css::Position::Sticky && box.GetKind() != Box::Kind::Text) {
      const gfx::FloatRect unpinned = box.Geometry().BorderBox();
      const gfx::FloatPoint shift = StickyOffset(
          box,
          gfx::FloatRect{unpinned.x + offset.x, unpinned.y + offset.y, unpinned.width,
                         unpinned.height},
          frame);
      offset = gfx::FloatPoint{offset.x + shift.x, offset.y + shift.y};
    }
    // A text box has no background and no border by construction, but the
    // painter says so too: this is the kind of invariant that is cheap to
    // assert here and expensive to rediscover from a screenshot.
    if (box.GetKind() == Box::Kind::Text) {
      if (style.visibility != css::Visibility::Visible) {
        return;
      }
      const gfx::FontRequest font = FontRequestFor(style);
      const gfx::Color ink = WithPaintOpacity(style.color, paint_opacity);
      for (const TextFragment& fragment : box.Fragments()) {
        const std::string_view piece(box.Text().data() + fragment.begin, fragment.length);
        // Rule L4, and it belongs here rather than in the DOM: the *character* is still U+0028, and
        // `textContent` and a copy to the clipboard must both still say so. Only what is painted
        // changes. `MirrorForRightToLeft` returns its input when nothing in the run mirrors, which is
        // almost every run even in Arabic.
        const std::string mirrored =
            fragment.right_to_left ? text::MirrorForRightToLeft(piece) : std::string();
        // The baseline, not the top of the line box. They differ by an ascent,
        // and using the wrong one puts every line of text a line too low.
        out.DrawText(fragment.right_to_left ? std::string_view(mirrored) : piece,
                     fragment.rect.width, font,
                     gfx::FloatPoint{fragment.rect.x + offset.x, fragment.baseline + offset.y},
                     ink, fragment.right_to_left);
      }
      return;
    }
    if (box.GetKind() == Box::Kind::Inline) {
      PaintInlineBackground(box, offset, paint_opacity, out);
    }
    const gfx::FloatRect unshifted = box.Geometry().BorderBox();
    const gfx::FloatRect border_box{unshifted.x + offset.x, unshifted.y + offset.y,
                                    unshifted.width, unshifted.height};

    // `transform`, pushed around this box and everything under it. ADR 0014 §4.
    //
    // The matrix is resolved in *painted* coordinates rather than the box's own,
    // which is why the origin is the border box's painted corner plus the resolved
    // `transform-origin`: the display list has one coordinate space and a matrix
    // that assumed a local one would rotate the box about the top-left of the page.
    //
    // Percentages resolve against the border box, which is the size the property is
    // defined against and the reason the list is not collapsed to a matrix in the
    // cascade.
    // Only a box that came from an element. An anonymous box is generated by layout
    // and carries a copy of its parent's style, so a transformed `<div>` with text in
    // it would otherwise push the same matrix once for the div and again for each
    // anonymous wrapper inside it -- applying it two or three times over.
    const bool transformed =
        (!style.transform.IsNone() || !style.individual_transform.IsNone()) &&
        !border_box.IsEmpty() && box.Origin() != nullptr;
    if (transformed) {
      const gfx::FloatPoint origin{
          border_box.x + style.transform_origin_x.Used(border_box.width, style.font_size),
          border_box.y + style.transform_origin_y.Used(border_box.height, style.font_size)};
      // `translate`, `rotate`, `scale`, then `transform` -- the specification's
      // order (CSS Transforms 2 §"Current transformation matrix"), which is not
      // the order the page declared them in. All four about the same origin, so
      // the origin translation is applied once around the whole product rather
      // than once per part.
      const gfx::FloatSize size{border_box.width, border_box.height};
      gfx::AffineTransform matrix =
          style.individual_transform.ToMatrix(size, style.font_size)
              .Then(style.transform.ToMatrix(size, gfx::FloatPoint{0.0f, 0.0f},
                                             style.font_size));
      matrix = gfx::AffineTransform::Translation(-origin.x, -origin.y)
                   .Then(matrix)
                   .Then(gfx::AffineTransform::Translation(origin.x, origin.y));
      out.PushTransform(matrix);
    }
    // Every `return` below this line would leak the push, so the one exit that
    // exists -- the replaced-element branch -- pops explicitly. A scope guard was
    // the alternative and it hides a balance question that is worth seeing.
    const auto pop_transform = [transformed, &out]() {
      if (transformed) {
        out.PopTransform();
      }
    };

    // `visibility: hidden` still generates boxes and still paints *visible*
    // descendants, but skips its own ink. youtube's closed guide drawer relies
    // on that for both painting and hit-testing (ADR 0017 §5).
    //
    // Anonymous boxes (no element origin) copy a parent's style for inheritance
    // of font/color into text, but must not re-paint that parent's background:
    // wrappers under `ytd-app` were emitting full-viewport white fills *after*
    // the feed nudge text and erasing it.
    const bool paints_self =
        style.visibility == css::Visibility::Visible && box.Origin() != nullptr;
    const gfx::Color background = WithPaintOpacity(style.background_color, paint_opacity);
    const gfx::Color ink = WithPaintOpacity(style.color, paint_opacity);

    if (paints_self && !background.IsFullyTransparent() && !border_box.IsEmpty()) {
      gfx::Path background_path;
      background_path.AddRect(border_box);
      out.FillPath(background_path, background);
    }
    // Over the colour and under the border, which is the order CSS paints a
    // background in and the reason a page can put a translucent image over a
    // solid colour and get both.
    if (paints_self) {
      PaintBackgroundImage(box, border_box, out);
    }
    if (paints_self && !border_box.IsEmpty()) {
      PaintBorders(style, border_box, paint_opacity, out);
    }

    if (box.GetKind() == Box::Kind::Replaced) {
      if (paints_self) {
        const gfx::FloatRect content = box.Geometry().content;
        if (box.Image() != nullptr && box.Image()->IsValid()) {
          // The used size, not the intrinsic one: a declared width scales the
          // image, which is what an <img width=40> on a 400px file means.
          out.DrawImage(box.Image(),
                        gfx::EnclosingIntRect(gfx::FloatRect{content.x + offset.x,
                                                             content.y + offset.y, content.width,
                                                             content.height}));
        } else if (box.VideoSurface() != gfx::kNoSurface) {
          out.DrawSurface(box.VideoSurface(),
                          gfx::EnclosingIntRect(gfx::FloatRect{content.x + offset.x,
                                                               content.y + offset.y, content.width,
                                                               content.height}));
        }
        if (!box.Text().empty()) {
          const gfx::FontRequest font = FontRequestFor(style);
          const float baseline = content.y + content.height * 0.5f + style.font_size * 0.3f;
          out.DrawText(box.Text(), std::max(0.0f, content.width - 8.0f), font,
                       gfx::FloatPoint{content.x + offset.x + 4.0f, baseline + offset.y},
                       ink);
        }
        PaintCheckedInputIndicator(box, out, offset);
        PaintMediaControls(box, out, offset);
      }
      pop_transform();
      return;
    }

    // Anything but `overflow: visible` cuts its content off at the padding
    // box -- the same rectangle a background paints, which is what makes a
    // clipped child stop exactly where the box's own paint stops. The clip is
    // pushed after this box's own background and border so that neither is
    // clipped by it.
    //
    // Scrolling is the other half of overflow, and it is here now: the clip
    // stays where the box is, and the *children* are translated by the negated
    // offset. That is the whole of what a scroll costs in paint, and it is why
    // ADR 0018 calls a scroll a paint rather than a layout -- nothing above
    // this line changed.
    const bool clips = box.ClipsOverflow();
    PaintFrame child_frame = frame;
    gfx::FloatPoint child_offset = offset;
    const gfx::FloatRect padding_box{box.Geometry().PaddingBox().x + offset.x,
                                     box.Geometry().PaddingBox().y + offset.y,
                                     box.Geometry().PaddingBox().width,
                                     box.Geometry().PaddingBox().height};
    if (clips) {
      out.PushClip(gfx::IntRect{
          static_cast<int>(std::floor(padding_box.x)),
          static_cast<int>(std::floor(padding_box.y)),
          static_cast<int>(std::ceil(padding_box.width)),
          static_cast<int>(std::ceil(padding_box.height)),
      });
      child_offset.x -= box.ScrollOffset().x;
      child_offset.y -= box.ScrollOffset().y;
      child_frame.scrollport = padding_box;
    }
    // The containing block a sticky child may not escape is this box's content
    // box, in the coordinate system its children paint in.
    const gfx::FloatRect content_box = box.Geometry().content;
    child_frame.containing_block =
        gfx::FloatRect{content_box.x + child_offset.x, content_box.y + child_offset.y,
                       content_box.width, content_box.height};
    // CSS 2.1 Appendix E, in the order it specifies: this box's background and
    // border are already painted above; then negative layers, then in-flow
    // content, then layer zero, then the positive layers.
    //
    // Only a **stacking context** does this. A box that is merely positioned paints
    // its non-unit children in tree order and leaves its positioned descendants to
    // the nearest ancestor context -- which is what `z-index: auto` means, and what
    // stops every ancestor from collecting the same descendant and painting it once
    // per ancestor.
    if (!collects) {
      for (const std::unique_ptr<Box>& child : box.Children()) {
        if (!PaintsAsUnit(*child)) {
          self(*child, child_offset, child_frame, false, paint_opacity, self);
        }
      }
      if (clips) {
        out.PopClip();
      }
      pop_transform();
      return;
    }

    // A "unit" is a positioned, transformed or z-indexed box (see PaintsAsUnit).
    // Collecting them is a separate walk from painting them, and it has to be: a
    // negative layer paints *before* the in-flow content of this box, and the units
    // that belong in it can be arbitrarily deep in subtrees whose content paints
    // after. One traversal cannot produce that order.
    //
    // The collect walk descends *through* non-unit boxes and stops at units, which
    // is what puts a positioned box in one subtree and a positioned box in another
    // into the same ordered list -- the thing tree order could not do, and the
    // reason old.reddit.com's header bar could not be ordered against its sidebar.
    std::vector<StackingUnit> units;
    const auto collect = [&units](const Box& parent, gfx::FloatPoint scroll_from_sc,
                                  std::vector<InterveningClip> clips_from_sc,
                                  auto& recurse) -> void {
      for (const std::unique_ptr<Box>& child : parent.Children()) {
        const bool unit = PaintsAsUnit(*child);
        if (unit) {
          units.push_back(StackingUnit{child.get(), PaintLayer(*child), units.size(),
                                       scroll_from_sc, clips_from_sc});
        }
        // A unit that is **not** a stacking context is descended into anyway, and
        // this line is load-bearing: `z-index: auto` means "order me, but let my own
        // positioned descendants be ordered against my siblings" (CSS 2.1 E.2), so
        // this context is the one that has to collect them. Stopping here instead
        // dropped them from the page entirely -- old.reddit.com went from 1055
        // display-list commands to 40, because nearly everything on it is inside a
        // positioned box with no z-index.
        //
        // Recursing here does not double-paint: a non-context unit paints only its
        // *non*-unit children when its turn comes.
        if (!unit || !IsStackingContext(*child)) {
          gfx::FloatPoint into = scroll_from_sc;
          std::vector<InterveningClip> clips_into = clips_from_sc;
          AccumulateOverflowForCollect(*child, into, clips_into);
          recurse(*child, into, std::move(clips_into), recurse);
        }
      }
    };
    collect(box, gfx::FloatPoint{}, {}, collect);
    // Stable in tree order within a layer, which is the tie-break the specification
    // names and the only one that makes a page deterministic.
    std::stable_sort(units.begin(), units.end(), [](const StackingUnit& a, const StackingUnit& b) {
      return a.layer != b.layer ? a.layer < b.layer : a.order < b.order;
    });

    const auto paint_units = [&](int from, int to, bool inclusive) {
      for (const StackingUnit& unit : units) {
        if (unit.layer >= from && (inclusive ? unit.layer <= to : unit.layer < to)) {
          const gfx::FloatPoint unit_offset{child_offset.x - unit.scroll_delta.x,
                                            child_offset.y - unit.scroll_delta.y};
          // Intervening overflow clips the tree walk would have PushClip'd while
          // descending to this unit (TD-0030). Abspos keeps the existing exception
          // shared with hit-testing — see SkipsInterveningOverflowClip.
          int pushed = 0;
          if (!SkipsInterveningOverflowClip(*unit.box)) {
            for (const InterveningClip& clip : unit.intervening_clips) {
              const float x = clip.padding_box.x + child_offset.x - clip.scroll_before.x;
              const float y = clip.padding_box.y + child_offset.y - clip.scroll_before.y;
              out.PushClip(gfx::IntRect{
                  static_cast<int>(std::floor(x)),
                  static_cast<int>(std::floor(y)),
                  static_cast<int>(std::ceil(clip.padding_box.width)),
                  static_cast<int>(std::ceil(clip.padding_box.height)),
              });
              ++pushed;
            }
          }
          self(*unit.box, unit_offset, child_frame, IsStackingContext(*unit.box), paint_opacity,
               self);
          for (int i = 0; i < pushed; ++i) {
            out.PopClip();
          }
        }
      }
    };
    constexpr int kLowest = std::numeric_limits<int>::min();
    constexpr int kHighest = std::numeric_limits<int>::max();

    paint_units(kLowest, 0, false);
    for (const std::unique_ptr<Box>& child : box.Children()) {
      if (!PaintsAsUnit(*child)) {
        self(*child, child_offset, child_frame, false, paint_opacity, self);
      }
    }
    paint_units(0, 1, false);
    // Inclusive at the top, because `z-index: 2147483647` is a real thing a page
    // writes to be above everything and the arithmetic must not be what stops it.
    paint_units(1, kHighest, true);
    if (clips) {
      out.PopClip();
    }
    pop_transform();
  };
  paint(root, document_offset, root_frame, true, 1.0f, paint);
  AddPerformanceCounter(PerfCounterId::LayoutDisplayListsBuilt);
}

}  // namespace microbrowser::layout

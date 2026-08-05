#include "layout/LayoutEngine.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "html/FormControl.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::layout {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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
  const auto paint = [&out](const Box& box, gfx::FloatPoint offset, const PaintFrame& frame,
                            auto& self) -> void {
    const css::ComputedStyle& style = box.Style();
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
      const gfx::FontRequest font = FontRequestFor(style);
      for (const TextFragment& fragment : box.Fragments()) {
        const std::string_view piece(box.Text().data() + fragment.begin, fragment.length);
        // The baseline, not the top of the line box. They differ by an ascent,
        // and using the wrong one puts every line of text a line too low.
        out.DrawText(piece, fragment.rect.width, font,
                     gfx::FloatPoint{fragment.rect.x + offset.x, fragment.baseline + offset.y},
                     style.color);
      }
      return;
    }
    const gfx::FloatRect unshifted = box.Geometry().BorderBox();
    const gfx::FloatRect border_box{unshifted.x + offset.x, unshifted.y + offset.y,
                                    unshifted.width, unshifted.height};

    if (!style.background_color.IsFullyTransparent() && !border_box.IsEmpty()) {
      gfx::Path background;
      background.AddRect(border_box);
      out.FillPath(background, style.background_color);
    }
    // Over the colour and under the border, which is the order CSS paints a
    // background in and the reason a page can put a translucent image over a
    // solid colour and get both.
    PaintBackgroundImage(box, border_box, out);
    if (style.has_border && !border_box.IsEmpty()) {
      const float width = style.border_width.top.Resolve(style.font_size);
      if (width > 0.0f) {
        gfx::Path outline;
        // Inset by half the stroke width so the border lands inside the border
        // box rather than straddling its edge.
        outline.AddRect(gfx::FloatRect{border_box.x + width * 0.5f, border_box.y + width * 0.5f,
                                       std::max(0.0f, border_box.width - width),
                                       std::max(0.0f, border_box.height - width)});
        gfx::StrokeStyle stroke;
        stroke.width = width;
        out.StrokePath(outline, stroke, style.border_color);
      }
    }

    if (box.GetKind() == Box::Kind::Replaced) {
      const gfx::FloatRect content = box.Geometry().content;
      if (box.Image() != nullptr && box.Image()->IsValid()) {
        // The used size, not the intrinsic one: a declared width scales the
        // image, which is what an <img width=40> on a 400px file means.
        out.DrawImage(box.Image(),
                      gfx::EnclosingIntRect(gfx::FloatRect{content.x + offset.x,
                                                           content.y + offset.y, content.width,
                                                           content.height}));
      }
      if (!box.Text().empty()) {
        const gfx::FontRequest font = FontRequestFor(style);
        const float baseline = content.y + content.height * 0.5f + style.font_size * 0.3f;
        out.DrawText(box.Text(), std::max(0.0f, content.width - 8.0f), font,
                     gfx::FloatPoint{content.x + offset.x + 4.0f, baseline + offset.y},
                     style.color);
      }
      PaintCheckedInputIndicator(box, out, offset);
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
    // Two passes, and the second holds exactly the two positions that are a
    // function of the scroll: `sticky` and `fixed`. Both exist to sit *over*
    // the content they do not move with, and tree order put them in the display
    // list first, so every later sibling drew on top of them -- a sticky header
    // that sticks underneath the article renders as though the feature were
    // absent.
    //
    // Deliberately not every positioned box, which is what CSS 2.1 Appendix E
    // actually says. Hoisting `relative` and `absolute` too was tried and it
    // broke old.reddit.com: its header bar's background is a positioned box in
    // one subtree and the subreddit list is in another, and ordering *between*
    // subtrees is what a stacking context decides rather than what tree order
    // can. That is session 21. These two are the cases that are wrong without
    // any of it.
    for (int pass = 0; pass < 2; ++pass) {
      for (const std::unique_ptr<Box>& child : box.Children()) {
        const css::Position position = child->Style().position;
        const bool over = position == css::Position::Sticky || position == css::Position::Fixed;
        if (over != (pass == 1)) {
          continue;
        }
        self(*child, child_offset, child_frame, self);
      }
    }
    if (clips) {
      out.PopClip();
    }
  };
  paint(root, document_offset, root_frame, paint);
  AddPerformanceCounter(PerfCounterId::LayoutDisplayListsBuilt);
}

}  // namespace microbrowser::layout

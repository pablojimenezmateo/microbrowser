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
    return length.IsPercent() ? box_extent * length.value / 100.0f
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
    if (length.IsPercent()) {
      return (box_extent - tile_extent) * length.value / 100.0f;
    }
    return length.Resolve(style.font_size, 0.0f);
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

}  // namespace

void BuildDisplayList(const Box& root, gfx::DisplayList& out, gfx::FloatPoint offset) {
  // Backgrounds and borders paint before content, which is what makes a child
  // draw on top of its parent's background rather than under it.
  const auto paint = [&out, offset](const Box& box, auto& self) -> void {
    const css::ComputedStyle& style = box.Style();
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
    // Scrolling is the other half of overflow and is not here: a scroller
    // clips what is outside it *and* offers the rest back, and only the
    // clipping half is paint's. Until there is a scroll offset per box, a
    // `scroll` box shows its first screenful, which is what it shows before
    // anyone scrolls it anyway.
    const bool clips = box.ClipsOverflow();
    if (clips) {
      const gfx::FloatRect padding = box.Geometry().PaddingBox();
      out.PushClip(gfx::IntRect{
          static_cast<int>(std::floor(padding.x + offset.x)),
          static_cast<int>(std::floor(padding.y + offset.y)),
          static_cast<int>(std::ceil(padding.width)),
          static_cast<int>(std::ceil(padding.height)),
      });
    }
    for (const std::unique_ptr<Box>& child : box.Children()) {
      self(*child, self);
    }
    if (clips) {
      out.PopClip();
    }
  };
  paint(root, paint);
  AddPerformanceCounter(PerfCounterId::LayoutDisplayListsBuilt);
}

}  // namespace microbrowser::layout

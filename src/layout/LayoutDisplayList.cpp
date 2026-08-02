#include "layout/LayoutEngine.h"

#include <algorithm>
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

    for (const std::unique_ptr<Box>& child : box.Children()) {
      self(*child, self);
    }
  };
  paint(root, paint);
  AddPerformanceCounter(PerfCounterId::LayoutDisplayListsBuilt);
}

}  // namespace microbrowser::layout

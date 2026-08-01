#include "gfx/DisplayList.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "gfx/TextRenderer.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How far a stroke can reach beyond the geometry it follows. A miter tip sits
// at half the width divided by the sine of the half angle, which the miter
// limit caps at `miter_limit * width / 2`; a square cap reaches the corner of a
// half-width square, which is sqrt(2)/2 of the width.
//
// Over-estimating here costs a slightly wider damage rect. Under-estimating
// leaves stale pixels on screen, so the rounding goes one way only.
float StrokeOutset(const StrokeStyle& style) {
  if (!std::isfinite(style.width) || style.width <= 0.0f) {
    return 0.0f;
  }
  const float miter = std::isfinite(style.miter_limit) ? std::max(style.miter_limit, 1.0f) : 1.0f;
  return style.width * 0.5f * std::max(miter, 1.5f);
}

IntRect BoundsOfPath(const Path& path) {
  return EnclosingIntRect(path.ControlBounds());
}

// Where a text run can put ink, without asking a font.
//
// Damage is computed from a display list alone — the compositor side has no
// font stack, and in the process-split future it is on the other side of the
// sandbox from one. So this over-estimates deliberately: a font's ink can climb
// well above its ascent (accents, a tall integral sign) and drop below its
// descent, and a diacritic can sit outside the advance at either end.
// Over-estimating costs a wider damage rect; under-estimating leaves stale
// pixels on screen, so the rounding goes one way only.
IntRect TextInkBounds(const DisplayList::TextRun& run, const FontRequest& font,
                      FloatPoint origin) {
  const float size = std::isfinite(font.size) && font.size > 0.0f ? font.size : 0.0f;
  const float advance = std::max(run.advance, 0.0f);
  return EnclosingIntRect(FloatRect{origin.x - size, origin.y - size * 2.0f,
                                    advance + size * 2.0f, size * 3.0f});
}

}  // namespace

void DisplayList::Clear() {
  // clear(), not a fresh vector: display lists are rebuilt every frame, and
  // reusing the capacity is what keeps painting off the allocator.
  commands_.clear();
  paths_.clear();
  texts_.clear();
  fonts_.clear();
  images_.clear();
}

void DisplayList::FillRect(const IntRect& rect, Color color) {
  if (rect.IsEmpty() || color.IsFullyTransparent()) {
    return;
  }
  commands_.emplace_back(FillRectCommand{rect, color});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::PushClip(const IntRect& rect) {
  commands_.emplace_back(PushClipCommand{rect});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::PopClip() {
  commands_.emplace_back(PopClipCommand{});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::FillPath(const Path& path, Color color, FillRule rule) {
  if (path.IsEmpty() || color.IsFullyTransparent()) {
    return;
  }
  paths_.push_back(path);
  commands_.emplace_back(
      FillPathCommand{static_cast<std::uint32_t>(paths_.size() - 1), rule, color});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::StrokePath(const Path& path, const StrokeStyle& style, Color color) {
  if (path.IsEmpty() || color.IsFullyTransparent() || !std::isfinite(style.width) ||
      style.width <= 0.0f) {
    return;
  }
  paths_.push_back(path);
  commands_.emplace_back(
      StrokePathCommand{static_cast<std::uint32_t>(paths_.size() - 1), color, style});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::DrawText(std::string_view text, float advance, const FontRequest& font,
                           FloatPoint origin, Color color) {
  if (text.empty() || color.IsFullyTransparent() || !std::isfinite(advance) ||
      !std::isfinite(origin.x) || !std::isfinite(origin.y)) {
    return;
  }
  std::uint32_t font_index = 0;
  while (font_index < fonts_.size() && !(fonts_[font_index] == font)) {
    ++font_index;
  }
  if (font_index == fonts_.size()) {
    fonts_.push_back(font);
  }
  texts_.push_back(TextRun{std::string(text), advance});
  commands_.emplace_back(DrawTextCommand{static_cast<std::uint32_t>(texts_.size() - 1), font_index,
                                         origin, color});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

void DisplayList::DrawImage(std::shared_ptr<const Image> image, const IntRect& destination) {
  if (image == nullptr || !image->IsValid() || destination.IsEmpty() ||
      !IsWithinDeviceRange(destination)) {
    return;
  }
  std::uint32_t index = 0;
  while (index < images_.size() && images_[index] != image) {
    ++index;
  }
  if (index == images_.size()) {
    images_.push_back(std::move(image));
  }
  commands_.emplace_back(DrawImageCommand{index, destination});
  AddPerformanceCounter(PerfCounterId::DisplayListCommands);
}

IntRect DisplayList::Bounds() const {
  IntRect bounds;
  // A clipped command cannot paint outside its clip, so the bound must not
  // claim it can. Without this, a text run drawn inside a small field reports
  // damage for the ink box its font *could* need -- which is deliberately
  // generous -- and that box escapes the widget it was clipped to.
  std::vector<IntRect> clips;
  const auto current_clip = [&clips]() -> const IntRect* {
    return clips.empty() ? nullptr : &clips.back();
  };
  const auto add = [&bounds, &current_clip](IntRect rect) {
    if (const IntRect* clip = current_clip()) {
      rect = rect.Intersected(*clip);
    }
    bounds = bounds.United(rect);
  };

  for (const DisplayCommand& command : commands_) {
    if (const auto* push = std::get_if<PushClipCommand>(&command)) {
      // Nested clips intersect: a clip can only ever narrow.
      clips.push_back(clips.empty() ? push->rect : clips.back().Intersected(push->rect));
      continue;
    }
    if (std::holds_alternative<PopClipCommand>(command)) {
      if (!clips.empty()) {
        clips.pop_back();
      }
      continue;
    }
    if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
      add(fill->rect);
    } else if (const auto* fill_path = std::get_if<FillPathCommand>(&command)) {
      if (const Path* geometry = PathAt(fill_path->path)) {
        add(BoundsOfPath(*geometry));
      }
    } else if (const auto* stroke = std::get_if<StrokePathCommand>(&command)) {
      if (const Path* geometry = PathAt(stroke->path)) {
        // SaturateFloatToInt, not static_cast: the stroke width can arrive from a
        // compromised renderer, and ceil(1e38) reaching a raw cast is undefined
        // behavior rather than a very wide damage rect.
        const int outset = SaturateFloatToInt(std::ceil(StrokeOutset(stroke->style)));
        add(BoundsOfPath(*geometry).Inflated(outset));
      }
    } else if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      const TextRun* run = TextAt(text->text);
      const FontRequest* font = FontAt(text->font);
      if (run != nullptr && font != nullptr) {
        add(TextInkBounds(*run, *font, text->origin));
      }
    } else if (const auto* image = std::get_if<DrawImageCommand>(&command)) {
      add(image->destination);
    }
  }
  return bounds;
}

void Execute(const DisplayList& list, Painter& painter, const IntRect& damage,
             TextRenderer* text_renderer) {
  AddPerformanceCounter(PerfCounterId::DisplayListExecutions);

  // Rect commands go through the canvas directly, which knows nothing about the
  // painter's transform -- so the translation is applied here. Only the
  // translation: a rotated FillRect is not a rect, and pretending otherwise
  // would silently drop the rotation rather than refusing it.
  //
  // Without this, a list executed under a translation would draw its paths and
  // text in one place and its rects and clips in another, which is exactly what
  // compositing the page below the browser chrome does.
  const int offset_x = SaturateFloatToInt(painter.Transform().E());
  const int offset_y = SaturateFloatToInt(painter.Transform().F());
  const auto placed = [offset_x, offset_y](const IntRect& rect) {
    return rect.Translated(offset_x, offset_y);
  };

  Canvas& canvas = painter.Target();
  const IntRect region = damage.Intersected(canvas.Bounds());
  if (region.IsEmpty()) {
    return;
  }

  const std::size_t entry_depth = canvas.ClipDepth();
  canvas.PushClip(region);

  for (const DisplayCommand& command : list.Commands()) {
    if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
      canvas.FillRect(placed(fill->rect), fill->color);
    } else if (const auto* push = std::get_if<PushClipCommand>(&command)) {
      canvas.PushClip(placed(push->rect));
    } else if (const auto* fill_path = std::get_if<FillPathCommand>(&command)) {
      if (const Path* geometry = list.PathAt(fill_path->path)) {
        painter.FillPath(*geometry, fill_path->color, fill_path->rule);
      }
    } else if (const auto* stroke = std::get_if<StrokePathCommand>(&command)) {
      if (const Path* geometry = list.PathAt(stroke->path)) {
        painter.StrokePath(*geometry, stroke->style, stroke->color);
      }
    } else if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      const DisplayList::TextRun* run = list.TextAt(text->text);
      const FontRequest* font = list.FontAt(text->font);
      if (text_renderer != nullptr && run != nullptr && font != nullptr) {
        text_renderer->DrawRun(painter, run->text, *font, text->origin, text->color);
      }
    } else if (const auto* image = std::get_if<DrawImageCommand>(&command)) {
      if (const Image* pixels = list.ImageAt(image->image)) {
        painter.DrawImage(*pixels, image->destination);
      }
    } else {
      // PopClip. Refuse to pop past our own damage clip: an unbalanced list
      // would otherwise widen the clip beyond the damage region and paint
      // outside it.
      if (canvas.ClipDepth() > entry_depth + 1) {
        canvas.PopClip();
      }
    }
  }

  // Restore whatever depth the caller had, regardless of how balanced the list
  // turned out to be.
  while (canvas.ClipDepth() > entry_depth) {
    canvas.PopClip();
  }
}

}  // namespace microbrowser::gfx

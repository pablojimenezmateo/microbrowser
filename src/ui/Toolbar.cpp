#include "ui/Toolbar.h"

#include <algorithm>

namespace microbrowser::ui {

namespace {

constexpr int kButtonSize = 28;
constexpr int kGap = 4;
constexpr int kEdge = 4;

constexpr gfx::Color kChromeBackground = gfx::Color::Rgb(0xF2, 0xF2, 0xF5);
constexpr gfx::Color kChromeBorder = gfx::Color::Rgb(0xD0, 0xD0, 0xD8);
constexpr gfx::Color kFieldBackground = gfx::Color::Rgb(0xFF, 0xFF, 0xFF);
constexpr gfx::Color kFieldBorder = gfx::Color::Rgb(0xC0, 0xC0, 0xC8);
constexpr gfx::Color kFieldBorderFocused = gfx::Color::Rgb(0x1F, 0x6F, 0xEB);
constexpr gfx::Color kSelection = gfx::Color::Rgb(0xB4, 0xD5, 0xFE);
constexpr gfx::Color kGlyph = gfx::Color::Rgb(0x30, 0x30, 0x38);
constexpr gfx::Color kGlyphDisabled = gfx::Color::Rgb(0xA8, 0xA8, 0xB0);
constexpr gfx::Color kText = gfx::Color::Rgb(0x18, 0x18, 0x1C);

// A triangle pointing left or right, and a circle-ish arc for reload. Drawn
// rather than shipped as icon files: three glyphs do not justify an image
// format, an asset pipeline, and a licence.
gfx::Path Chevron(const gfx::IntRect& button, bool pointing_left) {
  const float mid_y = static_cast<float>(button.y) + static_cast<float>(button.height) * 0.5f;
  const float near_x = static_cast<float>(button.x) + static_cast<float>(button.width) * 0.36f;
  const float far_x = static_cast<float>(button.x) + static_cast<float>(button.width) * 0.62f;
  const float rise = static_cast<float>(button.height) * 0.20f;

  gfx::Path path;
  if (pointing_left) {
    path.MoveTo(gfx::FloatPoint{near_x, mid_y});
    path.LineTo(gfx::FloatPoint{far_x, mid_y - rise});
    path.LineTo(gfx::FloatPoint{far_x, mid_y + rise});
  } else {
    path.MoveTo(gfx::FloatPoint{far_x, mid_y});
    path.LineTo(gfx::FloatPoint{near_x, mid_y - rise});
    path.LineTo(gfx::FloatPoint{near_x, mid_y + rise});
  }
  path.Close();
  return path;
}

gfx::Path ReloadArc(const gfx::IntRect& button) {
  // Three quarters of a circle, so the gap reads as an arrow-less reload glyph
  // rather than as a full ring that could be a progress spinner.
  const float cx = static_cast<float>(button.x) + static_cast<float>(button.width) * 0.5f;
  const float cy = static_cast<float>(button.y) + static_cast<float>(button.height) * 0.5f;
  const float r = static_cast<float>(std::min(button.width, button.height)) * 0.28f;
  const float k = r * 0.5523f;  // circle-to-cubic constant

  gfx::Path path;
  path.MoveTo(gfx::FloatPoint{cx, cy - r});
  path.CubicTo(gfx::FloatPoint{cx + k, cy - r}, gfx::FloatPoint{cx + r, cy - k},
               gfx::FloatPoint{cx + r, cy});
  path.CubicTo(gfx::FloatPoint{cx + r, cy + k}, gfx::FloatPoint{cx + k, cy + r},
               gfx::FloatPoint{cx, cy + r});
  path.CubicTo(gfx::FloatPoint{cx - k, cy + r}, gfx::FloatPoint{cx - r, cy + k},
               gfx::FloatPoint{cx - r, cy});
  return path;
}

}  // namespace

gfx::FontRequest Toolbar::OmniboxFont() {
  gfx::FontRequest font;
  font.family = "sans-serif";
  font.size = 14.0f;
  return font;
}

void Toolbar::SetWidth(int width) {
  width_ = std::max(0, width);
  const int top = (kHeight - kButtonSize) / 2;
  int x = kEdge;
  back_ = gfx::IntRect{x, top, kButtonSize, kButtonSize};
  x += kButtonSize + kGap;
  forward_ = gfx::IntRect{x, top, kButtonSize, kButtonSize};
  x += kButtonSize + kGap;
  reload_ = gfx::IntRect{x, top, kButtonSize, kButtonSize};
  x += kButtonSize + kGap;
  omnibox_rect_ = gfx::IntRect{x, top, std::max(0, width_ - x - kEdge), kButtonSize};
}

gfx::IntRect Toolbar::OmniboxTextRect() const {
  constexpr int kPadding = 6;
  return gfx::IntRect{omnibox_rect_.x + kPadding, omnibox_rect_.y,
                      std::max(0, omnibox_rect_.width - 2 * kPadding), omnibox_rect_.height};
}

Toolbar::Part Toolbar::HitTest(gfx::IntPoint point) const {
  if (!Bounds().Contains(point)) {
    return Part::Outside;
  }
  if (back_.Contains(point)) {
    return Part::Back;
  }
  if (forward_.Contains(point)) {
    return Part::Forward;
  }
  if (reload_.Contains(point)) {
    return Part::Reload;
  }
  if (omnibox_rect_.Contains(point)) {
    return Part::Omnibox;
  }
  return Part::None;
}

void Toolbar::Paint(gfx::DisplayList& out, const OmniboxMetrics& metrics) const {
  if (width_ <= 0) {
    return;
  }

  out.FillRect(Bounds(), kChromeBackground);
  out.FillRect(gfx::IntRect{0, kHeight - 1, width_, 1}, kChromeBorder);

  // Buttons. A disabled one is drawn greyed rather than hidden: a control that
  // disappears when it stops working moves everything next to it.
  out.FillPath(Chevron(back_, true), can_go_back_ ? kGlyph : kGlyphDisabled);
  out.FillPath(Chevron(forward_, false), can_go_forward_ ? kGlyph : kGlyphDisabled);

  gfx::StrokeStyle arc;
  arc.width = 2.0f;
  arc.cap = gfx::LineCap::Round;
  out.StrokePath(ReloadArc(reload_), arc, kGlyph);

  // The field.
  out.FillRect(omnibox_rect_, kFieldBackground);
  gfx::StrokeStyle field_border;
  field_border.width = omnibox_focused_ ? 2.0f : 1.0f;
  const float inset = field_border.width * 0.5f;
  gfx::Path outline;
  outline.AddRect(gfx::FloatRect{static_cast<float>(omnibox_rect_.x) + inset,
                                 static_cast<float>(omnibox_rect_.y) + inset,
                                 static_cast<float>(omnibox_rect_.width) - field_border.width,
                                 static_cast<float>(omnibox_rect_.height) - field_border.width});
  out.StrokePath(outline, field_border, omnibox_focused_ ? kFieldBorderFocused : kFieldBorder);

  const gfx::IntRect text_rect = OmniboxTextRect();
  if (text_rect.width <= 0) {
    return;
  }

  // Clipped to the field, so a URL longer than the box does not paint across
  // the buttons. The clip is why the display list has a clip stack at all.
  out.PushClip(text_rect);

  const gfx::FontRequest font = OmniboxFont();
  const float baseline =
      static_cast<float>(text_rect.y) + static_cast<float>(text_rect.height) * 0.5f + 5.0f;

  if (omnibox_focused_ && omnibox_.HasSelection()) {
    // Under the text, so the glyphs stay readable: a selection that painted
    // over them would need a second text colour and a second draw.
    const int begin = text_rect.x + gfx::SaturateFloatToInt(metrics.selection_begin);
    const int end = text_rect.x + gfx::SaturateFloatToInt(metrics.selection_end);
    out.FillRect(gfx::IntRect{begin, text_rect.y + 4, std::max(0, end - begin),
                              text_rect.height - 8},
                 kSelection);
  }

  if (!omnibox_.Text().empty()) {
    out.DrawText(omnibox_.Text(), static_cast<float>(text_rect.width), font,
                 gfx::FloatPoint{static_cast<float>(text_rect.x), baseline}, kText);
  }

  if (omnibox_focused_ && !omnibox_.HasSelection()) {
    const int caret_x = text_rect.x + gfx::SaturateFloatToInt(metrics.caret);
    out.FillRect(gfx::IntRect{caret_x, text_rect.y + 4, 1, text_rect.height - 8}, kText);
  }

  out.PopClip();
}

}  // namespace microbrowser::ui

#include "css/Animation.h"

#include <algorithm>
#include <cmath>

#include "css/ComputedStyle.h"
#include "css/CssText.h"

namespace microbrowser::css {

namespace {

// A length interpolated between two others.
//
// **The units have to match**, and when they do not this returns the endpoint rather than mixing them.
// That is not a shortcut: `10px` to `50%` cannot be interpolated without a containing block, which the
// cascade does not have -- and the specification's answer is to produce a `calc()` of both, which this
// browser's `Length` can express only for one relative term plus an absolute offset. Rather than
// produce a wrong number, a mismatched pair snaps at the halfway point, which is a discrete animation
// and is what the timing model already has a word for.
//
// `auto` is the important case of that: `width: auto` to `width: 200px` is not interpolable in any
// browser, because `auto` is not a number until layout has run.
Length InterpolateLength(const Length& from, const Length& to, double t) {
  if (from.IsAuto() || to.IsAuto() || from.unit != to.unit) {
    return t < 0.5 ? from : to;
  }
  Length out = from;
  out.value = static_cast<float>(static_cast<double>(from.value) +
                                (static_cast<double>(to.value) - static_cast<double>(from.value)) * t);
  // The `calc()` offset travels with the value, or `calc(100% - 20px)` animating to `calc(100% - 40px)`
  // would keep the first offset for the whole transition and jump at the end.
  out.offset = static_cast<float>(
      static_cast<double>(from.offset) +
      (static_cast<double>(to.offset) - static_cast<double>(from.offset)) * t);
  return out;
}

// A colour interpolated in premultiplied alpha, which is what the specification requires and what
// stops the classic artefact: `rgba(255,0,0,1)` to `rgba(0,0,255,0)` interpolated per channel passes
// through a half-transparent purple, and premultiplied it fades the red out without ever being purple.
gfx::Color InterpolateColor(const gfx::Color& from, const gfx::Color& to, double t) {
  const double from_alpha = static_cast<double>(from.Alpha()) / 255.0;
  const double to_alpha = static_cast<double>(to.Alpha()) / 255.0;
  const double alpha = std::clamp(from_alpha + (to_alpha - from_alpha) * t, 0.0, 1.0);
  const auto channel = [&](std::uint8_t from_value, std::uint8_t to_value) {
    const double premultiplied_from = static_cast<double>(from_value) * from_alpha;
    const double premultiplied_to = static_cast<double>(to_value) * to_alpha;
    const double mixed = premultiplied_from + (premultiplied_to - premultiplied_from) * t;
    if (alpha <= 0.0) {
      return std::uint8_t{0};
    }
    // Clamped, because a channel's range *is* known -- unlike a length's. A `cubic-bezier` that
    // overshoots would otherwise wrap a colour round through zero, which reads as a flash of the
    // complementary colour.
    return static_cast<std::uint8_t>(std::lround(std::clamp(mixed / alpha, 0.0, 255.0)));
  };
  return gfx::Color::Rgba(channel(from.Red(), to.Red()), channel(from.Green(), to.Green()),
                          channel(from.Blue(), to.Blue()),
                          static_cast<std::uint8_t>(std::lround(alpha * 255.0)));
}

// Two transform lists interpolated.
//
// **Only when they have the same operations in the same order**, which is the specification's first
// case and the one every real page hits: `translateX(0)` to `translateX(100px)`, or a `scale` to a
// `scale`. When the lists differ the specification says to interpolate the *matrices*, decomposing
// each into translate/rotate/scale/skew -- which this browser does not do, so it snaps at the halfway
// point instead. That is a stated approximation: a page animating `rotate(0)` to
// `translateX(10px) rotate(90deg)` gets a jump rather than a wrong path, and a jump is visible while a
// wrong path looks like a design decision.
bool InterpolateTransform(const TransformList& from, const TransformList& to, double t,
                          TransformList& out) {
  if (from.operations.size() != to.operations.size()) {
    out = t < 0.5 ? from : to;
    return true;
  }
  out = from;
  for (std::size_t i = 0; i < from.operations.size(); ++i) {
    const TransformOperation& a = from.operations[i];
    const TransformOperation& b = to.operations[i];
    if (a.kind != b.kind) {
      out = t < 0.5 ? from : to;
      return true;
    }
    TransformOperation& result = out.operations[i];
    result.length_x = InterpolateLength(a.length_x, b.length_x, t);
    result.length_y = InterpolateLength(a.length_y, b.length_y, t);
    // The six matrix coefficients, interpolated componentwise. For a rotation that is *not* the same as
    // interpolating the angle -- it takes the chord rather than the arc -- and for a 90-degree rotation
    // the difference is visible. Recorded here as the approximation it is; doing it properly needs the
    // angle kept beside the matrix, which is a change to `TransformOperation` and not to this function.
    const auto mix = [t](float from_value, float to_value) {
      const double start = static_cast<double>(from_value);
      return static_cast<float>(start + (static_cast<double>(to_value) - start) * t);
    };
    result.a = mix(a.a, b.a);
    result.b = mix(a.b, b.b);
    result.c = mix(a.c, b.c);
    result.d = mix(a.d, b.d);
    result.e = mix(a.e, b.e);
    result.f = mix(a.f, b.f);
  }
  return true;
}

}  // namespace

AnimatableProperty AnimatablePropertyFromName(std::string_view name) {
  const std::string lowered = Lowered(Trim(name));
  if (lowered == "color") {
    return AnimatableProperty::Color;
  }
  if (lowered == "background-color") {
    return AnimatableProperty::BackgroundColor;
  }
  if (lowered == "border-color") {
    return AnimatableProperty::BorderColor;
  }
  if (lowered == "width") {
    return AnimatableProperty::Width;
  }
  if (lowered == "height") {
    return AnimatableProperty::Height;
  }
  if (lowered == "margin-top") {
    return AnimatableProperty::MarginTop;
  }
  if (lowered == "margin-right") {
    return AnimatableProperty::MarginRight;
  }
  if (lowered == "margin-bottom") {
    return AnimatableProperty::MarginBottom;
  }
  if (lowered == "margin-left") {
    return AnimatableProperty::MarginLeft;
  }
  if (lowered == "padding-top") {
    return AnimatableProperty::PaddingTop;
  }
  if (lowered == "padding-right") {
    return AnimatableProperty::PaddingRight;
  }
  if (lowered == "padding-bottom") {
    return AnimatableProperty::PaddingBottom;
  }
  if (lowered == "padding-left") {
    return AnimatableProperty::PaddingLeft;
  }
  if (lowered == "top") {
    return AnimatableProperty::Top;
  }
  if (lowered == "right") {
    return AnimatableProperty::Right;
  }
  if (lowered == "bottom") {
    return AnimatableProperty::Bottom;
  }
  if (lowered == "left") {
    return AnimatableProperty::Left;
  }
  if (lowered == "font-size") {
    return AnimatableProperty::FontSize;
  }
  if (lowered == "line-height") {
    return AnimatableProperty::LineHeight;
  }
  if (lowered == "border-width") {
    return AnimatableProperty::BorderWidth;
  }
  if (lowered == "transform") {
    return AnimatableProperty::Transform;
  }
  return AnimatableProperty::None;
}

bool InterpolateProperty(AnimatableProperty property, const ComputedStyle& from,
                         const ComputedStyle& to, double t, ComputedStyle& out) {
  switch (property) {
    case AnimatableProperty::Color:
      out.color = InterpolateColor(from.color, to.color, t);
      return true;
    case AnimatableProperty::BackgroundColor:
      out.background_color = InterpolateColor(from.background_color, to.background_color, t);
      return true;
    case AnimatableProperty::BorderColor:
      // Four sides, and each of them may be `currentColor` -- which is not a colour to interpolate
      // from but a deferred read of `color`. A side that is unset on either end is resolved against
      // that end's own `color` first, so `border-color: currentColor` to `red` animates from the
      // colour the text actually was.
      for (std::size_t side = 0; side < 4; ++side) {
        out.border_color[side] =
            InterpolateColor(from.BorderColorFor(side), to.BorderColorFor(side), t);
      }
      return true;
    case AnimatableProperty::Width:
      out.width = InterpolateLength(from.width, to.width, t);
      return true;
    case AnimatableProperty::Height:
      out.height = InterpolateLength(from.height, to.height, t);
      return true;
    case AnimatableProperty::MarginTop:
      out.margin.top = InterpolateLength(from.margin.top, to.margin.top, t);
      return true;
    case AnimatableProperty::MarginRight:
      out.margin.right = InterpolateLength(from.margin.right, to.margin.right, t);
      return true;
    case AnimatableProperty::MarginBottom:
      out.margin.bottom = InterpolateLength(from.margin.bottom, to.margin.bottom, t);
      return true;
    case AnimatableProperty::MarginLeft:
      out.margin.left = InterpolateLength(from.margin.left, to.margin.left, t);
      return true;
    case AnimatableProperty::PaddingTop:
      out.padding.top = InterpolateLength(from.padding.top, to.padding.top, t);
      return true;
    case AnimatableProperty::PaddingRight:
      out.padding.right = InterpolateLength(from.padding.right, to.padding.right, t);
      return true;
    case AnimatableProperty::PaddingBottom:
      out.padding.bottom = InterpolateLength(from.padding.bottom, to.padding.bottom, t);
      return true;
    case AnimatableProperty::PaddingLeft:
      out.padding.left = InterpolateLength(from.padding.left, to.padding.left, t);
      return true;
    case AnimatableProperty::Top:
      out.inset.top = InterpolateLength(from.inset.top, to.inset.top, t);
      return true;
    case AnimatableProperty::Right:
      out.inset.right = InterpolateLength(from.inset.right, to.inset.right, t);
      return true;
    case AnimatableProperty::Bottom:
      out.inset.bottom = InterpolateLength(from.inset.bottom, to.inset.bottom, t);
      return true;
    case AnimatableProperty::Left:
      out.inset.left = InterpolateLength(from.inset.left, to.inset.left, t);
      return true;
    case AnimatableProperty::FontSize:
      out.font_size = static_cast<float>(
          static_cast<double>(from.font_size) +
          (static_cast<double>(to.font_size) - static_cast<double>(from.font_size)) * t);
      return true;
    case AnimatableProperty::LineHeight:
      out.line_height = static_cast<float>(
          static_cast<double>(from.line_height) +
          (static_cast<double>(to.line_height) - static_cast<double>(from.line_height)) * t);
      return true;
    case AnimatableProperty::BorderWidth:
      out.border_width.top = InterpolateLength(from.border_width.top, to.border_width.top, t);
      out.border_width.right = InterpolateLength(from.border_width.right, to.border_width.right, t);
      out.border_width.bottom =
          InterpolateLength(from.border_width.bottom, to.border_width.bottom, t);
      out.border_width.left = InterpolateLength(from.border_width.left, to.border_width.left, t);
      return true;
    case AnimatableProperty::Transform:
      return InterpolateTransform(from.transform, to.transform, t, out.transform);
    case AnimatableProperty::None:
      break;
  }
  return false;
}

}  // namespace microbrowser::css

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "css/Length.h"
#include "gfx/Color.h"

// The SVG presentation properties (ADR 0043 §2).
//
// Its own header rather than more lines in `ComputedStyle.h`, which is at its
// module's cap: a file over its cap means a missing header, not a bigger one.
// The split is also the honest one -- nothing outside SVG painting reads any of
// this, and `ComputedStyle` holds it as a single grouped member.

namespace microbrowser::css {

// The SVG painting properties whose values are small closed sets. One enum
// each rather than one shared one: `fill-rule` and `clip-rule` genuinely take
// the same two values (so they share), and nothing else here does.
enum class FillRule : std::uint8_t { NonZero, EvenOdd };
enum class StrokeLinecap : std::uint8_t { Butt, Round, Square };
enum class StrokeLinejoin : std::uint8_t { Miter, Round, Bevel, MiterClip, Arcs };
enum class TextAnchor : std::uint8_t { Start, Middle, End };
enum class ColorInterpolation : std::uint8_t { Auto, SRgb, LinearRgb };
// `shape-rendering`, `image-rendering` and `text-rendering` share this only in
// the two values they all have; each keeps its own hint values, and a value
// that belongs to one property is refused on the others by the parser rather
// than by the type.
enum class RenderingHint : std::uint8_t {
  Auto,
  OptimizeSpeed,
  CrispEdges,
  GeometricPrecision,
  OptimizeQuality,
  OptimizeLegibility,
  Pixelated,
  SmoothOrHighQuality,
};
enum class DominantBaseline : std::uint8_t {
  Auto,
  TextBottom,
  Alphabetic,
  Ideographic,
  Middle,
  Central,
  Mathematical,
  Hanging,
  TextTop,
};
enum class AlignmentBaseline : std::uint8_t {
  Baseline,
  TextBottom,
  Alphabetic,
  Ideographic,
  Middle,
  Central,
  Mathematical,
  Hanging,
  TextTop,
};
enum class VectorEffect : std::uint8_t { None, NonScalingStroke };
enum class MaskType : std::uint8_t { Luminance, Alpha };

// The SVG painting properties, grouped.
//
// Grouped for the reason `FlexStyle` and `GridStyle` are: twenty-one fields
// for one feature, read together and by one part of the engine. Loose on
// `ComputedStyle` they would be a third of its members and would say nothing
// about belonging to each other.
//
// **Every one of them inherits**, which is unusual enough to be worth stating
// once here rather than per field: SVG's painting model is that a `<g>` sets
// the paint for everything under it. `InheritInto` copies the whole struct,
// so a field added here inherits by default -- which is the right default for
// this group and is why it is one struct rather than two.
struct SvgStyle {
  // `<paint>` is `none | <color> | <url> [none | <color>]?`. The reference is
  // kept as the text between `url(` and `)` rather than resolved, because
  // resolving it needs the paint servers of ADR 0043 §4 and this struct is
  // built by the cascade, which cannot see the tree it would resolve against.
  enum class PaintKind : std::uint8_t { None, Color, Reference };
  struct Paint {
    PaintKind kind = PaintKind::Color;
    gfx::Color color{0xFF000000};
    // Empty unless `kind` is `Reference`. A `std::string` rather than an id,
    // for the same reason a custom property's value is unparsed text: it
    // means nothing until something looks it up, and that is not here.
    std::string reference;
    friend bool operator==(const Paint&, const Paint&) = default;
  };
  Paint fill;                                          // initial: black
  Paint stroke{PaintKind::None, gfx::Color{0}, {}};    // initial: none
  float fill_opacity = 1.0f;
  float stroke_opacity = 1.0f;
  float stop_opacity = 1.0f;
  float flood_opacity = 1.0f;
  gfx::Color stop_color{0xFF000000};
  gfx::Color flood_color{0xFF000000};
  gfx::Color lighting_color{0xFFFFFFFF};
  FillRule fill_rule = FillRule::NonZero;
  FillRule clip_rule = FillRule::NonZero;
  Length stroke_width = Length::Pixels(1.0f);
  Length stroke_dashoffset = Length::Pixels(0.0f);
  // Empty is `none`, which is the initial value. A vector rather than the
  // canonical text because the stroker will want the numbers and a second
  // parse of our own serialization is a second chance to disagree with it.
  std::vector<Length> stroke_dasharray;
  float stroke_miterlimit = 4.0f;
  StrokeLinecap stroke_linecap = StrokeLinecap::Butt;
  StrokeLinejoin stroke_linejoin = StrokeLinejoin::Miter;
  TextAnchor text_anchor = TextAnchor::Start;
  ColorInterpolation color_interpolation = ColorInterpolation::SRgb;
  ColorInterpolation color_interpolation_filters = ColorInterpolation::LinearRgb;
  RenderingHint shape_rendering = RenderingHint::Auto;
  RenderingHint image_rendering = RenderingHint::Auto;
  RenderingHint text_rendering = RenderingHint::Auto;
  DominantBaseline dominant_baseline = DominantBaseline::Auto;
  AlignmentBaseline alignment_baseline = AlignmentBaseline::Baseline;
  VectorEffect vector_effect = VectorEffect::None;
  MaskType mask_type = MaskType::Luminance;
  // `paint-order`, as the three layers in the order they are painted: two bits
  // each, fill = 0, stroke = 1, markers = 2. Stored as an order rather than as
  // the keyword the page wrote, because `normal` and `fill stroke markers` name
  // the same order and nothing downstream may be able to tell which one a page
  // wrote -- which is also why `normal` is this packing rather than a zero of
  // its own.
  static constexpr std::uint8_t kPaintOrderNormal = 0 | (1 << 2) | (2 << 4);
  std::uint8_t paint_order = kPaintOrderNormal;

  friend bool operator==(const SvgStyle&, const SvgStyle&) = default;
};

}  // namespace microbrowser::css

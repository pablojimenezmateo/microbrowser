#include "engine/SvgComputedValues.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "css/SvgStyle.h"
#include "gfx/Color.h"
#include "gfx/ColorText.h"

// The computed values of the SVG presentation properties (ADR 0043 §2).
//
// Split out of `GeometryQueries.cpp` because that file is at its module's line
// cap, and a file over its cap means a missing translation unit rather than a
// bigger file. The split is also the honest one: twenty-eight properties that
// no other part of `getComputedStyle` shares a line with.

namespace microbrowser::engine {

namespace {

// The three number and colour spellings this file needs. Deliberately the same
// two functions `GeometryQueries.cpp` uses -- a colour written two ways is two
// answers to one question -- so they call into `gfx` rather than formatting.
std::string Number(float value) {
  if (value == static_cast<float>(static_cast<long long>(value))) {
    return std::to_string(static_cast<long long>(value));
  }
  std::string text = std::to_string(value);
  while (text.size() > 1 && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

std::string ColorText(const gfx::Color& color) { return gfx::SerializeColorText(color); }

std::string LengthText(const css::Length& length, float font_size) {
  switch (length.unit) {
    case css::Length::Unit::Auto:
      return "auto";
    case css::Length::Unit::Percent:
      return Number(length.value) + "%";
    default:
      break;
  }
  return Number(length.Resolve(font_size)) + "px";
}

}  // namespace

// The SVG presentation properties (ADR 0043 §2).
//
// A function of its own rather than more branches in the chain below, because
// there are twenty-eight of them and because the chain is a linear scan: an
// early `if` costs every later property a comparison, and the SVG ones are the
// least likely to be asked on an ordinary page.
std::optional<std::string> SvgComputedValueOf(const css::ComputedStyle& style,
                                              std::string_view property) {
  const css::SvgStyle& svg = style.svg;
  const auto paint = [](const css::SvgStyle::Paint& value) -> std::string {
    switch (value.kind) {
      case css::SvgStyle::PaintKind::None:
        return "none";
      case css::SvgStyle::PaintKind::Reference:
        return "url(\"" + value.reference + "\")";
      case css::SvgStyle::PaintKind::Color:
        break;
    }
    return ColorText(value.color);
  };
  const auto rendering = [](css::RenderingHint hint) -> std::string {
    switch (hint) {
      case css::RenderingHint::Auto: return "auto";
      case css::RenderingHint::OptimizeSpeed: return "optimizeSpeed";
      case css::RenderingHint::CrispEdges: return "crispEdges";
      case css::RenderingHint::GeometricPrecision: return "geometricPrecision";
      case css::RenderingHint::OptimizeQuality: return "optimizeQuality";
      case css::RenderingHint::OptimizeLegibility: return "optimizeLegibility";
      case css::RenderingHint::Pixelated: return "pixelated";
      case css::RenderingHint::SmoothOrHighQuality: return "smooth";
    }
    return "auto";
  };
  const auto interpolation = [](css::ColorInterpolation value) -> std::string {
    switch (value) {
      case css::ColorInterpolation::Auto: return "auto";
      case css::ColorInterpolation::SRgb: return "srgb";
      case css::ColorInterpolation::LinearRgb: return "linearrgb";
    }
    return "srgb";
  };

  if (property == "fill") return paint(svg.fill);
  if (property == "stroke") return paint(svg.stroke);
  if (property == "fill-opacity") return Number(svg.fill_opacity);
  if (property == "stroke-opacity") return Number(svg.stroke_opacity);
  if (property == "stop-opacity") return Number(svg.stop_opacity);
  if (property == "flood-opacity") return Number(svg.flood_opacity);
  if (property == "stop-color") return ColorText(svg.stop_color);
  if (property == "flood-color") return ColorText(svg.flood_color);
  if (property == "lighting-color") return ColorText(svg.lighting_color);
  if (property == "fill-rule") {
    return std::string(svg.fill_rule == css::FillRule::EvenOdd ? "evenodd" : "nonzero");
  }
  if (property == "clip-rule") {
    return std::string(svg.clip_rule == css::FillRule::EvenOdd ? "evenodd" : "nonzero");
  }
  if (property == "stroke-width") return LengthText(svg.stroke_width, style.font_size);
  if (property == "stroke-dashoffset") return LengthText(svg.stroke_dashoffset, style.font_size);
  if (property == "stroke-dasharray") {
    if (svg.stroke_dasharray.empty()) {
      return std::string("none");
    }
    std::string out;
    for (const css::Length& length : svg.stroke_dasharray) {
      if (!out.empty()) {
        out += ", ";
      }
      out += LengthText(length, style.font_size);
    }
    return out;
  }
  if (property == "stroke-miterlimit") return Number(svg.stroke_miterlimit);
  if (property == "stroke-linecap") {
    switch (svg.stroke_linecap) {
      case css::StrokeLinecap::Butt: return std::string("butt");
      case css::StrokeLinecap::Round: return std::string("round");
      case css::StrokeLinecap::Square: return std::string("square");
    }
  }
  if (property == "stroke-linejoin") {
    switch (svg.stroke_linejoin) {
      case css::StrokeLinejoin::Miter: return std::string("miter");
      case css::StrokeLinejoin::Round: return std::string("round");
      case css::StrokeLinejoin::Bevel: return std::string("bevel");
      case css::StrokeLinejoin::MiterClip: return std::string("miter-clip");
      case css::StrokeLinejoin::Arcs: return std::string("arcs");
    }
  }
  if (property == "text-anchor") {
    switch (svg.text_anchor) {
      case css::TextAnchor::Start: return std::string("start");
      case css::TextAnchor::Middle: return std::string("middle");
      case css::TextAnchor::End: return std::string("end");
    }
  }
  if (property == "color-interpolation") return interpolation(svg.color_interpolation);
  if (property == "color-interpolation-filters") {
    return interpolation(svg.color_interpolation_filters);
  }
  if (property == "shape-rendering") return rendering(svg.shape_rendering);
  if (property == "image-rendering") return rendering(svg.image_rendering);
  if (property == "text-rendering") return rendering(svg.text_rendering);
  if (property == "dominant-baseline") {
    switch (svg.dominant_baseline) {
      case css::DominantBaseline::Auto: return std::string("auto");
      case css::DominantBaseline::TextBottom: return std::string("text-bottom");
      case css::DominantBaseline::Alphabetic: return std::string("alphabetic");
      case css::DominantBaseline::Ideographic: return std::string("ideographic");
      case css::DominantBaseline::Middle: return std::string("middle");
      case css::DominantBaseline::Central: return std::string("central");
      case css::DominantBaseline::Mathematical: return std::string("mathematical");
      case css::DominantBaseline::Hanging: return std::string("hanging");
      case css::DominantBaseline::TextTop: return std::string("text-top");
    }
  }
  if (property == "alignment-baseline") {
    switch (svg.alignment_baseline) {
      case css::AlignmentBaseline::Baseline: return std::string("baseline");
      case css::AlignmentBaseline::TextBottom: return std::string("text-bottom");
      case css::AlignmentBaseline::Alphabetic: return std::string("alphabetic");
      case css::AlignmentBaseline::Ideographic: return std::string("ideographic");
      case css::AlignmentBaseline::Middle: return std::string("middle");
      case css::AlignmentBaseline::Central: return std::string("central");
      case css::AlignmentBaseline::Mathematical: return std::string("mathematical");
      case css::AlignmentBaseline::Hanging: return std::string("hanging");
      case css::AlignmentBaseline::TextTop: return std::string("text-top");
    }
  }
  if (property == "vector-effect") {
    return std::string(svg.vector_effect == css::VectorEffect::NonScalingStroke
                           ? "non-scaling-stroke"
                           : "none");
  }
  if (property == "mask-type") {
    return std::string(svg.mask_type == css::MaskType::Alpha ? "alpha" : "luminance");
  }
  if (property == "paint-order") {
    if (svg.paint_order == css::SvgStyle::kPaintOrderNormal) {
      return std::string("normal");
    }
    static constexpr std::string_view kLayers[] = {"fill", "stroke", "markers"};
    std::string out;
    for (int i = 0; i < 3; ++i) {
      const auto layer = static_cast<std::size_t>((svg.paint_order >> (i * 2)) & 0x3);
      if (layer > 2) {
        return std::string("normal");
      }
      if (!out.empty()) {
        out += ' ';
      }
      out += kLayers[layer];
    }
    return out;
  }
  return std::nullopt;
}

}  // namespace microbrowser::engine

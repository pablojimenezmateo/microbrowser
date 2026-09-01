#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "css/SvgStyle.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

// The SVG presentation properties (ADR 0043 §2).
//
// Its own translation unit for the reason `BoxDeclarations.cpp` is one:
// `Declarations.cpp` is at the module's line cap, and these are properties that
// arrive together and are read together.
//
// **They are ordinary CSS properties and this file is what makes that true.**
// Before it, `getComputedStyle(rect).fill` was the empty string, which is not
// "black" and not "unknown" -- it is the answer a property that does not exist
// gives, and it is why every later stage of ADR 0043 had nothing to build on.
// A presentation *attribute* (`<rect fill="blue">`) is a declaration of the
// same-named property at the lowest specificity, and that mapping is in
// `StyleResolver.cpp` beside HTML's, because it is the same mechanism.

namespace microbrowser::css {

namespace {

// Every SVG presentation attribute, as a sorted list of the property names they
// map to (SVG 2 §"Presentation attributes"). The attribute *is* the property
// name in every case, so there is no table of pairs -- only a membership test.
//
// A list rather than "try `ApplyDeclaration` on every attribute" for two
// reasons the suite states directly: an attribute is a presentation attribute
// only on elements the property applies to (`svg/styling/
// presentation-attributes-irrelevant.html`), and an attribute that is *not* one
// must not become a declaration however well its name parses -- `<rect
// transform="…">` is a presentation attribute and `<rect id="…">` is not, and
// `id` would otherwise be tried against the cascade once per element.
constexpr std::string_view kSvgPresentationAttributes[] = {
    "alignment-baseline",
    "baseline-shift",
    "clip-path",
    "clip-rule",
    "color",
    "color-interpolation",
    "color-interpolation-filters",
    "cursor",
    "direction",
    "display",
    "dominant-baseline",
    "fill",
    "fill-opacity",
    "fill-rule",
    "filter",
    "flood-color",
    "flood-opacity",
    "font-family",
    "font-size",
    "font-size-adjust",
    "font-stretch",
    "font-style",
    "font-variant",
    "font-weight",
    "image-rendering",
    "letter-spacing",
    "lighting-color",
    "marker-end",
    "marker-mid",
    "marker-start",
    "mask",
    "mask-type",
    "opacity",
    "overflow",
    "paint-order",
    "pointer-events",
    "shape-rendering",
    "stop-color",
    "stop-opacity",
    "stroke",
    "stroke-dasharray",
    "stroke-dashoffset",
    "stroke-linecap",
    "stroke-linejoin",
    "stroke-miterlimit",
    "stroke-opacity",
    "stroke-width",
    "text-anchor",
    "text-decoration",
    "text-overflow",
    "text-rendering",
    "transform",
    "transform-origin",
    "unicode-bidi",
    "vector-effect",
    "visibility",
    "white-space",
    "word-spacing",
    "writing-mode",
};

bool IsSvgPresentationAttribute(std::string_view name) {
  const auto found = std::lower_bound(std::begin(kSvgPresentationAttributes),
                                      std::end(kSvgPresentationAttributes), name);
  return found != std::end(kSvgPresentationAttributes) && *found == name;
}

std::string_view Trim(std::string_view text) { return util::TrimAscii(text); }

// `<opacity-value>`: a number or a percentage, clamped to [0, 1]. Out of range
// is *clamped* rather than refused (CSS Color 4 §"Resolving <alpha-value>"), so
// `fill-opacity="2"` is 1 and not a dropped declaration.
bool ParseOpacity(std::string_view value, float* out) {
  std::string_view text = Trim(value);
  bool percent = false;
  if (!text.empty() && text.back() == '%') {
    percent = true;
    text.remove_suffix(1);
  }
  const std::optional<double> number = util::ParseDouble(Trim(text));
  if (!number.has_value()) {
    return false;
  }
  double scaled = percent ? *number / 100.0 : *number;
  scaled = scaled < 0.0 ? 0.0 : (scaled > 1.0 ? 1.0 : scaled);
  *out = static_cast<float>(scaled);
  return true;
}

// A length whose *unitless* form is a user-space number rather than an error.
// SVG's lengths are the one place in CSS where `stroke-width="2"` is legal, and
// treating a bare number as invalid would drop the most common way the property
// is ever written.
bool ParseSvgLength(std::string_view value, const MediaContext& context, float font_size,
                    Length* out) {
  const std::string_view text = Trim(value);
  if (text.empty()) {
    return false;
  }
  if (const std::optional<double> number = util::ParseDouble(text); number.has_value()) {
    *out = Length::Pixels(static_cast<float>(*number));
    return true;
  }
  const std::optional<Length> parsed = ParseLength(text, context, font_size);
  if (!parsed.has_value()) {
    return false;
  }
  *out = *parsed;
  return true;
}

// `<paint>`, as far as this stage of ADR 0043 goes: `none`, a colour, or a
// `url(...)` kept as text. The optional fallback after a reference is parsed
// and *dropped* rather than refused -- a page that writes `url(#g) red` means
// the reference when the server resolves, and the fallback is the paint-server
// question of §4.
bool ParsePaint(std::string_view value, SvgStyle::Paint* out) {
  std::string_view text = Trim(value);
  if (text.empty()) {
    return false;
  }
  if (util::EqualsAsciiCaseInsensitive(text, "none")) {
    out->kind = SvgStyle::PaintKind::None;
    out->reference.clear();
    return true;
  }
  if (text.size() > 4 && util::EqualsAsciiCaseInsensitive(text.substr(0, 4), "url(")) {
    const std::size_t close = text.find(')');
    if (close == std::string_view::npos) {
      return false;
    }
    std::string_view target = Trim(text.substr(4, close - 4));
    if (target.size() >= 2 && (target.front() == '"' || target.front() == '\'') &&
        target.back() == target.front()) {
      target = target.substr(1, target.size() - 2);
    }
    out->kind = SvgStyle::PaintKind::Reference;
    out->reference = std::string(target);
    return true;
  }
  const std::optional<gfx::Color> color = ParseColor(text);
  if (!color.has_value()) {
    return false;
  }
  out->kind = SvgStyle::PaintKind::Color;
  out->color = *color;
  out->reference.clear();
  return true;
}

bool ParseFillRule(std::string_view value, FillRule* out) {
  if (value == "nonzero") {
    *out = FillRule::NonZero;
    return true;
  }
  if (value == "evenodd") {
    *out = FillRule::EvenOdd;
    return true;
  }
  return false;
}

// `stroke-dasharray`: `none` or a list of lengths, comma- and/or
// space-separated. A *negative* entry, or a list that is all zeros, makes the
// whole declaration invalid -- SVG says so explicitly, and a dash pattern of
// zeros is an infinite loop in any stroker that takes it at face value.
bool ParseDashArray(std::string_view value, const MediaContext& context, float font_size,
                    std::vector<Length>* out) {
  const std::string_view text = Trim(value);
  if (util::EqualsAsciiCaseInsensitive(text, "none")) {
    out->clear();
    return true;
  }
  std::vector<Length> lengths;
  std::size_t at = 0;
  bool any_positive = false;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ' ' || text[at] == ',' || text[at] == '\t' ||
                                text[at] == '\n' || text[at] == '\r' || text[at] == '\f')) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < text.size() && text[at] != ' ' && text[at] != ',' && text[at] != '\t' &&
           text[at] != '\n' && text[at] != '\r' && text[at] != '\f') {
      ++at;
    }
    if (at == begin) {
      break;
    }
    Length length;
    if (!ParseSvgLength(text.substr(begin, at - begin), context, font_size, &length)) {
      return false;
    }
    const float resolved = length.Resolve(font_size);
    if (resolved < 0.0f) {
      return false;
    }
    any_positive = any_positive || resolved > 0.0f;
    lengths.push_back(length);
  }
  if (lengths.empty() || !any_positive) {
    return false;
  }
  *out = std::move(lengths);
  return true;
}

// `paint-order`, packed as the permutation it names. `normal` is fill, stroke,
// markers; a partial list is completed in the canonical order, so `stroke` is
// `stroke fill markers`. Stored as the order rather than as the keyword because
// two different keywords name the same order and a renderer must not be able to
// tell them apart.
bool ParsePaintOrder(std::string_view value, std::uint8_t* out) {
  if (util::EqualsAsciiCaseInsensitive(Trim(value), "normal")) {
    *out = SvgStyle::kPaintOrderNormal;
    return true;
  }
  // 0 = fill, 1 = stroke, 2 = markers.
  std::uint8_t order[3] = {0, 0, 0};
  bool seen[3] = {false, false, false};
  std::size_t count = 0;
  std::size_t at = 0;
  const std::string_view text = Trim(value);
  while (at < text.size()) {
    while (at < text.size() && util::IsHtmlWhitespace(text[at])) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < text.size() && !util::IsHtmlWhitespace(text[at])) {
      ++at;
    }
    if (at == begin) {
      break;
    }
    const std::string_view word = text.substr(begin, at - begin);
    std::uint8_t layer = 0;
    if (word == "fill") {
      layer = 0;
    } else if (word == "stroke") {
      layer = 1;
    } else if (word == "markers") {
      layer = 2;
    } else {
      return false;
    }
    if (seen[layer] || count == 3) {
      return false;  // a repeated layer is invalid, not a later-wins list
    }
    seen[layer] = true;
    order[count++] = layer;
  }
  if (count == 0) {
    return false;
  }
  for (std::uint8_t layer = 0; layer < 3 && count < 3; ++layer) {
    if (!seen[layer]) {
      order[count++] = layer;
    }
  }
  // Three layers, two bits each. `fill stroke markers` packs to exactly what
  // `normal` does, which is the point: the two spell the same order.
  *out = static_cast<std::uint8_t>(order[0] | (order[1] << 2) | (order[2] << 4));
  return true;
}

bool ParseColorInterpolation(std::string_view value, ColorInterpolation* out) {
  if (value == "auto") {
    *out = ColorInterpolation::Auto;
  } else if (util::EqualsAsciiCaseInsensitive(value, "srgb")) {
    *out = ColorInterpolation::SRgb;
  } else if (util::EqualsAsciiCaseInsensitive(value, "linearrgb")) {
    *out = ColorInterpolation::LinearRgb;
  } else {
    return false;
  }
  return true;
}

// The three `*-rendering` properties. Each takes `auto` plus its own hints, and
// the sets do not agree -- `pixelated` is `image-rendering` only,
// `optimizeLegibility` is `text-rendering` only. Refusing the wrong one here is
// what makes `CSS.supports('shape-rendering', 'pixelated')` answer no.
bool ParseRenderingHint(std::string_view property, std::string_view value, RenderingHint* out) {
  if (value == "auto") {
    *out = RenderingHint::Auto;
    return true;
  }
  if (util::EqualsAsciiCaseInsensitive(value, "optimizespeed")) {
    *out = RenderingHint::OptimizeSpeed;
    return true;
  }
  if (property == "shape-rendering") {
    if (util::EqualsAsciiCaseInsensitive(value, "crispedges")) {
      *out = RenderingHint::CrispEdges;
      return true;
    }
    if (util::EqualsAsciiCaseInsensitive(value, "geometricprecision")) {
      *out = RenderingHint::GeometricPrecision;
      return true;
    }
    return false;
  }
  if (property == "image-rendering") {
    if (util::EqualsAsciiCaseInsensitive(value, "optimizequality")) {
      *out = RenderingHint::OptimizeQuality;
      return true;
    }
    if (value == "pixelated") {
      *out = RenderingHint::Pixelated;
      return true;
    }
    if (value == "crisp-edges" || util::EqualsAsciiCaseInsensitive(value, "crispedges")) {
      *out = RenderingHint::CrispEdges;
      return true;
    }
    if (value == "smooth" || value == "high-quality") {
      *out = RenderingHint::SmoothOrHighQuality;
      return true;
    }
    return false;
  }
  // text-rendering
  if (util::EqualsAsciiCaseInsensitive(value, "optimizelegibility")) {
    *out = RenderingHint::OptimizeLegibility;
    return true;
  }
  if (util::EqualsAsciiCaseInsensitive(value, "geometricprecision")) {
    *out = RenderingHint::GeometricPrecision;
    return true;
  }
  return false;
}

bool ParseDominantBaseline(std::string_view value, DominantBaseline* out) {
  if (value == "auto") {
    *out = DominantBaseline::Auto;
  } else if (value == "text-bottom") {
    *out = DominantBaseline::TextBottom;
  } else if (value == "alphabetic") {
    *out = DominantBaseline::Alphabetic;
  } else if (value == "ideographic") {
    *out = DominantBaseline::Ideographic;
  } else if (value == "middle") {
    *out = DominantBaseline::Middle;
  } else if (value == "central") {
    *out = DominantBaseline::Central;
  } else if (value == "mathematical") {
    *out = DominantBaseline::Mathematical;
  } else if (value == "hanging") {
    *out = DominantBaseline::Hanging;
  } else if (value == "text-top") {
    *out = DominantBaseline::TextTop;
  } else {
    return false;
  }
  return true;
}

bool ParseAlignmentBaseline(std::string_view value, AlignmentBaseline* out) {
  if (value == "baseline") {
    *out = AlignmentBaseline::Baseline;
  } else if (value == "text-bottom") {
    *out = AlignmentBaseline::TextBottom;
  } else if (value == "alphabetic") {
    *out = AlignmentBaseline::Alphabetic;
  } else if (value == "ideographic") {
    *out = AlignmentBaseline::Ideographic;
  } else if (value == "middle") {
    *out = AlignmentBaseline::Middle;
  } else if (value == "central") {
    *out = AlignmentBaseline::Central;
  } else if (value == "mathematical") {
    *out = AlignmentBaseline::Mathematical;
  } else if (value == "hanging") {
    *out = AlignmentBaseline::Hanging;
  } else if (value == "text-top") {
    *out = AlignmentBaseline::TextTop;
  } else {
    return false;
  }
  return true;
}

}  // namespace

bool ApplySvgDeclaration(std::string_view property, std::string_view value,
                         const ComputedStyle& parent, ComputedStyle& style,
                         const MediaContext& context) {
  (void)parent;
  SvgStyle& svg = style.svg;
  const float font_size = style.font_size;

  if (property == "fill") {
    return ParsePaint(value, &svg.fill);
  }
  if (property == "stroke") {
    return ParsePaint(value, &svg.stroke);
  }
  if (property == "fill-opacity") {
    return ParseOpacity(value, &svg.fill_opacity);
  }
  if (property == "stroke-opacity") {
    return ParseOpacity(value, &svg.stroke_opacity);
  }
  if (property == "stop-opacity") {
    return ParseOpacity(value, &svg.stop_opacity);
  }
  if (property == "flood-opacity") {
    return ParseOpacity(value, &svg.flood_opacity);
  }
  if (property == "stop-color" || property == "flood-color" || property == "lighting-color") {
    // `currentColor` is the initial value of `stop-color` and `flood-color` and
    // resolves against this element's own `color`, which the cascade has
    // already applied -- these three are the only SVG properties that take it.
    gfx::Color resolved;
    if (util::EqualsAsciiCaseInsensitive(Trim(value), "currentcolor")) {
      resolved = style.color;
    } else {
      const std::optional<gfx::Color> parsed = ParseColor(Trim(value));
      if (!parsed.has_value()) {
        return false;
      }
      resolved = *parsed;
    }
    if (property == "stop-color") {
      svg.stop_color = resolved;
    } else if (property == "flood-color") {
      svg.flood_color = resolved;
    } else {
      svg.lighting_color = resolved;
    }
    return true;
  }
  if (property == "fill-rule") {
    return ParseFillRule(value, &svg.fill_rule);
  }
  if (property == "clip-rule") {
    return ParseFillRule(value, &svg.clip_rule);
  }
  if (property == "stroke-width") {
    Length length;
    if (!ParseSvgLength(value, context, font_size, &length) || length.Resolve(font_size) < 0.0f) {
      return false;
    }
    svg.stroke_width = length;
    return true;
  }
  if (property == "stroke-dashoffset") {
    // Signed, unlike `stroke-width`: a negative offset shifts the pattern
    // backwards and is legal.
    Length length;
    if (!ParseSvgLength(value, context, font_size, &length)) {
      return false;
    }
    svg.stroke_dashoffset = length;
    return true;
  }
  if (property == "stroke-dasharray") {
    return ParseDashArray(value, context, font_size, &svg.stroke_dasharray);
  }
  if (property == "stroke-miterlimit") {
    const std::optional<double> number = util::ParseDouble(Trim(value));
    if (!number.has_value() || *number < 1.0) {
      return false;  // below 1 is invalid, not clamped
    }
    svg.stroke_miterlimit = static_cast<float>(*number);
    return true;
  }
  if (property == "stroke-linecap") {
    if (value == "butt") {
      svg.stroke_linecap = StrokeLinecap::Butt;
    } else if (value == "round") {
      svg.stroke_linecap = StrokeLinecap::Round;
    } else if (value == "square") {
      svg.stroke_linecap = StrokeLinecap::Square;
    } else {
      return false;
    }
    return true;
  }
  if (property == "stroke-linejoin") {
    if (value == "miter") {
      svg.stroke_linejoin = StrokeLinejoin::Miter;
    } else if (value == "round") {
      svg.stroke_linejoin = StrokeLinejoin::Round;
    } else if (value == "bevel") {
      svg.stroke_linejoin = StrokeLinejoin::Bevel;
    } else if (value == "miter-clip") {
      svg.stroke_linejoin = StrokeLinejoin::MiterClip;
    } else if (value == "arcs") {
      svg.stroke_linejoin = StrokeLinejoin::Arcs;
    } else {
      return false;
    }
    return true;
  }
  if (property == "text-anchor") {
    if (value == "start") {
      svg.text_anchor = TextAnchor::Start;
    } else if (value == "middle") {
      svg.text_anchor = TextAnchor::Middle;
    } else if (value == "end") {
      svg.text_anchor = TextAnchor::End;
    } else {
      return false;
    }
    return true;
  }
  if (property == "color-interpolation") {
    return ParseColorInterpolation(value, &svg.color_interpolation);
  }
  if (property == "color-interpolation-filters") {
    // The one difference from `color-interpolation`: `auto` is allowed and the
    // initial value is linearRGB rather than sRGB.
    return ParseColorInterpolation(value, &svg.color_interpolation_filters);
  }
  if (property == "shape-rendering") {
    return ParseRenderingHint(property, value, &svg.shape_rendering);
  }
  if (property == "image-rendering") {
    return ParseRenderingHint(property, value, &svg.image_rendering);
  }
  if (property == "text-rendering") {
    return ParseRenderingHint(property, value, &svg.text_rendering);
  }
  if (property == "dominant-baseline") {
    return ParseDominantBaseline(value, &svg.dominant_baseline);
  }
  if (property == "alignment-baseline") {
    return ParseAlignmentBaseline(value, &svg.alignment_baseline);
  }
  if (property == "vector-effect") {
    if (value == "none") {
      svg.vector_effect = VectorEffect::None;
    } else if (value == "non-scaling-stroke") {
      svg.vector_effect = VectorEffect::NonScalingStroke;
    } else {
      return false;
    }
    return true;
  }
  if (property == "mask-type") {
    if (util::EqualsAsciiCaseInsensitive(value, "luminance")) {
      svg.mask_type = MaskType::Luminance;
    } else if (util::EqualsAsciiCaseInsensitive(value, "alpha")) {
      svg.mask_type = MaskType::Alpha;
    } else {
      return false;
    }
    return true;
  }
  if (property == "paint-order") {
    return ParsePaintOrder(value, &svg.paint_order);
  }
  return false;
}

std::vector<Declaration> SvgPresentationDeclarations(const dom::Element& element) {
  std::vector<Declaration> declarations;
  for (const dom::Attribute& present : element.Attributes()) {
    // Namespaced attributes are never presentation attributes: `xlink:href` is
    // not `href`, and the qualified name is what a page wrote.
    if (!present.name_space.IsNone() || !IsSvgPresentationAttribute(present.name)) {
      continue;
    }
    declarations.push_back(Declaration{present.name, present.value, false});
  }
  return declarations;
}

}  // namespace microbrowser::css

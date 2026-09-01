#pragma once

#include <string_view>

// **Which SVG tag gets which interface**, and the hierarchy above it.
//
// A second table beside `TagInterfaces.h` rather than more rows in it, because
// the two are keyed differently and folding them would lose exactly the
// distinction that makes either correct: HTML's key is a *lower-cased* tag name
// in the HTML namespace, SVG's is a **case-sensitive** local name in the SVG
// namespace. `clipPath`, `linearGradient` and `feGaussianBlur` are the names,
// and a case-insensitive lookup would answer `SVGClipPathElement` for
// `<clippath>`, which is an element SVG does not define.
//
// ADR 0043 §3. Private to the module, like `TagInterfaces.h`.

namespace microbrowser::bindings {

// One SVG interface and its parent. Unlike HTML's table this carries interfaces
// with *no* tag: `SVGGraphicsElement` and `SVGGeometryElement` are pure
// intermediates, and `rect instanceof SVGGeometryElement` is what the suite
// asks about far more often than it asks about `SVGRectElement` itself.
struct SvgInterface {
  // Empty for an intermediate that no tag names.
  std::string_view tag;
  const char* interface;
  // Null means `SVGElement`.
  const char* parent = nullptr;
};

// **In dependency order**: a parent appears before every child, because the
// installer looks each parent up by name as it goes and a forward reference
// would silently reparent onto `SVGElement`.
constexpr SvgInterface kSvgInterfaces[] = {
    // The three intermediates first.
    {"", "SVGGraphicsElement"},
    {"", "SVGGeometryElement", "SVGGraphicsElement"},
    {"", "SVGTextContentElement", "SVGGraphicsElement"},
    {"", "SVGTextPositioningElement", "SVGTextContentElement"},
    {"", "SVGGradientElement"},
    {"", "SVGAnimationElement"},
    {"", "SVGComponentTransferFunctionElement"},

    // Containers and structure.
    {"svg", "SVGSVGElement", "SVGGraphicsElement"},
    {"g", "SVGGElement", "SVGGraphicsElement"},
    {"defs", "SVGDefsElement", "SVGGraphicsElement"},
    {"symbol", "SVGSymbolElement", "SVGGraphicsElement"},
    {"use", "SVGUseElement", "SVGGraphicsElement"},
    {"switch", "SVGSwitchElement", "SVGGraphicsElement"},
    {"a", "SVGAElement", "SVGGraphicsElement"},
    {"foreignObject", "SVGForeignObjectElement", "SVGGraphicsElement"},
    {"image", "SVGImageElement", "SVGGraphicsElement"},
    {"desc", "SVGDescElement"},
    {"title", "SVGTitleElement"},
    {"metadata", "SVGMetadataElement"},
    {"style", "SVGStyleElement"},
    {"script", "SVGScriptElement"},
    {"view", "SVGViewElement"},

    // Shapes. Every one of them is a geometry element, which is the interface
    // `getTotalLength` and `isPointInFill` live on.
    {"path", "SVGPathElement", "SVGGeometryElement"},
    {"rect", "SVGRectElement", "SVGGeometryElement"},
    {"circle", "SVGCircleElement", "SVGGeometryElement"},
    {"ellipse", "SVGEllipseElement", "SVGGeometryElement"},
    {"line", "SVGLineElement", "SVGGeometryElement"},
    {"polyline", "SVGPolylineElement", "SVGGeometryElement"},
    {"polygon", "SVGPolygonElement", "SVGGeometryElement"},

    // Text.
    {"text", "SVGTextElement", "SVGTextPositioningElement"},
    {"tspan", "SVGTSpanElement", "SVGTextPositioningElement"},
    {"textPath", "SVGTextPathElement", "SVGTextContentElement"},

    // Paint servers, markers and clipping.
    {"linearGradient", "SVGLinearGradientElement", "SVGGradientElement"},
    {"radialGradient", "SVGRadialGradientElement", "SVGGradientElement"},
    {"stop", "SVGStopElement"},
    {"pattern", "SVGPatternElement"},
    {"marker", "SVGMarkerElement"},
    {"clipPath", "SVGClipPathElement"},
    {"mask", "SVGMaskElement"},

    // SMIL. The *interfaces* exist even though ADR 0043 refuses the animation
    // model, and that is not a contradiction: `SVGAnimateElement` is the type of
    // an element a document may contain, and a page that asks
    // `el instanceof SVGAnimateElement` must get the right answer whether or not
    // anything animates. The members that *drive* the timeline -- `beginElement`,
    // `getCurrentTime` -- are the part that stays absent.
    {"animate", "SVGAnimateElement", "SVGAnimationElement"},
    {"set", "SVGSetElement", "SVGAnimationElement"},
    {"animateMotion", "SVGAnimateMotionElement", "SVGAnimationElement"},
    {"animateTransform", "SVGAnimateTransformElement", "SVGAnimationElement"},
    {"mpath", "SVGMPathElement"},

    // Filters.
    {"filter", "SVGFilterElement"},
    {"feBlend", "SVGFEBlendElement"},
    {"feColorMatrix", "SVGFEColorMatrixElement"},
    {"feComponentTransfer", "SVGFEComponentTransferElement"},
    {"feFuncA", "SVGFEFuncAElement", "SVGComponentTransferFunctionElement"},
    {"feFuncB", "SVGFEFuncBElement", "SVGComponentTransferFunctionElement"},
    {"feFuncG", "SVGFEFuncGElement", "SVGComponentTransferFunctionElement"},
    {"feFuncR", "SVGFEFuncRElement", "SVGComponentTransferFunctionElement"},
    {"feComposite", "SVGFECompositeElement"},
    {"feConvolveMatrix", "SVGFEConvolveMatrixElement"},
    {"feDiffuseLighting", "SVGFEDiffuseLightingElement"},
    {"feDisplacementMap", "SVGFEDisplacementMapElement"},
    {"feDistantLight", "SVGFEDistantLightElement"},
    {"feDropShadow", "SVGFEDropShadowElement"},
    {"feFlood", "SVGFEFloodElement"},
    {"feGaussianBlur", "SVGFEGaussianBlurElement"},
    {"feImage", "SVGFEImageElement"},
    {"feMerge", "SVGFEMergeElement"},
    {"feMergeNode", "SVGFEMergeNodeElement"},
    {"feMorphology", "SVGFEMorphologyElement"},
    {"feOffset", "SVGFEOffsetElement"},
    {"fePointLight", "SVGFEPointLightElement"},
    {"feSpecularLighting", "SVGFESpecularLightingElement"},
    {"feSpotLight", "SVGFESpotLightElement"},
    {"feTile", "SVGFETileElement"},
    {"feTurbulence", "SVGFETurbulenceElement"},
};

// The interface for an element in the SVG namespace, by local name.
//
// `SVGElement` for a name SVG does not define, which is what every engine does
// and is deliberately *not* `SVGUnknownElement`: that interface exists in the
// specification, no engine returns it for a plain unknown name, and inventing a
// type is worse than a general one.
inline const char* InterfaceForSvgTag(std::string_view local_name) {
  for (const SvgInterface& entry : kSvgInterfaces) {
    if (!entry.tag.empty() && entry.tag == local_name) {
      return entry.interface;
    }
  }
  return "SVGElement";
}

}  // namespace microbrowser::bindings

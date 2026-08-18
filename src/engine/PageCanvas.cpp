// `<canvas>`, from the page's side.
//
// ADR 0029 §2, session 36. Its own translation unit for the reason PageMediaSource.cpp is one: every
// function here is a lookup plus a call, and everything that decides anything -- the graphics state, the
// bounds, the taint -- is in `engine::CanvasSurfaces`, where it can be tested without a document.

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "bindings/Canvas.h"
#include "dom/Node.h"
#include "css/ComputedStyle.h"
#include "gfx/ColorText.h"
#include "url/Url.h"
#include "util/StringUtil.h"
#include "engine/Page.h"
#include "util/Parse.h"

namespace microbrowser::engine {

bool Page::IsCanvas(const dom::Element& element) const { return element.TagName() == "canvas"; }

void Page::SetCanvasSize(dom::Element& element, int width, int height) {
  canvases_.SetSize(element, width, height);
  // The box's intrinsic size came from the backing store, so a resize is a relayout. Without this a
  // page that sizes its canvas in script draws into the new store and paints the old box.
  InvalidateLayout();
}

int Page::CanvasWidth(const dom::Element& element) const {
  const CanvasSurfaces::Surface* surface = canvases_.Find(element);
  if (surface != nullptr) {
    return surface->canvas.Width();
  }
  // No surface yet, so the answer is the *attribute* -- which is what a page reading `canvas.width`
  // before drawing expects, and what the specification says the property reflects. Falling back to the
  // default here rather than creating a surface keeps a read from allocating 300x150 of pixels.
  if (const std::string* attribute = element.GetAttribute("width")) {
    if (const std::optional<std::int64_t> parsed = util::ParseHtmlNonNegativeInteger(*attribute)) {
      return static_cast<int>(std::min<std::int64_t>(*parsed, CanvasSurfaces::kMaxCanvasPixels));
    }
  }
  return CanvasSurfaces::kDefaultWidth;
}

int Page::CanvasHeight(const dom::Element& element) const {
  const CanvasSurfaces::Surface* surface = canvases_.Find(element);
  if (surface != nullptr) {
    return surface->canvas.Height();
  }
  if (const std::string* attribute = element.GetAttribute("height")) {
    if (const std::optional<std::int64_t> parsed = util::ParseHtmlNonNegativeInteger(*attribute)) {
      return static_cast<int>(std::min<std::int64_t>(*parsed, CanvasSurfaces::kMaxCanvasPixels));
    }
  }
  return CanvasSurfaces::kDefaultHeight;
}

void Page::ExecuteCanvasOp(dom::Element& element, const bindings::CanvasOp& op) {
  // `currentColor` is resolved *here*, against the canvas element's computed `color`, and it is
  // resolved when the attribute is **set** rather than when it is painted -- which the specification
  // says and which `2d.fillStyle.parse.current.changed` measures: a page that sets `fillStyle` and
  // then changes the element's colour keeps the colour it set.
  //
  // The substitution is in this file because it is the only side that can resolve a cascade, and it
  // is a substitution rather than a flag so that the far side never has to know the keyword exists.
  if (!op.text.empty() && util::AsciiLowerCase(op.text) == "currentcolor" &&
      (op.kind == bindings::CanvasOp::Kind::SetFillColor ||
       op.kind == bindings::CanvasOp::Kind::SetStrokeColor ||
       op.kind == bindings::CanvasOp::Kind::SetShadowColor ||
       op.kind == bindings::CanvasOp::Kind::AddColorStop)) {
    bindings::CanvasOp resolved = op;
    // Opaque black when the canvas is not in a document: there is no cascade to ask, and the
    // specification names that case explicitly rather than leaving it to the initial value.
    resolved.text = element.ConnectedDocument() != nullptr
                        ? gfx::SerializeColorText(StyleWithoutBox(element).color)
                        : std::string("#000000");
    canvases_.Execute(element, resolved);
    InvalidateLayout();
    return;
  }
  // The two commands that need pixels are resolved here, because this is the object that knows what
  // an `<img>` fetched and what origin it came from. Everything else is state or geometry and goes
  // straight through.
  if (op.kind == bindings::CanvasOp::Kind::DrawImage ||
      op.kind == bindings::CanvasOp::Kind::CreatePattern) {
    bool taints = false;
    const std::shared_ptr<const gfx::Image> image = CanvasImageSource(op.source, taints);
    if (op.kind == bindings::CanvasOp::Kind::DrawImage) {
      canvases_.DrawImage(element, op, image, taints);
    } else {
      canvases_.SetPattern(element, op, image, taints);
    }
    InvalidateLayout();
    return;
  }
  canvases_.Execute(element, op);
  // Every drawing command changes what the element paints as. Marked here rather than per command,
  // because "did this draw?" is the surface's answer and asking it twice is two answers to keep in step.
  InvalidateLayout();
}

std::vector<std::uint8_t> Page::ReadCanvasPixels(const dom::Element& element, int x, int y,
                                                 int width, int height) const {
  return canvases_.ReadPixels(element, x, y, width, height);
}

bool Page::CanvasIsTainted(const dom::Element& element) const {
  const CanvasSurfaces::Surface* surface = canvases_.Find(element);
  return surface != nullptr && surface->tainted;
}

void Page::WriteCanvasPixels(dom::Element& element, int x, int y, int width, int height,
                             const std::vector<std::uint8_t>& rgba) {
  canvases_.WritePixels(element, x, y, width, height, rgba);
  InvalidateLayout();
}

double Page::MeasureCanvasText(const dom::Element& element, const std::string& text) const {
  return canvases_.MeasureText(element, text);
}

std::string Page::CanvasStateText(const dom::Element& element,
                                  bindings::CanvasOp::Kind which) const {
  return canvases_.StateText(element, which);
}

double Page::CanvasStateNumber(const dom::Element& element, bindings::CanvasOp::Kind which) const {
  return canvases_.StateNumber(element, which);
}

std::vector<double> Page::CanvasTransform(const dom::Element& element) const {
  return canvases_.Transform(element);
}

bool Page::CanvasHitTest(const dom::Element& element, double x, double y, bool stroke,
                         bool even_odd) const {
  return canvases_.HitTest(element, x, y, stroke, even_odd);
}

bool Page::CanvasParsesColor(const std::string& text) const {
  return css::ParseColor(text).has_value();
}

std::pair<int, int> Page::CanvasSourceSize(const dom::Element* source) const {
  bool ignored = false;
  const std::shared_ptr<const gfx::Image> image = CanvasImageSource(source, ignored);
  if (image == nullptr || !image->IsValid()) {
    return {0, 0};
  }
  return {image->Width(), image->Height()};
}

std::shared_ptr<const gfx::Image> Page::CanvasImageSource(const dom::Element* source,
                                                          bool& taints) const {
  taints = false;
  if (source == nullptr) {
    return nullptr;
  }
  // **The taint decision, and it is deliberately conservative.** This browser sends no
  // `crossorigin` on an image request and enforces no CORS on one, so there is no such thing here as
  // a cross-origin image a canvas may read back -- and the only safe reading of that is that every
  // cross-origin source taints. Erring the other way would be a page reading pixels of an image it
  // was never allowed to see, which is the whole reason the flag exists.
  if (source->TagName() == "img") {
    const auto selected = resources_.selected_image_urls.find(source);
    if (selected != resources_.selected_image_urls.end()) {
      const std::optional<url::Url> resolved =
          policy_.Base().has_value() ? url::Url::Parse(selected->second, *policy_.Base())
                                     : url::Url::Parse(selected->second);
      // An address that does not parse taints, rather than not. There is no image behind it either
      // way, but the flag is a security decision and "we could not tell" is not a reason to allow.
      taints = !resolved.has_value() || !policy_.IsSameOrigin(*resolved);
    }
  }
  return ImageForElement(*source);
}

}  // namespace microbrowser::engine

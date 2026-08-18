// `<canvas>`, from the page's side.
//
// ADR 0029 §2, session 36. Its own translation unit for the reason PageMediaSource.cpp is one: every
// function here is a lookup plus a call, and everything that decides anything -- the graphics state, the
// bounds, the taint -- is in `engine::CanvasSurfaces`, where it can be tested without a document.

#include <optional>
#include <string>
#include <vector>

#include "bindings/Canvas.h"
#include "dom/Node.h"
#include "css/ComputedStyle.h"
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
    if (const std::optional<float> parsed = util::ParseFloat(*attribute)) {
      return static_cast<int>(*parsed);
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
    if (const std::optional<float> parsed = util::ParseFloat(*attribute)) {
      return static_cast<int>(*parsed);
    }
  }
  return CanvasSurfaces::kDefaultHeight;
}

void Page::ExecuteCanvasOp(dom::Element& element, const bindings::CanvasOp& op) {
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

}  // namespace microbrowser::engine

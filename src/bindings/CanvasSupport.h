#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "dom/Node.h"
#include "js/Heap.h"
#include "js/Value.h"

namespace microbrowser::bindings {

// The vocabulary the three canvas translation units share.
//
// A header rather than three copies, because every one of these is a *convention* about where
// something is stored -- which canvas a context belongs to, which handle a gradient carries -- and a
// convention with three copies is a convention until one of them is edited.

// The `<canvas>` a context was made for, and the context cached on the canvas.
inline constexpr const char* kCanvasContextSlot = "#canvas-context";
inline constexpr const char* kContextCanvasSlot = "#context-canvas";
// The gradients and patterns a context has handed out, by handle, so that reading `fillStyle` back
// gives the *same object* the page assigned -- which the specification requires and which is what a
// page comparing `ctx.fillStyle === myGradient` depends on. The handle is far-side state, so
// `save()`/`restore()` restores which one is selected for free.
inline constexpr const char* kPaintRegistrySlot = "#canvas-paints";
inline constexpr const char* kPaintCounterSlot = "#canvas-paint-counter";
inline constexpr const char* kPaintHandleSlot = "#paint-handle";

inline dom::Element* CanvasOfContext(const js::Value& context) {
  if (!context.IsObject()) {
    return nullptr;
  }
  const js::Value* canvas = context.object->GetOwn(kContextCanvasSlot);
  if (canvas == nullptr) {
    return nullptr;
  }
  dom::Node* node = NodeOf(*canvas);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

// An argument as a number, with a fallback for one that was not passed. NaN is *not* filtered here:
// the specification's rule is that a non-finite argument makes the whole call a no-op, and which
// arguments a command has is the command's business rather than this function's.
inline double CanvasNumber(const std::vector<js::Value>& arguments, std::size_t index,
                           double fallback = 0.0) {
  return index >= arguments.size() ? fallback : js::ToNumber(arguments[index]);
}

// Web IDL's argument count check, which is a `TypeError` *before* anything else happens.
//
// Not decoration: `2d.conformance.requirements.missingargs` asserts it for thirty methods, and the
// reason it matters outside a test is that a page calling `ctx.fillRect(x, y, w)` has a bug, and a
// browser that filled a zero-height rectangle would hide it.
inline bool CanvasArity(js::NativeCall& call, std::size_t required, const char* name,
                        js::Value& thrown) {
  if (call.arguments.size() >= required) {
    return true;
  }
  thrown = call.Throw("TypeError",
                      std::string(name) + " requires " + std::to_string(required) +
                          " arguments, but only " + std::to_string(call.arguments.size()) +
                          " were passed");
  return false;
}

// The handle a gradient or pattern object carries, or zero when the value is not one of ours.
inline std::uint32_t PaintHandleOf(const js::Value& value) {
  if (!value.IsObject()) {
    return 0;
  }
  const js::Value* handle = value.object->GetOwn(kPaintHandleSlot);
  if (handle == nullptr) {
    return 0;
  }
  const double number = js::ToNumber(*handle);
  return number > 0.0 ? static_cast<std::uint32_t>(number) : 0;
}

}  // namespace microbrowser::bindings

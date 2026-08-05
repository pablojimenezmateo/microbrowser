#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/Geometry.h"
#include "dom/Node.h"
#include "js/Heap.h"
#include "js/Value.h"
#include "util/StringUtil.h"

namespace microbrowser::js {
class Interpreter;
}  // namespace microbrowser::js

// Shared by the binding translation units, and private to the module: a
// binding is an implementation detail of the seam, not part of its interface,
// so this header is deliberately absent from MODULE.deps' `public:` list.

namespace microbrowser::bindings {

// Where a wrapper keeps the node it stands for, and where a native keeps the
// bindings instance it belongs to. `#` names, the same convention private
// class fields and the engine's other internal slots already use.
inline constexpr const char* kNodeSlot = "#node";
inline constexpr const char* kOwnerSlot = "#bindings";
// Where the document wrapper keeps its `readyState`. A hidden property rather
// than a C++ member, because the collector can see a property and cannot see a
// `js::Value` in a field -- the same rule the wrapper cache follows.
inline constexpr const char* kReadyStateSlot = "#readyState";

// A C++ pointer, as a value script can hold but not usefully forge. It travels
// as a double, which holds a 53-bit integer exactly -- more than any address on
// a machine with canonical-form pointers.
inline js::Value PointerValue(const void* pointer) {
  return js::Value::Number(static_cast<double>(reinterpret_cast<std::uintptr_t>(pointer)));
}

// An argument, or undefined when the caller passed fewer.
//
// A local copy rather than `js::Argument`, which lives in a header `src/js`
// keeps to itself. That is the module boundary working: a binding is a
// consumer of the engine's *public* surface, and reaching past it for a
// three-line helper would be the first crack in the line this module's
// manifest calls a security boundary.
inline js::Value Argument(const std::vector<js::Value>& arguments, std::size_t index) {
  return index < arguments.size() ? arguments[index] : js::Value::Undefined();
}

class DomBindings;

// The node behind a wrapper, or null for anything that is not one.
//
// Every binding starts here rather than trusting its receiver, because a page
// can call one on anything: `Element.prototype.appendChild.call(7, x)` is legal
// JavaScript and must be a TypeError rather than a jump through a bad pointer.
inline dom::Node* NodeOf(const js::Value& value) {
  if (!value.IsObject()) {
    return nullptr;
  }
  const js::Value* slot = value.object->GetOwn(kNodeSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(slot->number));
}

// The bindings instance a native belongs to. Carried on the function object
// rather than captured, because a capture is invisible to the collector and a
// raw pointer in one is a lifetime nobody is tracking.
inline DomBindings* OwnerOf(const js::NativeCall& call) {
  const js::Value* slot = call.callee == nullptr ? nullptr : call.callee->GetOwn(kOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<DomBindings*>(static_cast<std::uintptr_t>(slot->number));
}

// A rectangle as a page reads one: the eight members of a `DOMRect`, where
// `x`/`y`/`width`/`height` and `top`/`right`/`bottom`/`left` are the same four
// numbers under two names.
//
// One implementation rather than one per caller, which is not tidiness: a page
// that gets one set of names and not the other silently computes zero, and
// `getBoundingClientRect`, an intersection record and a resize record all have
// to answer with the same object shape. Defined in GeometryBindings.cpp.
js::Value MakeDomRect(js::Interpreter& interpreter, const GeometryRect& rect);

// The one ASCII lower-caser, in util. This name stays because it is used at
// forty call sites in this module and `util::` on each of them buys nothing.
inline std::string LowerCase(std::string_view text) { return util::AsciiLowerCase(text); }

}  // namespace microbrowser::bindings

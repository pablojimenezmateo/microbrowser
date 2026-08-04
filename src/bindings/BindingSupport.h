#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "js/Value.h"

// Shared by the binding translation units, and private to the module: a
// binding is an implementation detail of the seam, not part of its interface,
// so this header is deliberately absent from MODULE.deps' `public:` list.

namespace microbrowser::bindings {

// Where a wrapper keeps the node it stands for, and where a native keeps the
// bindings instance it belongs to. `#` names, the same convention private
// class fields and the engine's other internal slots already use.
inline constexpr const char* kNodeSlot = "#node";
inline constexpr const char* kOwnerSlot = "#bindings";

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

inline std::string LowerCase(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

}  // namespace microbrowser::bindings

#pragma once

#include <cstddef>
#include <vector>

#include "js/Value.h"

// Shared by the builtin translation units. Private to the module: a builtin is
// an implementation detail of the interpreter, not part of its interface, so
// this header is deliberately absent from MODULE.deps' `public:` list.

namespace microbrowser::js {

// A native function's argument, or undefined when the caller passed fewer than
// it reads. JavaScript has no arity check, so every builtin needs this and none
// of them should be indexing the vector directly.
inline Value Argument(const std::vector<Value>& arguments, std::size_t index) {
  return index < arguments.size() ? arguments[index] : Value::Undefined();
}

// The ceiling on anything a script can size by handing over a number:
// `a.length = n`, `a[n] = x`, `s.repeat(n)`, `s.padStart(n)`.
//
// Every one of those is a multiplication on attacker-controlled input, and
// without a bound `"x".repeat(1e9)` is a gigabyte and `a.length = 4294967295`
// is thirty-four. The limit is far past any real page and the failure is a
// RangeError, which is what a script can catch -- an allocation failure is not.
//
// One number for elements and bytes alike. They are not the same cost, but a
// second constant would be a second thing to get wrong, and this one is chosen
// to be safe read either way.
inline constexpr std::size_t kMaxAllocationLength = 1u << 26;

}  // namespace microbrowser::js

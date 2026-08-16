#pragma once

#include <cstddef>
#include <vector>

#include "js/Heap.h"
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

// The object a native constructor should fill in, or null when it should
// allocate one of its own.
//
// `new Map()` and `class Sorted extends Map { }` reach the same native, and
// they need different things from it. The first wants a fresh object; the
// second has one already -- allocated by Construct with the *derived* class's
// prototype on it -- and needs this constructor to initialize that one, because
// a second object allocated here is one nobody would ever see. Getting this
// wrong is what made `class E extends Error` produce an error with no message.
//
// The test is that the receiver is an instance of this constructor: reached
// through the prototype chain, which is true for a direct `new` and for every
// depth of subclass, and false for `Map.call(somethingElse)`.
inline Object* ConstructionTarget(NativeCall& call) {
  if (!call.self.IsObject() || call.callee == nullptr) {
    return nullptr;
  }
  const Value* prototype = call.callee->GetOwn("prototype");
  if (prototype == nullptr || !prototype->IsObject()) {
    return nullptr;
  }
  // Bounded, like every prototype walk here: the chain can be a cycle a page
  // built.
  const Object* walk = call.self.object->Prototype();
  for (int depth = 0; walk != nullptr && depth < 1000; ++depth) {
    if (walk == prototype->object) {
      return call.self.object;
    }
    walk = walk->Prototype();
  }
  return nullptr;
}

// The property a native constructor is marked with to say what *kind* of
// object `new` on it produces.
//
// Read by Construct, which walks a class's superclass chain to its root before
// allocating: `class E extends Error` has to allocate an Error rather than a
// plain object, because the kind is fixed at allocation and is what makes
// `String(e)` say "E: boom".
inline constexpr const char* kConstructsKey = "#constructs";
inline void MarksConstructedKind(Object* constructor, Object::Kind kind) {
  if (constructor != nullptr) {
    constructor->Set(kConstructsKey, Value::Number(static_cast<double>(kind)));
  }
}

// A function's `length` is own, non-enumerable, non-writable. Web IDL operations
// with only optional arguments have length 0, and a missing length is `undefined`
// rather than 0, which idlharness treats as a different failure.
inline void SetNativeLength(Object* function, double length) {
  if (function == nullptr) {
    return;
  }
  Object::Property property;
  property.value = Value::Number(length);
  property.writable = false;
  property.enumerable = false;
  function->Define("length", std::move(property));
}

}  // namespace microbrowser::js

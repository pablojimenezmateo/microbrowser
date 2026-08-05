#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "js/Value.h"

namespace microbrowser::js {

class Interpreter;

// The structured clone algorithm, as *bytes*.
//
// ADR 0026 §1 is what makes this necessary rather than convenient: a history
// entry outlives the document that created it, and `history.state` is a value a
// page handed over. Holding it as a live `js::Value` would keep a dead
// document's heap alive and hand a later document a reference into it -- the
// same value-not-pointer rule ADR 0015 applies to geometry, for a stronger
// reason. Bytes have no owner.
//
// It is also what ADR 0021's storage and ADR 0022's workers need, which is why
// this is a serializer rather than a deep copy: `sessionStorage`, a worker
// message and a history entry are three places a value has to survive without
// the heap it came from.
//
// **What it refuses, and why refusing is the whole design.** A function, a
// symbol, a proxy and anything holding one are not clonable, and the answer is
// `nullopt` rather than a partial result -- which the caller turns into a
// `DataCloneError`. A serializer that silently dropped a function would hand a
// page back an object that is *nearly* the one it stored, and the bug surfaces
// a navigation later in code that has no idea a clone happened.
//
// Cycles are preserved. A value that appears twice deserializes to *one*
// object, because the algorithm is defined that way and because a page that
// stores a graph and reads back a tree has a bug it cannot see.
struct SerializedValue {
  std::vector<std::uint8_t> bytes;

  bool Empty() const { return bytes.empty(); }
  friend bool operator==(const SerializedValue&, const SerializedValue&) = default;
};

// Nothing when `value` is not clonable. `interpreter` is needed because reading
// a Map's entries or a Date's instant goes through the object model.
std::optional<SerializedValue> StructuredSerialize(Interpreter& interpreter, const Value& value);

// The value those bytes describe, rebuilt in `interpreter`'s heap. `undefined`
// for bytes this build cannot read, which is the only safe answer: the bytes may
// have been written by a different build of this browser, and a partially
// rebuilt object is worse than an absent one.
//
// A Map, a Set, a Date and a RegExp are rebuilt by *calling the page's own
// constructor*, not by assembling their internals. A Map's index lives beside
// the heap rather than on the object, and a Map assembled without one answers
// `get` with undefined for a key it contains.
Value StructuredDeserialize(Interpreter& interpreter, const SerializedValue& serialized);

}  // namespace microbrowser::js

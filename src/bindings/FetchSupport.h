#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/BindingSupport.h"
#include "js/Interpreter.h"

// Shared by the two `fetch` translation units and private to the module, for
// the reason BindingSupport.h is: a binding is an implementation detail of the
// seam rather than part of its interface.
//
// The split is by *kind* rather than by size: FetchTypes.cpp is the object
// model a page holds -- `Headers`, `Response` and the body methods -- and
// FetchBindings.cpp is the act of fetching, which is the pending table, the
// delivery and the abort. The header exists because both halves read the same
// slots, and a second spelling of `"#headers"` is a bug that only appears when
// somebody changes one of them.

namespace microbrowser::bindings {

// Where a Headers instance keeps its fields: an array of two-element arrays, in
// insertion order, because order is observable through `forEach` and iteration.
// The same shape `URLSearchParams` uses, for the same reason -- the collector
// can see a property and cannot see a `js::Value` in a C++ field.
inline constexpr const char* kHeaderPairsSlot = "#headers";
// A Response's body and whether it has been read.
inline constexpr const char* kBodySlot = "#body";
inline constexpr const char* kBodyUsedSlot = "#bodyUsed";
// A fetch in flight: the id `NetworkSource` gave it, the promise it will
// settle, and the signal that may cancel it.
inline constexpr const char* kPendingFetchSlot = "#fetches";
inline constexpr const char* kFetchPromiseSlot = "#promise";
inline constexpr const char* kFetchSignalSlot = "#signal";
inline constexpr const char* kFetchIdSlot = "#id";
// AbortSignal's state. `aborted` is what a page reads; the reason is what a
// rejected fetch rejects with.
inline constexpr const char* kAbortedSlot = "#aborted";
inline constexpr const char* kAbortReasonSlot = "#reason";

inline std::vector<js::Value> ReadPairs(const js::Value& holder, const char* slot) {
  std::vector<js::Value> out;
  if (!holder.IsObject()) {
    return out;
  }
  const js::Value* pairs = holder.object->GetOwn(slot);
  if (pairs == nullptr || !pairs->IsObject()) {
    return out;
  }
  out.reserve(pairs->object->ElementCount());
  for (std::size_t i = 0; i < pairs->object->ElementCount(); ++i) {
    out.push_back(pairs->object->GetElement(i));
  }
  return out;
}

inline std::string PairPart(const js::Value& pair, std::size_t index) {
  if (!pair.IsObject() || pair.object->ElementCount() <= index) {
    return {};
  }
  return js::ToString(pair.object->GetElement(index));
}

inline void WritePairs(js::Interpreter& interpreter, const js::Value& holder, const char* slot,
                       std::vector<js::Value> pairs) {
  if (holder.IsObject()) {
    holder.object->SetHidden(slot, interpreter.NewArrayValue(std::move(pairs)));
  }
}

inline js::Value MakePair(js::Interpreter& interpreter, std::string name, std::string value) {
  return interpreter.NewArrayValue(
      {js::Value::String(std::move(name)), js::Value::String(std::move(value))});
}

// Whether `value` is one of ours, by the slot it carries. A page can call any
// method on anything -- `Response.prototype.text.call(7)` is legal JavaScript
// and must be a TypeError rather than a jump through a bad pointer.
inline bool HasSlot(const js::Value& value, const char* slot) {
  return value.IsObject() && value.object->GetOwn(slot) != nullptr;
}

// The headers a page may not set on a request.
//
// A *second* line of defence and knowingly so: `net::IsHeaderOwnedByFetch`
// drops these on the way to the wire and could not be talked out of it. This
// one exists because the specification puts it here, so a page that reads back
// what it set sees what every other browser shows it -- and because a header
// dropped silently at the socket is a header the page believes it sent.
inline bool IsForbiddenHeaderName(std::string_view folded) {
  static constexpr std::string_view kForbidden[] = {
      "accept-charset",
      "accept-encoding",
      "access-control-request-headers",
      "access-control-request-method",
      "connection",
      "content-length",
      "cookie",
      "cookie2",
      "date",
      "dnt",
      "expect",
      "host",
      "keep-alive",
      "origin",
      "referer",
      "te",
      "trailer",
      "transfer-encoding",
      "upgrade",
      "via"};
  return std::find(std::begin(kForbidden), std::end(kForbidden), folded) != std::end(kForbidden);
}

}  // namespace microbrowser::bindings

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "bindings/BindingSupport.h"
#include "dom/Node.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Value.h"

// **A Range that tracks the tree**, and the four rules that keep it doing so.
//
// Split out of BindingSupport.h when that file passed the module's line cap,
// which is the cap working as intended: this is a subject of its own rather
// than more shared plumbing. Everything a live range needs is here -- where its
// boundary points live, the registry the mutation primitives consult, the four
// fixups the DOM specifies, and `splitText`, which is the one tree operation
// defined *in terms of* those fixups rather than merely subject to them.
//
// Private to the module, like BindingSupport.h and for the same reason: a
// binding is an implementation detail of the seam, not part of its interface.

namespace microbrowser::bindings {

// A live range's two boundary points. Hidden properties on the Range object
// rather than C++ fields, for the reason every other wrapper's state is: the
// collector can see a property and cannot see a `js::Value` in a field.
// Shared between Ranges.cpp (the points and the comparisons) and
// RangeContents.cpp (the tree surgery), which is the whole reason they are here
// -- two files agreeing on where a boundary lives by writing the same string
// twice is a bug waiting for a typo.
inline constexpr const char* kStartNodeSlot = "#rangeStartNode";
inline constexpr const char* kStartOffsetSlot = "#rangeStartOffset";
inline constexpr const char* kEndNodeSlot = "#rangeEndNode";
inline constexpr const char* kEndOffsetSlot = "#rangeEndOffset";

inline dom::Node* NodeSlot(const js::Value& range, const char* slot) {
  if (!range.IsObject()) {
    return nullptr;
  }
  const js::Value* found = range.object->GetOwn(slot);
  if (found == nullptr || !found->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(found->number));
}

inline std::uint32_t OffsetSlot(const js::Value& range, const char* slot) {
  if (!range.IsObject()) {
    return 0;
  }
  const js::Value* found = range.object->GetOwn(slot);
  return found == nullptr || !found->IsNumber() ? 0
                                                : static_cast<std::uint32_t>(found->number);
}

// ---------------------------------------------------------------------------
// **Live ranges.**
//
// A Range is not a snapshot. The DOM requires that mutating the tree under one
// moves its boundary points with it: inserting a node before the start bumps
// the start offset, removing the node a boundary is inside collapses that
// boundary onto where the node used to be, and splicing characters out of a
// text node drags every offset past the splice. Without this a page that
// selects a word and then edits the paragraph gets a range describing text
// that has moved -- which reads as a rendering bug three operations later.
//
// The list is a JavaScript array hung off the global, for the reason every
// other cross-call table in this module is: the collector can see a property
// and cannot see a `js::Value` in a C++ field.
//
// **These are free functions rather than methods** because the four places
// that must call them -- `InsertNodeBefore`, `DetachFromTree`, `ClearChildren`
// and CharacterData's `replace data` -- are spread across three translation
// units, and `DomBindings.h` is at its line cap. Each of those call sites is
// already inside a member function, so nothing here needs private access.
//
// Known cost, and it is deliberate rather than overlooked: the list holds its
// ranges **strongly**, so a page that calls `document.createRange()` in a loop
// grows it without bound. The heap's weak table is keyed by object and has no
// enumeration, so a weak set is not expressible here today; the same shape and
// the same trade-off is already what `unattached_` carries. A weakly-held
// registry is the fix and it wants a heap API that does not exist yet.
inline constexpr const char* kLiveRangeListSlot = "#liveRanges";

inline js::Value LiveRangeList(js::Interpreter& interpreter, bool create) {
  js::Object* global = interpreter.Global();
  if (global == nullptr) {
    return js::Value::Undefined();
  }
  if (const js::Value* found = global->GetOwn(kLiveRangeListSlot);
      found != nullptr && found->IsObject()) {
    return *found;
  }
  if (!create) {
    return js::Value::Undefined();
  }
  const js::Value list = interpreter.NewArrayValue({});
  if (list.IsObject()) {
    global->Set(kLiveRangeListSlot, list);
  }
  return list;
}

// **The list is a bounded ring, and that bound is a deviation worth naming.**
//
// A live range has to be found again from the mutation side, so C++ must hold
// a reference to it -- and every reference this module can hold is a *strong*
// one, because the heap's weak table is keyed by object and offers no way to
// enumerate what is still alive. An unbounded strong list is two bugs: a page
// that calls `createRange()` in a loop never gets that memory back, and every
// tree mutation then walks the whole list, which turned a 45-second WPT run
// into 75 seconds and two fresh timeouts the first time it was measured.
//
// So the ring holds the most recent `kMaxLiveRanges` and the oldest entry is
// dropped rather than kept. A dropped range still *works* -- every method on
// it answers from its own slots -- it simply stops being updated when the tree
// moves under it. That is a real deviation, and it is bounded, visible here,
// and preferable to the alternatives: no live ranges at all, or a leak a page
// can drive. The fix is a weak set, which wants a heap API that does not exist.
inline constexpr std::size_t kMaxLiveRanges = 512;
inline constexpr const char* kLiveRangeCursorSlot = "#liveRangeCursor";

inline void RegisterLiveRange(js::Interpreter& interpreter, const js::Value& range) {
  const js::Value list = LiveRangeList(interpreter, true);
  if (!list.IsObject() || !range.IsObject()) {
    return;
  }
  if (list.object->ElementCount() < kMaxLiveRanges) {
    list.object->PushElement(range);
    return;
  }
  const js::Value* cursor = list.object->GetOwn(kLiveRangeCursorSlot);
  const std::size_t at =
      cursor != nullptr && cursor->IsNumber() ? static_cast<std::size_t>(cursor->number) : 0;
  list.object->SetElement(at % kMaxLiveRanges, range);
  list.object->Set(kLiveRangeCursorSlot,
                   js::Value::Number(static_cast<double>((at + 1) % kMaxLiveRanges)));
}

// Run `visit` over every live range, as (range, is_start). Both boundaries of
// each range get the same treatment in every one of the rules below, so they
// are visited together rather than in two passes -- a rule applied to the start
// and forgotten for the end is the classic bug in this area.
template <typename Visit>
inline void ForEachBoundary(js::Interpreter& interpreter, Visit&& visit) {
  const js::Value list = LiveRangeList(interpreter, false);
  if (!list.IsObject()) {
    return;
  }
  const std::size_t count = list.object->ElementCount();
  for (std::size_t i = 0; i < count; ++i) {
    const js::Value entry = list.object->GetElement(i);
    if (!entry.IsObject()) {
      continue;
    }
    visit(entry, true);
    visit(entry, false);
  }
}

inline void SetBoundary(const js::Value& range, bool start, dom::Node* node, std::size_t offset) {
  range.object->SetHidden(start ? kStartNodeSlot : kEndNodeSlot, PointerValue(node));
  range.object->SetHidden(start ? kStartOffsetSlot : kEndOffsetSlot,
                          js::Value::Number(static_cast<double>(offset)));
}

// True when anything is tracking the tree at all. Every hook below starts with
// this, *before* it computes an index or walks to a root: a page with no live
// range must pay one pointer comparison per mutation and nothing more, which is
// the same rule `StyleInvalidation` follows for a page with no `:hover`.
inline bool AnyLiveRanges(js::Interpreter& interpreter) {
  const js::Value list = LiveRangeList(interpreter, false);
  return list.IsObject() && list.object->ElementCount() != 0;
}

// "For each live range whose start node is parent and start offset is greater
// than index, increase its start offset by count." Called *after* the
// insertion, with the index the node landed at.
inline void RangesDidInsert(js::Interpreter& interpreter, const dom::Node& parent,
                            std::size_t index, std::size_t count) {
  if (!AnyLiveRanges(interpreter)) {
    return;
  }
  ForEachBoundary(interpreter, [&](const js::Value& range, bool start) {
    if (NodeSlot(range, start ? kStartNodeSlot : kEndNodeSlot) != &parent) {
      return;
    }
    const std::size_t offset = OffsetSlot(range, start ? kStartOffsetSlot : kEndOffsetSlot);
    if (offset > index) {
      SetBoundary(range, start, const_cast<dom::Node*>(&parent), offset + count);
    }
  });
}

// Called *before* the removal, while the node still knows where it lives. A
// boundary inside the departing subtree collapses onto the gap it leaves; a
// boundary in the parent past that gap shifts back by one.
inline void RangesWillRemove(js::Interpreter& interpreter, const dom::Node& node) {
  if (!AnyLiveRanges(interpreter)) {
    return;
  }
  const dom::Node* parent = node.Parent();
  if (parent == nullptr) {
    return;
  }
  const std::size_t index = IndexIn(node);
  ForEachBoundary(interpreter, [&](const js::Value& range, bool start) {
    dom::Node* at = NodeSlot(range, start ? kStartNodeSlot : kEndNodeSlot);
    if (at == nullptr) {
      return;
    }
    for (const dom::Node* walk = at; walk != nullptr; walk = walk->Parent()) {
      if (walk == &node) {
        SetBoundary(range, start, const_cast<dom::Node*>(parent), index);
        return;
      }
    }
    if (at != parent) {
      return;
    }
    const std::size_t offset = OffsetSlot(range, start ? kStartOffsetSlot : kEndOffsetSlot);
    if (offset > index) {
      SetBoundary(range, start, at, offset - 1);
    }
  });
}

// "Replace data" with (offset, count) replaced by `added` code units. A
// boundary inside the replaced span clamps to its start; one past the span
// shifts by the difference. The two rules are disjoint on the *original*
// offset, which is why they can be applied in one pass.
inline void RangesDidReplaceData(js::Interpreter& interpreter, const dom::Node& node,
                                 std::size_t offset, std::size_t count, std::size_t added) {
  if (!AnyLiveRanges(interpreter)) {
    return;
  }
  ForEachBoundary(interpreter, [&](const js::Value& range, bool start) {
    if (NodeSlot(range, start ? kStartNodeSlot : kEndNodeSlot) != &node) {
      return;
    }
    const std::size_t at = OffsetSlot(range, start ? kStartOffsetSlot : kEndOffsetSlot);
    if (at > offset && at <= offset + count) {
      SetBoundary(range, start, const_cast<dom::Node*>(&node), offset);
    } else if (at > offset + count) {
      SetBoundary(range, start, const_cast<dom::Node*>(&node), at + added - count);
    }
  });
}

// The boundary half of `splitText`: a boundary past the split point moves into
// the tail node, and one sitting just after the original node in its parent
// steps over the newcomer.
inline void RangesDidSplitText(js::Interpreter& interpreter, const dom::Node& node,
                               const dom::Node& tail, std::size_t offset) {
  if (!AnyLiveRanges(interpreter)) {
    return;
  }
  const dom::Node* parent = node.Parent();
  const std::size_t index = IndexIn(node);
  ForEachBoundary(interpreter, [&](const js::Value& range, bool start) {
    dom::Node* at = NodeSlot(range, start ? kStartNodeSlot : kEndNodeSlot);
    const std::size_t value = OffsetSlot(range, start ? kStartOffsetSlot : kEndOffsetSlot);
    if (at == &node && value > offset) {
      SetBoundary(range, start, const_cast<dom::Node*>(&tail), value - offset);
    } else if (parent != nullptr && at == parent && value == index + 1) {
      SetBoundary(range, start, at, value + 1);
    }
  });
}

// `splitText`, which `Text.splitText` and `Range.insertNode` both need -- the
// specification defines the second in terms of the first, so there is one copy
// of it here rather than one in each caller. Returns the tail node, or null if
// the node has no parent to be split inside.
//
// The order matters and is the specification's: insert the tail, move the
// boundaries that belong to it, *then* truncate the original. Truncating first
// would run the replace-data fixup over boundaries that should have moved to
// the tail and clamp them to the split point instead.
inline dom::Node* SplitTextNode(js::Interpreter& interpreter, dom::Text& node,
                                std::size_t offset) {
  dom::Node* parent = node.Parent();
  if (parent == nullptr) {
    return nullptr;
  }
  const std::string data = node.Data();
  const std::size_t at = std::min(offset, data.size());
  auto tail = std::make_unique<dom::Text>(data.substr(at), node.IsCData());
  dom::Node* tail_raw = tail.get();
  parent->InsertBefore(std::move(tail), NextSiblingOf(node));
  RangesDidSplitText(interpreter, node, *tail_raw, at);
  node.SetData(data.substr(0, at));
  RangesDidReplaceData(interpreter, node, at, data.size() - at, 0);
  return tail_raw;
}

// ---------------------------------------------------------------------------
// Live NodeIterators are **not** here: they are NodeIterators.h.
//
// Two worktrees implemented the DOM's "NodeIterator pre-removing steps" against
// the same base and both landed in one merge. The copy that lived here was the
// narrower of the two -- it asked whether the departing node *was* the
// iterator's root rather than whether it contained it, and it walked back to a
// previous sibling's last descendant by hand where the other has a tree-order
// predecessor. NodeIterators.h is the one that survived, and it owns the slot
// names both files used to spell separately.
//
// The reason this note is here rather than deleted: the registry pattern above
// is what both copies were modelled on, and the next thing that needs to track
// the tree will find this file first.

}  // namespace microbrowser::bindings

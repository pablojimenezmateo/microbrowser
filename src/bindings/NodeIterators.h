#pragma once

#include <cstddef>

#include "bindings/BindingSupport.h"
#include "dom/Node.h"
#include "js/Interpreter.h"

// **A NodeIterator survives the removal of the node it is pointing at**, and
// that is the whole content of this file.
//
// A TreeWalker does not need it: its `currentNode` is whatever a page last set
// or the walk last reached, and a page that removes that node keeps a walker
// pointing at a detached subtree, which is what the specification says. A
// NodeIterator is the opposite -- it holds a *position in a sequence*, and a
// removal has to move that position or the next `nextNode()` resumes inside a
// subtree the document no longer contains.
//
// So there is a registry, with the same shape and the same bound as the live
// ranges next door in LiveRanges.h, and for the same reason: the fixup has to
// find the iterators from the *mutation* side, every reference this module can
// hold is strong, and an unbounded strong list is a leak a page can drive with
// a loop. See kMaxLiveRanges for the full argument -- a dropped iterator still
// works, it simply stops being told the tree moved.
//
// The slot names live here rather than in TreeWalkers.cpp because both files
// need them: one writes the cursor, the other repairs it.

namespace microbrowser::bindings {

// The cursor both a TreeWalker and a NodeIterator are, as slots.
inline constexpr const char* kWalkRootSlot = "#walkRoot";
inline constexpr const char* kWalkCurrentSlot = "#walkCurrent";
inline constexpr const char* kWalkShowSlot = "#walkShow";
inline constexpr const char* kWalkFilterSlot = "#walkFilter";
// `pointerBeforeReferenceNode`: whether the iterator sits *before* its
// reference node or after it. A NodeIterator has no "current" node -- it has a
// gap, and this says which side of the reference the gap is on.
inline constexpr const char* kWalkBeforeSlot = "#walkBefore";

inline constexpr const char* kNodeIteratorListSlot = "#nodeIterators";
inline constexpr const char* kNodeIteratorCursorSlot = "#nodeIteratorCursor";
inline constexpr std::size_t kMaxNodeIterators = 256;

inline js::Value NodeIteratorList(js::Interpreter& interpreter, bool create) {
  js::Object* global = interpreter.Global();
  if (global == nullptr) {
    return js::Value::Undefined();
  }
  if (const js::Value* existing = global->GetOwn(kNodeIteratorListSlot);
      existing != nullptr && existing->IsObject()) {
    return *existing;
  }
  if (!create) {
    return js::Value::Undefined();
  }
  const js::Value list = interpreter.NewArrayValue({});
  if (list.IsObject()) {
    global->Set(kNodeIteratorListSlot, list);
  }
  return list;
}

inline void RegisterNodeIterator(js::Interpreter& interpreter, const js::Value& iterator) {
  const js::Value list = NodeIteratorList(interpreter, true);
  if (!list.IsObject() || !iterator.IsObject()) {
    return;
  }
  if (list.object->ElementCount() < kMaxNodeIterators) {
    list.object->PushElement(iterator);
    return;
  }
  const js::Value* cursor = list.object->GetOwn(kNodeIteratorCursorSlot);
  const std::size_t at =
      cursor != nullptr && cursor->IsNumber() ? static_cast<std::size_t>(cursor->number) : 0;
  list.object->SetElement(at % kMaxNodeIterators, iterator);
  list.object->Set(kNodeIteratorCursorSlot,
                   js::Value::Number(static_cast<double>((at + 1) % kMaxNodeIterators)));
}

// The node behind a pointer slot, or null.
inline dom::Node* WalkNodeSlot(const js::Value& cursor, const char* slot) {
  if (!cursor.IsObject()) {
    return nullptr;
  }
  const js::Value* stored = cursor.object->GetOwn(slot);
  if (stored == nullptr || !stored->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(stored->number));
}

inline bool IsInclusiveAncestorNode(const dom::Node* ancestor, const dom::Node* descendant) {
  for (const dom::Node* walk = descendant; walk != nullptr; walk = walk->Parent()) {
    if (walk == ancestor) {
      return true;
    }
  }
  return false;
}

// The last node before `node` in tree order: the deepest last descendant of its
// previous sibling, or its parent when there is none.
inline dom::Node* NodeBefore(dom::Node& node) {
  dom::Node* previous = nullptr;
  if (dom::Node* parent = node.Parent()) {
    for (const std::unique_ptr<dom::Node>& child : parent->Children()) {
      if (child.get() == &node) {
        break;
      }
      previous = child.get();
    }
    if (previous == nullptr) {
      return parent;
    }
  }
  while (previous != nullptr && !previous->Children().empty()) {
    previous = previous->Children().back().get();
  }
  return previous;
}

// The next node after `node` **and all its descendants** in tree order, or null
// when `node` is the last node or an ancestor of it.
inline dom::Node* NodeAfterSubtree(dom::Node& node) {
  for (dom::Node* walk = &node; walk != nullptr; walk = walk->Parent()) {
    dom::Node* parent = walk->Parent();
    if (parent == nullptr) {
      continue;
    }
    const auto& children = parent->Children();
    for (std::size_t i = 0; i + 1 < children.size(); ++i) {
      if (children[i].get() == walk) {
        return children[i + 1].get();
      }
    }
  }
  return nullptr;
}

// DOM §6.1, "NodeIterator pre-removing steps". Called **before** the removal,
// while the node still knows where it lives -- every one of the three answers
// below is a question about the tree it is about to leave.
inline void NodeIteratorsWillRemove(js::Interpreter& interpreter, dom::Node& node) {
  const js::Value list = NodeIteratorList(interpreter, false);
  // A page with no NodeIterator pays one pointer comparison per removal, which
  // is the rule the live ranges and the style invalidation index both follow.
  if (!list.IsObject() || list.object->ElementCount() == 0) {
    return;
  }
  const std::size_t count = list.object->ElementCount();
  for (std::size_t i = 0; i < count; ++i) {
    const js::Value cursor = list.object->GetElement(i);
    if (!cursor.IsObject()) {
      continue;
    }
    dom::Node* root = WalkNodeSlot(cursor, kWalkRootSlot);
    dom::Node* reference = WalkNodeSlot(cursor, kWalkCurrentSlot);
    if (root == nullptr || reference == nullptr) {
      continue;
    }
    // Removing the iterator's own root -- or anything containing it -- takes
    // the whole traversal out of the document, and the specification leaves the
    // iterator where it is rather than inventing a position outside its root.
    if (IsInclusiveAncestorNode(&node, root)) {
      continue;
    }
    // Nothing to repair unless the reference is inside what is leaving.
    if (!IsInclusiveAncestorNode(&node, reference)) {
      continue;
    }
    const js::Value* before = cursor.object->GetOwn(kWalkBeforeSlot);
    const bool pointer_before = before != nullptr && js::ToBoolean(*before);
    if (pointer_before) {
      // The gap is before the reference, so it survives as the gap *after* the
      // departing subtree -- if there is anything after it.
      if (dom::Node* next = NodeAfterSubtree(node)) {
        cursor.object->SetHidden(kWalkCurrentSlot, PointerValue(next));
        continue;
      }
      // Nothing follows, so the gap becomes the one *after* the last node
      // before the subtree.
      cursor.object->SetHidden(kWalkBeforeSlot, js::Value::Bool(false));
    }
    if (dom::Node* previous = NodeBefore(node)) {
      cursor.object->SetHidden(kWalkCurrentSlot, PointerValue(previous));
    }
  }
}

}  // namespace microbrowser::bindings

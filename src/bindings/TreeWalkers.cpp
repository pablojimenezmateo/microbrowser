// `NodeFilter`, `createTreeWalker` and `createNodeIterator`: walking the tree
// as an object that remembers where it is.
//
// A separate translation unit because DocumentBindings.cpp is what `document`
// itself answers and this is a *cursor over* the document -- and because the
// module's line cap means a missing file rather than a bigger one.
//
// The reason it exists at all is a measurement rather than a milestone.
// youtube.com loads `webcomponents-all-noPatch.js`, and the first thing that
// file does at module scope is
//
//   var M = document.createTreeWalker(document, NodeFilter.SHOW_ALL, null, !1)
//
// so an absent `NodeFilter` ended the whole polyfill on its first line -- and
// with it every custom element youtube's page is built out of.
//
// **The filter is a page's own function and it is called while a walk is in
// progress.** That is the hostile-input surface here, and it is not a parser
// one: a filter can throw, can move `currentNode`, and can remove the node the
// walk is standing on. So every step re-reads the tree through the node
// pointers rather than caching a child list, the traversal is bounded by a
// step count as well as by reaching the root, and a throw out of the filter
// stops the walk and propagates rather than being swallowed into "reject".

#include <cstdint>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// Where a cursor keeps what it is walking. `#node` already means "the node
// this wrapper stands for" and a cursor is not a node, so these are their own
// names rather than a second meaning for that one.
constexpr const char* kRootSlot = "#walkRoot";
constexpr const char* kCurrentSlot = "#walkCurrent";
constexpr const char* kShowSlot = "#walkShow";
constexpr const char* kFilterSlot = "#walkFilter";
// A NodeIterator's `nextNode` and `previousNode` are defined against a
// *reference node* and a "before or after it" flag rather than against a
// current node, which is the whole difference between the two objects: an
// iterator survives its reference node being removed and a walker does not.
constexpr const char* kBeforeSlot = "#walkBefore";

// What `whatToShow` bit each node kind answers to. The numbering is
// `1 << (nodeType - 1)`, which is the specification's and is why SHOW_ELEMENT
// is 1 and SHOW_TEXT is 4 rather than 1 and 2.
std::uint32_t ShowBitFor(const dom::Node& node) {
  switch (node.GetKind()) {
    case dom::Node::Kind::Element:
      return 0x1;
    case dom::Node::Kind::Text:
      return 0x4;
    case dom::Node::Kind::Comment:
      return 0x80;
    case dom::Node::Kind::Document:
      return 0x100;
    case dom::Node::Kind::DocumentType:
      return 0x200;
    case dom::Node::Kind::DocumentFragment:
      return 0x400;
    case dom::Node::Kind::ProcessingInstruction:
      return 0x40;
  }
  return 0;
}

dom::Node* PointerSlot(const Value& self, const char* name) {
  if (!self.IsObject()) {
    return nullptr;
  }
  const Value* slot = self.object->GetOwn(name);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(slot->number));
}

std::uint32_t ShowSlot(const Value& self) {
  if (!self.IsObject()) {
    return 0;
  }
  const Value* slot = self.object->GetOwn(kShowSlot);
  return slot == nullptr || !slot->IsNumber() ? 0
                                              : static_cast<std::uint32_t>(slot->number);
}

// The three answers a filter can give. Named rather than numbered here because
// the difference between Reject and Skip is the one thing about this API that
// is easy to get backwards: **Skip passes over the node and still descends
// into it; Reject passes over its whole subtree** -- and for a NodeIterator
// there is no subtree to reject, so the two mean the same thing there.
enum class Verdict : std::uint8_t { Accept, Reject, Skip, Threw };

// Runs `whatToShow` and then the page's filter, in that order -- which is the
// specification's order and matters, because a filter must not be called for a
// node the mask already excluded.
Verdict Filter(NativeCall& call, dom::Node& node) {
  const std::uint32_t show = ShowSlot(call.self);
  if ((show & ShowBitFor(node)) == 0) {
    return Verdict::Skip;
  }
  const Value* filter = call.self.IsObject() ? call.self.object->GetOwn(kFilterSlot) : nullptr;
  if (filter == nullptr || !filter->IsObject()) {
    return Verdict::Accept;
  }
  DomBindings* owner = OwnerOf(call);
  if (owner == nullptr) {
    return Verdict::Accept;
  }
  // Either a function or an object with `acceptNode`, and both spellings are
  // in use -- the second is what a polyfill written against the original
  // interface passes.
  Value callee = *filter;
  Value receiver = Value::Undefined();
  if (!callee.object->IsCallable()) {
    const js::Value* accept = filter->object->Get("acceptNode");
    if (accept == nullptr || !accept->IsObject() || !accept->object->IsCallable()) {
      return Verdict::Accept;
    }
    receiver = *filter;
    callee = *accept;
  }
  const js::Result answer =
      call.interpreter.CallFunction(callee, receiver, {owner->WrapperFor(&node)});
  if (answer.IsAbrupt()) {
    // Not swallowed into a reject. A filter that threw has not answered, and
    // continuing the walk past it would be inventing one.
    call.ThrowValue(answer.value);
    return Verdict::Threw;
  }
  const double verdict = js::ToNumber(answer.value);
  if (verdict == 1.0) {
    return Verdict::Accept;
  }
  return verdict == 2.0 ? Verdict::Reject : Verdict::Skip;
}

// How many nodes one call may look at before giving up.
//
// A filter is a page's function and it can mutate the tree, so "walk until the
// root" is a bound the page controls: a filter that reparents the node the
// walk just left can keep a traversal moving forever. This is the same shape
// as every other bound here -- a wrong answer, not a hang.
constexpr int kMaxSteps = 1'000'000;

dom::Node* Sibling(dom::Node* node, int step) {
  if (node == nullptr || node->Parent() == nullptr) {
    return nullptr;
  }
  const std::vector<std::unique_ptr<dom::Node>>& siblings = node->Parent()->Children();
  for (std::size_t i = 0; i < siblings.size(); ++i) {
    if (siblings[i].get() != node) {
      continue;
    }
    const auto at = static_cast<std::ptrdiff_t>(i) + step;
    if (at < 0 || at >= static_cast<std::ptrdiff_t>(siblings.size())) {
      return nullptr;
    }
    return siblings[static_cast<std::size_t>(at)].get();
  }
  return nullptr;
}

// The next node in document order, descending into `node` unless told not to.
// Null at the end of `root`'s subtree.
dom::Node* FollowingNode(dom::Node* node, dom::Node* root, bool descend) {
  if (node == nullptr) {
    return nullptr;
  }
  if (descend && node->FirstChild() != nullptr) {
    return node->FirstChild();
  }
  for (dom::Node* at = node; at != nullptr && at != root; at = at->Parent()) {
    if (dom::Node* next = Sibling(at, 1)) {
      return next;
    }
  }
  return nullptr;
}

// The previous node in document order: the deepest last descendant of the
// previous sibling, or the parent.
dom::Node* PrecedingNode(dom::Node* node, dom::Node* root) {
  if (node == nullptr || node == root) {
    return nullptr;
  }
  dom::Node* previous = Sibling(node, -1);
  if (previous == nullptr) {
    return node->Parent() == root ? nullptr : node->Parent();
  }
  for (int steps = 0; previous->LastChild() != nullptr && steps < kMaxSteps; ++steps) {
    previous = previous->LastChild();
  }
  return previous;
}

void SetCurrent(const Value& self, dom::Node* node) {
  if (self.IsObject()) {
    self.object->SetHidden(kCurrentSlot, PointerValue(node));
  }
}

}  // namespace

void DomBindings::InstallTreeWalkers(const js::Value& document) {
  // --- NodeFilter -----------------------------------------------------------
  // Constants and nothing else, which is what the interface is: the three
  // verdicts and the mask bits. A page reads `NodeFilter.SHOW_ELEMENT` far
  // more often than it implements the interface.
  const Value node_filter = interpreter_->NewObjectValue();
  if (node_filter.IsObject()) {
    node_filter.object->Set("FILTER_ACCEPT", Value::Number(1));
    node_filter.object->Set("FILTER_REJECT", Value::Number(2));
    node_filter.object->Set("FILTER_SKIP", Value::Number(3));
    node_filter.object->Set("SHOW_ALL", Value::Number(4294967295.0));
    node_filter.object->Set("SHOW_ELEMENT", Value::Number(0x1));
    node_filter.object->Set("SHOW_ATTRIBUTE", Value::Number(0x2));
    node_filter.object->Set("SHOW_TEXT", Value::Number(0x4));
    node_filter.object->Set("SHOW_CDATA_SECTION", Value::Number(0x8));
    node_filter.object->Set("SHOW_PROCESSING_INSTRUCTION", Value::Number(0x40));
    node_filter.object->Set("SHOW_COMMENT", Value::Number(0x80));
    node_filter.object->Set("SHOW_DOCUMENT", Value::Number(0x100));
    node_filter.object->Set("SHOW_DOCUMENT_TYPE", Value::Number(0x200));
    node_filter.object->Set("SHOW_DOCUMENT_FRAGMENT", Value::Number(0x400));
    node_filter.object->Set("SHOW_NOTATION", Value::Number(0x800));
    interpreter_->GlobalScope()->Declare("NodeFilter", node_filter, false);
  }

  const auto method = [this](const Value& target, const char* name,
                             js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject() && target.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };

  // --- The cursor both objects are -------------------------------------------
  //
  // One builder, because a TreeWalker and a NodeIterator differ in three
  // methods and not in what they hold. `whatToShow` defaults to SHOW_ALL when
  // it is absent, which is the specification's default and is what
  // `createTreeWalker(root)` relies on.
  const auto make_cursor = [this, method](NativeCall& call, bool iterator) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* root = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || root == nullptr) {
      return call.Throw("TypeError", "createTreeWalker needs a root node");
    }
    const Value cursor = call.interpreter.NewObjectValue();
    if (!cursor.IsObject()) {
      return Value::Undefined();
    }
    const Value show = Argument(call.arguments, 1);
    // ToUint32 rather than a cast: `whatToShow` is an `unsigned long` in the
    // interface, and -1 is how a page spells SHOW_ALL when it does not want to
    // type the constant.
    const double mask = show.IsUndefined() ? 4294967295.0
                                           : static_cast<double>(js::ToUint32(js::ToNumber(show)));
    cursor.object->SetHidden(kRootSlot, PointerValue(root));
    cursor.object->SetHidden(kCurrentSlot, PointerValue(root));
    cursor.object->SetHidden(kShowSlot, Value::Number(mask));
    cursor.object->SetHidden(kFilterSlot, Argument(call.arguments, 2));
    cursor.object->SetHidden(kBeforeSlot, Value::Bool(true));
    cursor.object->Set("root", owner->WrapperFor(root));
    cursor.object->Set("whatToShow", Value::Number(mask));
    cursor.object->Set("filter", Argument(call.arguments, 2));

    // `currentNode` is readable *and writable* on a TreeWalker -- a page
    // positions the walk by assigning to it, and every implementation of
    // `getComposedRanges`-style code does. The accessor pair is over the
    // hidden slot, so the two can never disagree.
    const Value get_current = call.interpreter.NewNativeValue("currentNode", [](NativeCall& inner) {
      DomBindings* bindings = OwnerOf(inner);
      dom::Node* at = PointerSlot(inner.self, kCurrentSlot);
      return bindings == nullptr ? Value::Null() : bindings->WrapperFor(at);
    });
    const Value set_current = call.interpreter.NewNativeValue("currentNode", [](NativeCall& inner) {
      if (dom::Node* node = NodeOf(Argument(inner.arguments, 0))) {
        SetCurrent(inner.self, node);
      }
      return Value::Undefined();
    });
    if (get_current.IsObject() && set_current.IsObject()) {
      get_current.object->Set(kOwnerSlot, PointerValue(this));
      set_current.object->Set(kOwnerSlot, PointerValue(this));
      cursor.object->DefineAccessor(iterator ? "referenceNode" : "currentNode",
                                    get_current.object,
                                    iterator ? nullptr : set_current.object);
    }

    // --- nextNode / previousNode ---------------------------------------------
    //
    // The one method both objects have and the one place they genuinely
    // differ. A walker descends from `currentNode` and a rejected node takes
    // its subtree with it; an iterator is a flat document-order sequence over
    // the subtree, where reject and skip mean the same thing because there is
    // nothing to descend into.
    const auto step = [iterator](NativeCall& inner, int direction) {
      DomBindings* bindings = OwnerOf(inner);
      dom::Node* limit = PointerSlot(inner.self, kRootSlot);
      if (bindings == nullptr || limit == nullptr) {
        return Value::Null();
      }
      dom::Node* at = PointerSlot(inner.self, kCurrentSlot);
      if (at == nullptr || !IsInclusiveDescendant(at, limit)) {
        // The current node was removed from under the walk. Answering null is
        // the honest end rather than resuming from a node that is no longer in
        // the tree -- which would be a walk into freed memory the moment the
        // detached subtree went away.
        return Value::Null();
      }
      // **A NodeIterator is a cursor *between* nodes, not on one.** That is
      // what `pointerBeforeReferenceNode` records, and getting it wrong is not
      // cosmetic: it decides whether the next `nextNode()` re-reports the
      // reference node or steps past it, so an iterator driven forwards and
      // then backwards used to skip one node at the turn. 597 subtests of
      // `dom/traversal/NodeIterator.html` were this one flag.
      //
      // The rule is symmetric and the old code only had half of it: moving
      // forward leaves the pointer *after* the node it reports, moving backward
      // leaves it *before*. When the pointer is already on the far side of the
      // reference node, the move consumes the flag instead of the node -- which
      // is why a direction change reports the same node twice, once from each
      // side, exactly as the specification says.
      if (iterator) {
        bool before = true;
        if (const Value* slot = inner.self.IsObject() ? inner.self.object->GetOwn(kBeforeSlot)
                                                      : nullptr) {
          before = js::ToBoolean(*slot);
        }
        for (int steps = 0; steps < kMaxSteps; ++steps) {
          if (direction > 0 ? before : !before) {
            // The flag is on the side we are heading, so flip it and stay.
            before = direction < 0;
          } else {
            bool descend = true;
            dom::Node* next = direction > 0 ? FollowingNode(at, limit, descend)
                                            : PrecedingNode(at, limit);
            if (next == nullptr) {
              return Value::Null();
            }
            at = next;
          }
          const Verdict verdict = Filter(inner, *at);
          if (verdict == Verdict::Threw) {
            return Value::Undefined();
          }
          if (verdict != Verdict::Accept) {
            continue;
          }
          SetCurrent(inner.self, at);
          if (inner.self.IsObject()) {
            inner.self.object->SetHidden(kBeforeSlot, Value::Bool(before));
          }
          return bindings->WrapperFor(at);
        }
        return Value::Null();
      }
      bool descend = true;
      for (int steps = 0; steps < kMaxSteps; ++steps) {
        dom::Node* next = direction > 0 ? FollowingNode(at, limit, descend)
                                        : PrecedingNode(at, limit);
        if (next == nullptr) {
          return Value::Null();
        }
        at = next;
        const Verdict verdict = Filter(inner, *at);
        if (verdict == Verdict::Threw) {
          return Value::Undefined();
        }
        if (verdict == Verdict::Accept) {
          SetCurrent(inner.self, at);
          return bindings->WrapperFor(at);
        }
        // Reject skips the subtree; skip does not.
        descend = verdict != Verdict::Reject;
      }
      return Value::Null();
    };
    method(cursor, "nextNode", [step](NativeCall& inner) { return step(inner, 1); });
    method(cursor, "previousNode", [step](NativeCall& inner) { return step(inner, -1); });
    method(cursor, "detach", [](NativeCall&) {
      // A no-op, and that is the whole of it in the current specification:
      // there is nothing to release, and it is kept because old code calls it.
      return Value::Undefined();
    });
    if (iterator) {
      // `pointerBeforeReferenceNode`, which is the state that makes an
      // iterator's first `nextNode` answer with the root.
      const Value before = call.interpreter.NewNativeValue(
          "pointerBeforeReferenceNode", [](NativeCall& inner) {
            const Value* slot =
                inner.self.IsObject() ? inner.self.object->GetOwn(kBeforeSlot) : nullptr;
            return Value::Bool(slot != nullptr && js::ToBoolean(*slot));
          });
      if (before.IsObject()) {
        cursor.object->DefineAccessor("pointerBeforeReferenceNode", before.object, nullptr);
      }
      return cursor;
    }

    // --- The five relative moves, which only a TreeWalker has ----------------
    //
    // Each one moves `currentNode` if it finds an accepted node and leaves it
    // alone if it does not, which is the property a `while (walker.nextSibling())`
    // loop is written against.
    const auto relative = [](NativeCall& inner, int direction, bool children) {
      DomBindings* bindings = OwnerOf(inner);
      dom::Node* limit = PointerSlot(inner.self, kRootSlot);
      dom::Node* at = PointerSlot(inner.self, kCurrentSlot);
      if (bindings == nullptr || limit == nullptr || at == nullptr ||
          !IsInclusiveDescendant(at, limit)) {
        return Value::Null();
      }
      // "Traverse siblings" begins "if node is root, then return null", and it
      // is the first step for a reason: the root's *actual* siblings are
      // outside the walker's subtree entirely. Without it a walker rooted at
      // `document.body` answered `nextSibling()` with whatever followed `body`
      // in the document -- a node the walker was created not to see. The child
      // forms have no such rule, because descending from the root is the one
      // move that cannot leave it.
      if (!children && at == limit) {
        return Value::Null();
      }
      dom::Node* candidate =
          children ? (direction > 0 ? at->FirstChild() : at->LastChild()) : Sibling(at, direction);
      for (int steps = 0; candidate != nullptr && steps < kMaxSteps; ++steps) {
        const Verdict verdict = Filter(inner, *candidate);
        if (verdict == Verdict::Threw) {
          return Value::Undefined();
        }
        if (verdict == Verdict::Accept) {
          SetCurrent(inner.self, candidate);
          return bindings->WrapperFor(candidate);
        }
        // A skipped node is passed over but still descended into: its own
        // children are candidates in its place. A rejected one is not.
        if (verdict == Verdict::Skip && candidate->FirstChild() != nullptr) {
          candidate = direction > 0 ? candidate->FirstChild() : candidate->LastChild();
          continue;
        }
        dom::Node* next = Sibling(candidate, direction);
        while (next == nullptr && candidate != nullptr && candidate->Parent() != at &&
               candidate->Parent() != limit && candidate->Parent() != nullptr) {
          candidate = candidate->Parent();
          next = Sibling(candidate, direction);
        }
        candidate = next;
      }
      return Value::Null();
    };
    method(cursor, "firstChild",
           [relative](NativeCall& inner) { return relative(inner, 1, true); });
    method(cursor, "lastChild",
           [relative](NativeCall& inner) { return relative(inner, -1, true); });
    method(cursor, "nextSibling",
           [relative](NativeCall& inner) { return relative(inner, 1, false); });
    method(cursor, "previousSibling",
           [relative](NativeCall& inner) { return relative(inner, -1, false); });
    // `parentNode` walks *up* and stops at the root, which is the one move
    // that cannot leave the root's subtree by construction.
    method(cursor, "parentNode", [](NativeCall& inner) {
      DomBindings* bindings = OwnerOf(inner);
      dom::Node* limit = PointerSlot(inner.self, kRootSlot);
      dom::Node* at = PointerSlot(inner.self, kCurrentSlot);
      if (bindings == nullptr || limit == nullptr || at == nullptr) {
        return Value::Null();
      }
      for (int steps = 0; at != nullptr && at != limit && steps < kMaxSteps; ++steps) {
        at = at->Parent();
        if (at == nullptr) {
          return Value::Null();
        }
        const Verdict verdict = Filter(inner, *at);
        if (verdict == Verdict::Threw) {
          return Value::Undefined();
        }
        if (verdict == Verdict::Accept) {
          SetCurrent(inner.self, at);
          return bindings->WrapperFor(at);
        }
        if (at == limit) {
          return Value::Null();
        }
      }
      return Value::Null();
    });
    return cursor;
  };

  method(document, "createTreeWalker",
         [make_cursor](NativeCall& call) { return make_cursor(call, false); });
  method(document, "createNodeIterator",
         [make_cursor](NativeCall& call) { return make_cursor(call, true); });
}

}  // namespace microbrowser::bindings

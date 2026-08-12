// `Range`, the other half: the five methods that **change the tree**.
//
// `insertNode`, `deleteContents`, `extractContents`, `cloneContents` and
// `surroundContents`. Ranges.cpp is boundary points and the comparisons
// between them; everything here reparents nodes, and the split is the module's
// line cap pointing at a real seam rather than at a big file.
//
// ADR 0012 listed these as deliberately absent, and the reason it gave was a
// good one: "a version that got the common case right and the partial-
// containment case wrong would corrupt a page's DOM silently". That reason is
// what shapes this file. **Partial containment is not a corner case, it is the
// algorithm** -- a range that starts inside one paragraph and ends inside
// another has, at the top, two children that are *half* inside it, and the
// specification's answer for each is "clone the node shallowly, then run this
// same procedure over the part of it that is inside". So the extract below is
// recursive, and the recursion is the feature.
//
// The three content methods are one function with a mode, for the same reason
// there is one ordering function next door. `cloneContents` differs from
// `extractContents` only in whether the original is left behind, and
// `deleteContents` is the same walk building nothing -- three separate
// implementations would be three chances to disagree about which node a
// half-contained subtree hangs from.
//
// **Everything here is a lambda inside one member function**, which is not a
// style choice: `DomBindings.h` is at its translation-unit cap, so this file
// gets exactly one declaration. A lambda written inside a member function has
// that member's access to private state, so `CreateDocumentFragment`,
// `InsertNodeBefore` and `SetCharacterData` are all reachable without widening
// the header by a line.
//
// **Where these are measured, and where they are not.** WPT's five files named
// after these methods -- `Range-insertNode`, `-surroundContents`,
// `-cloneContents`, `-extractContents`, `-deleteContents`, 4,176 subtests --
// test nothing here. Every one of them drives two `<iframe>`s and is started by
// `iframe.onload`, so without nested browsing contexts (ADR 0027) their
// `async_test`s never begin and the file reports a harness TIMEOUT with one
// synchronous subtest run. They carry expectation lines naming that ADR. What
// does measure this code is `dom/nodes/MutationObserver-childList.html` and
// `-characterData.html`, whose eleven Range subtests assert the *number* of
// records each operation queues and the `previousSibling`/`nextSibling` on
// each -- which is a stricter check than "the tree looks right afterwards",
// because it catches a removal batched as one record when it should be several.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/LiveRanges.h"
#include "bindings/WebIdl.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// What `extractContents`, `cloneContents` and `deleteContents` each want out of
// the one walk below.
enum class ContentAction : std::uint8_t {
  Extract,  // move what is inside into a fragment
  Clone,    // copy what is inside into a fragment, leaving the tree alone
  Delete,   // remove what is inside and build nothing
};

// A boundary point read off a Range once, so the walk works from a snapshot.
// That matters: the specification's later steps refer to "original start node"
// after mutations that would have moved the live one, and re-reading the slots
// would make each step see the previous step's damage.
struct Point {
  dom::Node* node = nullptr;
  std::uint32_t offset = 0;
};

Point PointOf(const Value& range, bool start) {
  Point point;
  point.node = NodeSlot(range, start ? kStartNodeSlot : kEndNodeSlot);
  point.offset = OffsetSlot(range, start ? kStartOffsetSlot : kEndOffsetSlot);
  return point;
}

void SetPointOn(const Value& range, bool start, const dom::Node* node, std::size_t offset) {
  if (!range.IsObject() || node == nullptr) {
    return;
  }
  range.object->SetHidden(start ? kStartNodeSlot : kEndNodeSlot,
                          PointerValue(const_cast<dom::Node*>(node)));
  range.object->SetHidden(start ? kStartOffsetSlot : kEndOffsetSlot,
                          Value::Number(static_cast<double>(offset)));
}

// The same predicate as `IsInclusiveDescendant` with its arguments the other way
// round, and named for the direction the DOM's Range steps read in -- "original
// start node is an inclusive ancestor of original end node" is asked six times
// below, and spelling it backwards at each site is how the two get swapped.
bool IsAncestorOrSelf(const dom::Node* maybe_ancestor, const dom::Node* node) {
  return IsInclusiveDescendant(node, maybe_ancestor);
}

// "Contained": wholly inside the range -- both of the node's own sides lie
// between the two boundary points.
bool IsContained(const dom::Node& node, const Point& start, const Point& end) {
  if (node.Parent() == nullptr || start.node == nullptr) {
    return false;  // a root has no point either side of it
  }
  if (RootOf(node) != RootOf(*start.node)) {
    return false;
  }
  const std::size_t index = IndexIn(node);
  return ComparePoints(*node.Parent(), index, *start.node, start.offset) >= 0 &&
         ComparePoints(*node.Parent(), index + 1, *end.node, end.offset) <= 0;
}

// "Partially contained": an inclusive ancestor of exactly one of the two
// boundary nodes. *Exactly* one -- a node containing both ends contains the
// whole range and is not partially in it.
bool IsPartiallyContained(const dom::Node& node, const Point& start, const Point& end) {
  return IsAncestorOrSelf(&node, start.node) != IsAncestorOrSelf(&node, end.node);
}

// The data of a Text, Comment or ProcessingInstruction. The three share the
// CharacterData interface, which is what makes "both ends in one character
// node" a single case rather than three.
std::string SubstringOf(const dom::Node& node, std::size_t from, std::size_t count) {
  const std::string data = CharacterDataOf(&node);
  if (from >= data.size()) {
    return {};
  }
  return data.substr(from, std::min(count, data.size() - from));
}

std::string DataWithout(const dom::Node& node, std::size_t from, std::size_t count) {
  std::string data = CharacterDataOf(&node);
  if (from >= data.size()) {
    return data;
  }
  data.erase(from, std::min(count, data.size() - from));
  return data;
}

}  // namespace

void DomBindings::InstallRangeContents(const js::Value& range_interface) {
  if (!range_interface.IsObject()) {
    return;
  }
  DomBindings* self = this;

  const auto method = [this, &range_interface](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      range_interface.object->Set(name, native);
    }
  };

  // A throwaway Range for the recursion: a plain object with the four slots. It
  // never reaches script, so it needs no prototype and no identity.
  const auto sub_range = [self](const dom::Node& start_node, std::size_t start_offset,
                                const dom::Node& end_node, std::size_t end_offset) -> Value {
    const Value sub = self->interpreter_->NewObjectValue();
    if (sub.IsObject()) {
      SetPointOn(sub, true, &start_node, start_offset);
      SetPointOn(sub, false, &end_node, end_offset);
    }
    return sub;
  };

  // Move every child of `from` into `to`, in order: how an extracted
  // sub-fragment hangs off the shallow clone standing for its parent.
  const auto move_children = [self](dom::Node& from, dom::Node& to) {
    while (!from.Children().empty()) {
      dom::Node* child = from.Children().front().get();
      if (child == nullptr) {
        break;
      }
      self->InsertNodeBefore(to, child, nullptr, false);
    }
  };

  // **The one content algorithm**, taking itself so it can recurse without a
  // heap-allocated `std::function`. Returns the fragment (undefined for
  // Delete) and sets `failed` if a DOM constraint refused.
  const auto extract_impl = [self, sub_range, move_children](
                                auto&& recurse, NativeCall& call, const Value& range,
                                ContentAction action, bool& failed) -> Value {
    failed = false;
    const Point start = PointOf(range, true);
    const Point end = PointOf(range, false);
    if (start.node == nullptr || end.node == nullptr) {
      failed = true;
      call.Throw("TypeError", "the range has no boundary points");
      return Value::Undefined();
    }
    const bool wants_fragment = action != ContentAction::Delete;
    const Value fragment = wants_fragment
                               ? self->CreateDocumentFragment(self->NodeDocumentOf(*start.node))
                               : Value::Undefined();
    dom::Node* fragment_node = wants_fragment ? NodeOf(fragment) : nullptr;
    if (wants_fragment && fragment_node == nullptr) {
      failed = true;
      return Value::Undefined();
    }
    // A collapsed range holds nothing, whatever it is asked to do with it.
    if (ComparePoints(*start.node, start.offset, *end.node, end.offset) == 0) {
      return fragment;
    }

    // (1) Both ends in one character node. Nothing is reparented at all -- a
    // substring is copied out and, for Extract, spliced out of the original.
    // This is the case a naive implementation gets right, which is why a
    // broken one still looks fine on a page.
    if (start.node == end.node && IsCharacterDataNode(*start.node)) {
      const std::size_t count = end.offset - start.offset;
      if (wants_fragment) {
        std::unique_ptr<dom::Node> clone = CloneDomNode(*start.node, false);
        if (clone == nullptr) {
          failed = true;
          return Value::Undefined();
        }
        dom::Node* raw = clone.get();
        fragment_node->Append(std::move(clone));
        self->SetCharacterData(raw, SubstringOf(*start.node, start.offset, count));
      }
      if (action != ContentAction::Clone) {
        self->SetCharacterData(start.node, DataWithout(*start.node, start.offset, count));
      }
      return fragment;
    }

    // The deepest node holding both ends: where the top-level split happens.
    const std::vector<const dom::Node*> start_chain = AncestorsOf(*start.node);
    const std::vector<const dom::Node*> end_chain = AncestorsOf(*end.node);
    const dom::Node* common = nullptr;
    for (std::size_t i = 0;
         i < start_chain.size() && i < end_chain.size() && start_chain[i] == end_chain[i]; ++i) {
      common = start_chain[i];
    }
    if (common == nullptr) {
      failed = true;
      ThrowDom(call, "WrongDocumentError", "the range's ends are in different trees");
      return Value::Undefined();
    }

    // The two half-in children and everything whole between them, all collected
    // *before* anything moves -- Extract mutates the very child list this would
    // otherwise still be walking.
    dom::Node* first_partial = nullptr;
    dom::Node* last_partial = nullptr;
    std::vector<dom::Node*> contained;
    if (!IsAncestorOrSelf(start.node, end.node)) {
      for (const std::unique_ptr<dom::Node>& child : common->Children()) {
        if (child != nullptr && IsPartiallyContained(*child, start, end)) {
          first_partial = child.get();
          break;
        }
      }
    }
    if (!IsAncestorOrSelf(end.node, start.node)) {
      const std::vector<std::unique_ptr<dom::Node>>& children = common->Children();
      for (std::size_t i = children.size(); i > 0; --i) {
        if (children[i - 1] != nullptr && IsPartiallyContained(*children[i - 1], start, end)) {
          last_partial = children[i - 1].get();
          break;
        }
      }
    }
    for (const std::unique_ptr<dom::Node>& child : common->Children()) {
      if (child == nullptr || !IsContained(*child, start, end)) {
        continue;
      }
      // A doctype cannot be moved into a fragment, and the specification makes
      // that a refusal rather than a silent skip.
      if (child->GetKind() == dom::Node::Kind::DocumentType) {
        failed = true;
        ThrowDom(call, "HierarchyRequestError", "a doctype cannot be extracted");
        return Value::Undefined();
      }
      contained.push_back(child.get());
    }

    // Where the range collapses to afterwards, computed before the tree moves.
    const dom::Node* new_node = start.node;
    std::size_t new_offset = start.offset;
    if (!IsAncestorOrSelf(start.node, end.node)) {
      const dom::Node* reference = start.node;
      while (reference->Parent() != nullptr && !IsAncestorOrSelf(reference->Parent(), end.node)) {
        reference = reference->Parent();
      }
      new_node = reference->Parent();
      new_offset = IndexIn(*reference) + 1;
    }

    // (2a) The child the range starts inside, and (2b) the one it ends inside,
    // are mirror images. `leading` picks which.
    const auto half_contained = [&](dom::Node* partial, bool leading) {
      if (partial == nullptr) {
        return true;
      }
      if (IsCharacterDataNode(*partial)) {
        const std::size_t from = leading ? start.offset : 0;
        const std::size_t count = leading ? NodeLength(*partial) - from : end.offset;
        if (wants_fragment) {
          std::unique_ptr<dom::Node> clone = CloneDomNode(*partial, false);
          dom::Node* raw = clone.get();
          fragment_node->Append(std::move(clone));
          self->SetCharacterData(raw, SubstringOf(*partial, from, count));
        }
        if (action != ContentAction::Clone) {
          self->SetCharacterData(partial, DataWithout(*partial, from, count));
        }
        return true;
      }
      // Shallow clone, then the same algorithm again over just the inside
      // part. A half-contained child may itself have a half-contained child,
      // to whatever depth the page built.
      const Value inner_range =
          leading ? sub_range(*start.node, start.offset, *partial, NodeLength(*partial))
                  : sub_range(*partial, 0, *end.node, end.offset);
      std::unique_ptr<dom::Node> clone = wants_fragment ? CloneDomNode(*partial, false) : nullptr;
      dom::Node* raw = clone.get();
      if (wants_fragment) {
        fragment_node->Append(std::move(clone));
      }
      const Value inner = recurse(recurse, call, inner_range, action, failed);
      if (failed) {
        return false;
      }
      if (raw != nullptr) {
        if (dom::Node* inner_node = NodeOf(inner); inner_node != nullptr) {
          move_children(*inner_node, *raw);
        }
      }
      return true;
    };

    if (!half_contained(first_partial, true)) {
      return Value::Undefined();
    }

    // (3) The whole subtrees in the middle.
    for (dom::Node* child : contained) {
      if (action == ContentAction::Clone) {
        if (std::unique_ptr<dom::Node> clone = CloneDomNode(*child, true)) {
          fragment_node->Append(std::move(clone));
        }
      } else if (action == ContentAction::Delete) {
        self->DetachFromTree(*child, true);
      } else {
        self->InsertNodeBefore(*fragment_node, child, nullptr, false);
      }
    }

    if (!half_contained(last_partial, false)) {
      return Value::Undefined();
    }

    // A clone leaves the tree alone, so the range still describes what it did.
    if (action != ContentAction::Clone) {
      SetPointOn(range, true, new_node, new_offset);
      SetPointOn(range, false, new_node, new_offset);
    }
    return fragment;
  };
  const auto extract = [extract_impl](NativeCall& call, const Value& range, ContentAction action,
                                      bool& failed) {
    return extract_impl(extract_impl, call, range, action, failed);
  };

  const auto contents = [extract](NativeCall& call, ContentAction action) -> Value {
    bool failed = false;
    const Value fragment = extract(call, call.self, action, failed);
    if (failed) {
      return call.ThrownValue();
    }
    return action == ContentAction::Delete ? Value::Undefined() : fragment;
  };
  method("extractContents",
         [contents](NativeCall& call) { return contents(call, ContentAction::Extract); });
  method("cloneContents",
         [contents](NativeCall& call) { return contents(call, ContentAction::Clone); });
  method("deleteContents",
         [contents](NativeCall& call) { return contents(call, ContentAction::Delete); });

  // **`insertNode` splits a text node in half.** That is the part worth
  // knowing: inserting at offset 3 of an eight-character text node leaves two
  // text nodes with the new node between them. Everything else is a validated
  // pre-insertion.
  const auto insert_node = [self](NativeCall& call, dom::Node* node) -> Value {
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    if (start == nullptr) {
      return call.Throw("TypeError", "insertNode needs a positioned Range");
    }
    const std::uint32_t offset = OffsetSlot(call.self, kStartOffsetSlot);
    // A comment or processing instruction has no children to insert among, and
    // a parentless text node has nothing to be split inside.
    if (start->GetKind() == dom::Node::Kind::Comment ||
        start->GetKind() == dom::Node::Kind::ProcessingInstruction ||
        (start->IsText() && start->Parent() == nullptr) || IsAncestorOrSelf(node, start)) {
      return ThrowDom(call, "HierarchyRequestError",
                      "the range's start is not a place a node can be inserted");
    }
    dom::Node* parent = start->IsText() ? start->Parent() : start;
    dom::Node* reference = nullptr;
    if (!start->IsText()) {
      reference = offset < start->Children().size() ? start->Children()[offset].get() : nullptr;
    }
    if (parent == nullptr) {
      return ThrowDom(call, "HierarchyRequestError", "the range's start has no parent");
    }
    if (const char* error = PreInsertionError(*parent, *node, reference); error != nullptr) {
      return ThrowDom(call, error, "the node cannot be inserted there");
    }
    // The split happens *after* validity is checked, so a refused insertion
    // leaves the text node whole -- otherwise a page that caught the exception
    // would find its document already changed.
    if (start->IsText()) {
      reference = SplitTextNode(call.interpreter, static_cast<dom::Text&>(*start), offset);
      if (reference == nullptr) {
        return ThrowDom(call, "HierarchyRequestError", "the text node could not be split");
      }
      // The split is an insertion in its own right, so it owes its own
      // childList record -- `insertNode` into the middle of a text node is
      // *two* mutations, and an observer that saw only one would be told the
      // tail text node appeared from nowhere.
      self->RecordMutation(*parent, "childList", {}, Value::Null(), {reference}, {});
    }
    if (node == reference) {
      reference = NextSiblingOf(*node);
    }
    const std::size_t added =
        node->IsDocumentFragment() ? node->Children().size() : static_cast<std::size_t>(1);
    const std::size_t new_offset =
        reference == nullptr ? parent->Children().size() : IndexIn(*reference);
    dom::Node* end_node = NodeSlot(call.self, kEndNodeSlot);
    const bool collapsed =
        end_node != nullptr &&
        ComparePoints(*NodeSlot(call.self, kStartNodeSlot),
                      OffsetSlot(call.self, kStartOffsetSlot), *end_node,
                      OffsetSlot(call.self, kEndOffsetSlot)) == 0;
    self->InsertNodeBefore(*parent, node, reference, true);
    // A collapsed range grows to cover what was just put inside it, which is
    // what makes `range.insertNode(x)` then `range.toString()` answer with
    // `x`'s text rather than the empty string.
    SetPointOn(call.self, true, parent, new_offset);
    SetPointOn(call.self, false, parent, collapsed ? new_offset + added : new_offset + added);
    return Value::Undefined();
  };

  method("insertNode", [insert_node](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Range", "insertNode", 1)) {
      return call.ThrownValue();
    }
    dom::Node* node = NodeOf(call.arguments[0]);
    if (node == nullptr) {
      return call.Throw("TypeError", "insertNode needs a Node");
    }
    return insert_node(call, node);
  });

  // `surroundContents` is `extractContents` plus `insertNode` plus an append.
  // Its one original step is the refusal at the top: a range that starts inside
  // one element and ends inside another cannot be wrapped in a single parent
  // without changing which element each half belongs to. The specification
  // refuses rather than guessing, and so does this.
  method("surroundContents", [self, extract, insert_node](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Range", "surroundContents", 1)) {
      return call.ThrownValue();
    }
    dom::Node* new_parent = NodeOf(call.arguments[0]);
    if (new_parent == nullptr) {
      return call.Throw("TypeError", "surroundContents needs a Node");
    }
    const Point start = PointOf(call.self, true);
    const Point end = PointOf(call.self, false);
    if (start.node == nullptr || end.node == nullptr) {
      return call.Throw("TypeError", "surroundContents needs a positioned Range");
    }
    // Every non-Text node partially inside the range is a refusal. Walking the
    // two ancestor chains finds them all: a partially contained node is by
    // definition an inclusive ancestor of exactly one boundary.
    for (const dom::Node* boundary : {start.node, end.node}) {
      for (const dom::Node* at : AncestorsOf(*boundary)) {
        if (at != nullptr && !at->IsText() && IsPartiallyContained(*at, start, end)) {
          return ThrowDom(call, "InvalidStateError",
                          "the range partially contains a non-Text node");
        }
      }
    }
    switch (new_parent->GetKind()) {
      case dom::Node::Kind::Document:
      case dom::Node::Kind::DocumentType:
      case dom::Node::Kind::DocumentFragment:
        return ThrowDom(call, "InvalidNodeTypeError",
                        "a document, doctype or fragment cannot surround a range");
      default:
        break;
    }
    bool failed = false;
    const Value fragment = extract(call, call.self, ContentAction::Extract, failed);
    if (failed) {
      return call.ThrownValue();
    }
    self->ClearChildren(*new_parent, true);
    const Value inserted = insert_node(call, new_parent);
    if (call.HasThrown()) {
      return inserted;
    }
    if (dom::Node* fragment_node = NodeOf(fragment); fragment_node != nullptr) {
      while (!fragment_node->Children().empty()) {
        dom::Node* child = fragment_node->Children().front().get();
        if (child == nullptr) {
          break;
        }
        self->InsertNodeBefore(*new_parent, child, nullptr, true);
      }
    }
    // The range ends up around what it just wrapped.
    if (new_parent->Parent() != nullptr) {
      const std::size_t index = IndexIn(*new_parent);
      SetPointOn(call.self, true, new_parent->Parent(), index);
      SetPointOn(call.self, false, new_parent->Parent(), index + 1);
    }
    return Value::Undefined();
  });

  // ---------------------------------------------------------------------
  // **`getSelection()`, and why it belongs in this file.**
//
// A Selection is a range plus a direction. Every browser stores at most one
// range in it, and the specification says so out loud, so the whole object is
// "the range the user (or the page) has chosen" -- which makes it a consumer of
// everything above rather than a feature of its own.
//
// What this is *not* is a user selection. Nothing here paints a highlight and
// no gesture creates one, because this browser has no text-selection input yet.
// That is not a stub in ADR 0012's sense: the Selection API is the object a
// page reads and writes, and every operation on it -- add a range, ask which
// node the anchor is in, stringify it -- is answered exactly. What a page
// cannot do is find a range in there that it did not put there, and a page that
// feature-detects `getSelection` gets an empty selection, which is the same
// thing every browser reports on a page with nothing selected.
//
// The range it holds is **the very object it was given**, not a copy. That is
// what makes a selected range live: `selection.getRangeAt(0) === range` is what
// `dom/ranges/Range-mutations-*.js` checks by relying on it, and a copy would
// track the tree separately from the range the page still holds.
  //
  // Installed from here rather than from an `InstallSelection` of its own
  // because `DomBindings.h` is at its line cap and this needs no state the
  // caller does not already have.
  const Value document_interface = DocumentInterface();
  const Value selection_interface = MakeInterface("Selection", Value::Undefined());
  if (!selection_interface.IsObject()) {
    return;
  }
  static constexpr const char* kSelectionRangeSlot = "#selectionRange";

  const auto sel_method = [this, &selection_interface](const char* name,
                                                   js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      selection_interface.object->Set(name, native);
    }
  };
  const auto sel_accessor = [this, &selection_interface](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      selection_interface.object->DefineAccessor(name, native.object, nullptr);
    }
  };

  // The one range, or undefined. Every accessor below is derived from it, so
  // there is no second copy of "what is selected" to fall out of step.
  const auto held = [](const Value& receiver) -> Value {
    if (!receiver.IsObject()) {
      return Value::Undefined();
    }
    const Value* found = receiver.object->GetOwn(kSelectionRangeSlot);
    return found == nullptr ? Value::Undefined() : *found;
  };

  sel_accessor("rangeCount", [held](NativeCall& call) {
    return Value::Number(held(call.self).IsObject() ? 1 : 0);
  });
  sel_method("getRangeAt", [held](NativeCall& call) -> Value {
    std::uint32_t index = 0;
    if (!RequireArguments(call, "Selection", "getRangeAt", 1) ||
        !ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, index)) {
      return call.ThrownValue();
    }
    const Value range = held(call.self);
    if (!range.IsObject() || index != 0) {
      return ThrowDom(call, "IndexSizeError", "there is no range at that index");
    }
    return range;
  });
  sel_method("addRange", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Selection", "addRange", 1)) {
      return call.ThrownValue();
    }
    // Held by identity, not by value -- see the note above.
    if (call.self.IsObject() && call.arguments[0].IsObject()) {
      call.self.object->SetHidden(kSelectionRangeSlot, call.arguments[0]);
    }
    return Value::Undefined();
  });
  const auto clear = [](NativeCall& call) -> Value {
    if (call.self.IsObject()) {
      call.self.object->SetHidden(kSelectionRangeSlot, Value::Undefined());
    }
    return Value::Undefined();
  };
  sel_method("removeAllRanges", clear);
  sel_method("empty", clear);
  sel_method("removeRange", [held](NativeCall& call) -> Value {
    if (call.self.IsObject() && held(call.self).IsObject() &&
        held(call.self).object == Argument(call.arguments, 0).object) {
      call.self.object->SetHidden(kSelectionRangeSlot, Value::Undefined());
    }
    return Value::Undefined();
  });

  // anchor/focus are the range's start and end. A selection made by a page has
  // no direction -- only a drag has one -- so anchor is always the start, which
  // is what every engine reports for a programmatic selection.
  const auto boundary = [held](const char* slot, bool node) {
    return [held, slot, node](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      const Value range = held(call.self);
      if (owner == nullptr || !range.IsObject()) {
        return node ? Value::Null() : Value::Number(0);
      }
      if (!node) {
        return Value::Number(OffsetSlot(range, slot));
      }
      return owner->WrapperFor(NodeSlot(range, slot));
    };
  };
  sel_accessor("anchorNode", boundary(kStartNodeSlot, true));
  sel_accessor("anchorOffset", boundary(kStartOffsetSlot, false));
  sel_accessor("focusNode", boundary(kEndNodeSlot, true));
  sel_accessor("focusOffset", boundary(kEndOffsetSlot, false));
  sel_accessor("isCollapsed", [held](NativeCall& call) {
    const Value range = held(call.self);
    if (!range.IsObject()) {
      return Value::Bool(true);
    }
    dom::Node* start = NodeSlot(range, kStartNodeSlot);
    dom::Node* end = NodeSlot(range, kEndNodeSlot);
    return Value::Bool(start == nullptr || end == nullptr ||
                       ComparePoints(*start, OffsetSlot(range, kStartOffsetSlot), *end,
                                     OffsetSlot(range, kEndOffsetSlot)) == 0);
  });
  sel_accessor("type", [held](NativeCall& call) {
    const Value range = held(call.self);
    return Value::String(std::string(!range.IsObject() ? "None" : "Range"));
  });
  // The selected text, which is the range's -- through the Range's own
  // `toString`, so the two can never disagree about what is inside.
  sel_method("toString", [held](NativeCall& call) -> Value {
    const Value range = held(call.self);
    if (!range.IsObject()) {
      return Value::String(std::string());
    }
    const Value* stringify = range.object->Get("toString");
    if (stringify == nullptr || !stringify->IsObject()) {
      return Value::String(std::string());
    }
    const js::Result text = call.interpreter.CallFunction(*stringify, range, {});
    if (text.completion == js::Completion::Throw) {
      return call.ThrowValue(text.value);
    }
    return text.value;
  });

  // One Selection per page, because a page compares them: `getSelection() ===
  // getSelection()` is true in every browser, and code caches the object.
  const Value selection = interpreter_->NewObjectValue();
  if (!selection.IsObject()) {
    return;
  }
  selection.object->SetPrototype(selection_interface.object);
  interpreter_->Global()->Set("#selection", selection);

  const Value get_selection =
      interpreter_->NewNativeValue("getSelection", [selection](NativeCall&) { return selection; });
  if (!get_selection.IsObject()) {
    return;
  }
  get_selection.object->Set(kOwnerSlot, OwnerValue(this));
  interpreter_->Global()->Set("getSelection", get_selection);
  interpreter_->GlobalScope()->Declare("getSelection", get_selection, false);
  // `document.getSelection()` is the same object as `window.getSelection()`,
  // which is the specification's wording and not a convenience.
  if (document_interface.IsObject()) {
    document_interface.object->Set("getSelection", get_selection);
  }
}

}  // namespace microbrowser::bindings

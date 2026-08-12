#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/LiveRanges.h"
#include "bindings/NodeIterators.h"
#include "dom/FlatTree.h"
#include "bindings/WebIdl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Changing the tree: appending, removing, inserting and replacing.
//
// Split from DomBindings.cpp because that file reached the module's line cap,
// and the cap is written to mean a missing translation unit rather than a
// bigger file. Reading the tree and changing it are the natural line, and the
// changing half is where every lifetime rule in this module lives.
//
// The rule, once: a wrapper holds a raw `dom::Node*`, so a node freed while
// script still refers to it is a use-after-free reachable from a page.
// Nothing here frees a node. A node script has not yet placed is held in
// `unattached_`, a node script has removed is held in `detached_`, and both
// live until the document does. That leaks a removed subtree until navigation,
// which for a browser that navigates away from a page is a bounded leak rather
// than an unbounded one -- the second of the two fixes ADR 0008 names.

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// The root a node's tree hangs from, crossing every shadow boundary through
// the host. `moveBefore` is the caller: "the same tree" is what makes an
// atomic move possible, and a shadow root has no parent -- deliberately,
// ADR 0019 §2 -- so a plain parent walk stops one level too early and calls
// two nodes in one component's tree strangers.
const dom::Node* ShadowIncludingRootOf(const dom::Node& node) {
  const dom::Node* walk = &node;
  for (;;) {
    if (walk->Parent() != nullptr) {
      walk = walk->Parent();
      continue;
    }
    const dom::Element* host = dom::ShadowHostOf(*walk);
    if (host == nullptr) {
      return walk;
    }
    walk = host;
  }
}

}  // namespace

// A copy of `node`, with its children when `deep`.
//
// Copied rather than shared: a clone is a new node, and two parents pointing
// at one node is the shape of every "it moved when I edited the copy" bug.
// Shared with `document.importNode` -- see BindingSupport.h.
std::unique_ptr<dom::Node> CloneDomNode(const dom::Node& node, bool deep) {
  std::unique_ptr<dom::Node> copy;
  switch (node.GetKind()) {
    case dom::Node::Kind::Element: {
      const auto& element = static_cast<const dom::Element&>(node);
      // Namespace, prefix and every attribute's namespace travel with the
      // copy. A clone that lost them would be a different element that
      // serialized the same -- and `importNode` is how a template's stamped
      // content reaches the document, so the loss would be permanent.
      auto made = std::make_unique<dom::Element>(element.Namespace(), element.TagName(),
                                                 static_cast<std::uint32_t>(
                                                     element.Prefix().size()));
      for (const dom::Attribute& attribute : element.Attributes()) {
        made->SetAttributeNS(attribute.name_space, attribute.name, attribute.prefix_length,
                             attribute.value);
      }
      copy = std::move(made);
      break;
    }
    case dom::Node::Kind::Text:
      copy = std::make_unique<dom::Text>(static_cast<const dom::Text&>(node).Data());
      break;
    case dom::Node::Kind::Comment:
      // Comments are part of the tree a template stamps. Polymer's
      // `_stampTemplate` finds nodes by child index (`parentIndex`), and a
      // clone that drops comments shifts every index after the first one --
      // which is how youtube's stamp threw `addEventListener of undefined`
      // after DI finally worked. Cloning a comment is cheap and exact.
      copy = std::make_unique<dom::Comment>(static_cast<const dom::Comment&>(node).Data());
      break;
    case dom::Node::Kind::DocumentFragment:
      // A fragment clones to an empty fragment, and `deep` fills it below --
      // which is what makes `template.content.cloneNode(true)` the way a page
      // stamps out a repeated subtree.
      copy = std::make_unique<dom::DocumentFragment>();
      break;
    case dom::Node::Kind::ProcessingInstruction: {
      const auto& instruction = static_cast<const dom::ProcessingInstruction&>(node);
      copy = std::make_unique<dom::ProcessingInstruction>(instruction.Target(),
                                                          instruction.Data());
      break;
    }
    case dom::Node::Kind::DocumentType: {
      const auto& doctype = static_cast<const dom::DocumentType&>(node);
      copy = std::make_unique<dom::DocumentType>(doctype.Name(), doctype.PublicId(),
                                                 doctype.SystemId());
      break;
    }
    case dom::Node::Kind::Document:
      // A document clones to a document, which needs the node document of every
      // node under it to be the *copy* -- and this function has no way to say
      // so, because it returns before anything owns the result. `cloneNode` on
      // a document is refused rather than approximated; C4 in docs/wpt-plan.md
      // owns finishing it.
      return nullptr;
  }
  if (!deep) {
    return copy;
  }
  // A template's markup is in its contents rather than in its children, so a
  // deep clone that copied only the children would produce an empty template --
  // and `template.cloneNode(true)` is precisely how a page stamps out a
  // repeated subtree.
  const dom::Node* source = &node;
  dom::Node* destination = copy.get();
  if (node.IsElement()) {
    const auto& element = static_cast<const dom::Element&>(node);
    if (const dom::DocumentFragment* content = element.Content()) {
      source = content;
      destination = static_cast<dom::Element&>(*copy).Content();
    }
    // A *clonable* shadow root is cloned with its host, contents and all. Only
    // clonable: a root attached without it is deliberately left behind, which is
    // what makes `attachShadow({clonable: false})` mean anything, and it is why
    // `template.content.cloneNode(true)` of a `<template shadowrootclonable>`
    // stamps a working component while the same markup without the attribute
    // stamps a bare host.
    if (const dom::DocumentFragment* shadow = element.ShadowRoot();
        shadow != nullptr && shadow->IsClonable()) {
      const dom::ShadowAttachResult attached =
          static_cast<dom::Element&>(*copy).AttachShadow(shadow->Flags());
      if (attached.root != nullptr) {
        for (const std::unique_ptr<dom::Node>& child : shadow->Children()) {
          if (std::unique_ptr<dom::Node> child_copy = CloneDomNode(*child, true)) {
            attached.root->Append(std::move(child_copy));
          }
        }
      }
    }
  }
  for (const std::unique_ptr<dom::Node>& child : source->Children()) {
    if (std::unique_ptr<dom::Node> child_copy = CloneDomNode(*child, true)) {
      destination->Append(std::move(child_copy));
    }
  }
  return copy;
}

void DomBindings::InstallMutationMethods(const js::Value& wrapper) {
  const auto accessor = [this, &wrapper](const char* name, js::NativeFunction get,
                                         js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      setter.object->Set(kOwnerSlot, OwnerValue(this));
      wrapper.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  // `textContent` both ways. On CharacterData it is `data`; on everything else
  // it drops every child and puts one text node in their place. Polymer's text
  // property-effects write `textNode.textContent`, and treating a Text like an
  // Element left the original `[[binding]]` in `Data()` while appending a
  // sibling -- which is how youtube.com painted literal tokens after effects ran.
  accessor(
      "textContent",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr) {
          return Value::Undefined();
        }
        // **Null**, not the empty string, on a Document and a DocumentType.
        // The DOM defines `textContent` per interface and gives those two no
        // definition at all, which is how the attribute answers null -- and a
        // page tells "this node has no text" from "this node's text is empty"
        // by exactly that difference.
        if (self->GetKind() == dom::Node::Kind::Document ||
            self->GetKind() == dom::Node::Kind::DocumentType) {
          return Value::Null();
        }
        return Value::String(self->TextContent());
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        const std::string text = js::ToString(Argument(call.arguments, 0));
        if (IsCharacterDataNode(*self)) {
          // On CharacterData, `textContent` *is* `data`: "replace data" over the
          // whole node, so every live range pointing into it moves too.
          const std::size_t previous = DomStringLength(CharacterDataOf(self));
          owner->SetCharacterData(self, text);
          RangesDidReplaceData(call.interpreter, *self, 0, previous, DomStringLength(text));
          return Value::Undefined();
        }
        // "String replace all", which is "replace all" with a Text node --
        // one record carrying the old children and the new node together, for
        // the reason `replaceChild` states. An empty string replaces with
        // *nothing*, so a cleared element's record has no addedNodes rather
        // than one empty text node.
        const std::vector<dom::Node*> removed = ChildrenOf(*self);
        owner->ClearChildren(*self, false);
        std::vector<dom::Node*> added;
        if (!text.empty()) {
          if (dom::Node* node = NodeOf(owner->AppendTextTo(*self, text))) {
            added.push_back(node);
          }
        }
        if (!added.empty() || !removed.empty()) {
          owner->RecordMutation(*self, "childList", {}, Value::Null(), added, removed);
        }
        return Value::Undefined();
      });

  // `innerHTML` and `outerHTML` are on the Element interface, in
  // HtmlParsing.cpp -- which is where the specification puts them, and where
  // the fragment parsing algorithm that writes them lives.

  // `length` is part of the contract for these -- see SetFunctionLength. The
  // variadic ones (append, before, …) take 0 required arguments, which is what
  // WebIDL says their length is.
  const auto method = [this, &wrapper](const char* name, js::NativeFunction function,
                                       double arity = 0) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(native, arity);
      wrapper.object->Set(name, native);
    }
  };
  method("cloneNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "cloneNode called on a non-node");
    }
    // Shallow by default, which catches out everyone who forgets the argument
    // and is what the specification says.
    // A clone stays in the node document of what it was cloned from, which is
    // the difference between this and `importNode`.
    return owner->AdoptClone(CloneDomNode(*self, js::ToBoolean(Argument(call.arguments, 0))),
                             owner->NodeDocumentOf(*self));
  }, 0);
  method("removeChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* child = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "removeChild requires a node");
    }
    if (child->Parent() != self) {
      // The specification's NotFoundError, and it is one now rather than a
      // TypeError with that name in a comment: `assert_throws_dom` reads
      // `e.name` and `e.code`, and so does every page that tells "not a child"
      // apart from "not a node".
      return ThrowDom(call, "NotFoundError", "the node to remove is not a child of this node");
    }
    const Value wrapper_for_child = owner->WrapperFor(child);
    owner->DetachFromTree(*child);
    // The removed node is returned, still usable -- a page removes a node and
    // appends it somewhere else, and that only works because it is alive.
    return wrapper_for_child;
  }, 1);
  // DOM "normalize": in this subtree, every run of adjacent Text nodes becomes
  // one, and an empty Text node disappears. It is the operation a page runs
  // after building text by hand, and it was simply absent -- which is not a
  // missing convenience, because `foo.normalize()` on a browser that has no
  // such method is a TypeError that stops the script.
  //
  // Iterative rather than recursive, with an explicit stack: the depth here is
  // a page's tree depth, which is attacker-controlled, and this module's own
  // parse-depth bound (ADR 0009) exists because that number can be 100,000.
  method("normalize", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "normalize called on a non-node");
    }
    std::vector<dom::Node*> pending{self};
    while (!pending.empty()) {
      dom::Node* node = pending.back();
      pending.pop_back();
      for (std::size_t i = 0; i < node->Children().size();) {
        dom::Node* child = node->Children()[i].get();
        if (!child->IsText()) {
          ++i;
          continue;
        }
        if (static_cast<const dom::Text*>(child)->Data().empty()) {
          // "If length is zero, then remove node" -- and do not advance, since
          // the removal moved the next child into this slot.
          owner->DetachFromTree(*child);
          continue;
        }
        // The data of every contiguous Text sibling after it, appended in one
        // write, and only then are those siblings removed. One record each for
        // the removals, which is what the specification queues and what an
        // observer counting them is written against.
        std::string merged = static_cast<const dom::Text*>(child)->Data();
        std::vector<dom::Node*> absorbed;
        for (std::size_t j = i + 1;
             j < node->Children().size() && node->Children()[j]->IsText(); ++j) {
          absorbed.push_back(node->Children()[j].get());
          merged += static_cast<const dom::Text*>(node->Children()[j].get())->Data();
        }
        if (!absorbed.empty()) {
          // An *append*, not a rewrite: the DOM's normalize replaces data at
          // offset `length` with count 0, so a boundary already inside this
          // node keeps its offset and only the siblings' boundaries move.
          const std::size_t previous = DomStringLength(CharacterDataOf(child));
          const std::size_t written = DomStringLength(merged);
          owner->SetCharacterData(child, std::move(merged));
          RangesDidReplaceData(call.interpreter, *child, previous, 0, written - previous);
          for (dom::Node* gone : absorbed) {
            owner->DetachFromTree(*gone);
          }
        }
        ++i;
      }
      for (const std::unique_ptr<dom::Node>& child : node->Children()) {
        if (!child->IsText()) {
          pending.push_back(child.get());
        }
      }
    }
    return Value::Undefined();
  });
  method("remove", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner != nullptr && self != nullptr) {
      owner->DetachFromTree(*self);
    }
    return Value::Undefined();
  });
  method("insertBefore", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    // `insertBefore(node)` with the reference left off is a TypeError: the IDL
    // is `insertBefore(Node node, Node? child)` and the second argument is
    // *nullable*, not optional. A missing one used to mean "append", which is
    // what `appendChild` is for.
    if (!RequireArguments(call, "Node", "insertBefore", 2)) {
      return call.ThrownValue();
    }
    dom::Node* child = NodeOf(call.arguments[0]);
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "insertBefore requires a node");
    }
    const Value reference_argument = call.arguments[1];
    dom::Node* reference = NodeOf(reference_argument);
    if (reference == nullptr && !reference_argument.IsNull() &&
        !reference_argument.IsUndefined()) {
      return call.Throw("TypeError", "insertBefore's reference must be a Node or null");
    }
    if (const char* refusal = PreInsertionError(*self, *child, reference); refusal != nullptr) {
      return ThrowDom(call, refusal, "insertBefore would not produce a valid tree");
    }
    return owner->InsertNodeBefore(*self, child, reference);
  }, 2);
  method("replaceChild", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (!RequireArguments(call, "Node", "replaceChild", 2)) {
      return call.ThrownValue();
    }
    dom::Node* fresh = NodeOf(call.arguments[0]);
    dom::Node* stale = NodeOf(call.arguments[1]);
    if (owner == nullptr || self == nullptr || fresh == nullptr || stale == nullptr) {
      return call.Throw("TypeError", "replaceChild requires two nodes");
    }
    // For a replacement the reference is the node *after* the one going out --
    // "child's next sibling" in the specification -- because `stale` itself is
    // no longer in the way. Passing `stale` as the reference would make
    // `replaceChild(html, html)` refuse itself.
    if (const char* refusal = PreInsertionError(*self, *fresh, stale, stale);
        refusal != nullptr) {
      return ThrowDom(call, refusal, "replaceChild would not produce a valid tree");
    }
    // Out before in, against a reference taken *first* -- the DOM's steps 7, 8,
    // 10 and 11 in that order. It used to be in-before-out with `stale` as the
    // reference, on the argument that the new node would otherwise land at the
    // end; taking the reference before the removal is what actually keeps the
    // position, and it is the only ordering under which
    // `parent.replaceChild(b, b)` leaves `b` where it was rather than deleting
    // it.
    dom::Node* reference = NextSiblingOf(*stale);
    if (reference == fresh) {
      reference = NextSiblingOf(*fresh);
    }
    // One record for the swap, not one for each half: the DOM's replace does
    // both removal and insertion with its "suppress observers" flag set and
    // queues a single record carrying `addedNodes` *and* `removedNodes`. An
    // observer that saw two would have to guess they were related.
    //
    // The incoming node leaves its old parent **first**, and that removal does
    // get its own record -- the DOM removes it before it removes `child`, not
    // as part of the insertion afterwards. The order is observable in both
    // directions: `parent.replaceChild(parent.lastChild, parent.firstChild)`
    // must report the last child leaving while the first is still beside it,
    // and `parent.replaceChild(x, x)` must report a removal and an insertion
    // rather than one record replacing `x` with itself.
    const std::vector<dom::Node*> added = InsertedNodesOf(*fresh);
    const Value removed = owner->WrapperFor(stale);
    if (fresh->Parent() != nullptr) {
      owner->DetachFromTree(*fresh);
    }
    std::vector<dom::Node*> removed_nodes;
    if (stale->Parent() != nullptr) {
      removed_nodes.push_back(stale);
      owner->DetachFromTree(*stale, false);
    }
    owner->InsertNodeBefore(*self, fresh, reference, false);
    owner->RecordMutation(*self, "childList", {}, Value::Null(), added, removed_nodes);
    return removed;
  }, 2);

  method("appendChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* child = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "appendChild requires a node");
    }
    if (const char* refusal = PreInsertionError(*self, *child, nullptr); refusal != nullptr) {
      return ThrowDom(call, refusal, "appendChild would not produce a valid tree");
    }
    // A node that already has a parent is *moved*, which is how a page
    // reorders a list. That works now because detaching hands the node over
    // rather than destroying it.
    return owner->InsertNodeBefore(*self, child, nullptr);
  }, 1);

  // The ParentNode and ChildNode mixins -- `append`, `prepend`,
  // `replaceChildren`, `before`, `after`, `replaceWith` -- are their own
  // translation unit, NodeMixins.cpp. They are one specification section
  // ("converting nodes into a node" plus the two mixins built on it) and they
  // are the half of this file that grew past the module cap.
  InstallNodeMixins(wrapper);
}

void DomBindings::ClearChildren(dom::Node& parent, bool record) {
  // Detached rather than destroyed, one at a time from the front, for the
  // reason every removal here is: script may still hold a wrapper for any of
  // them.
  //
  // The reactions and the record are owed here as much as they are on a single
  // `removeChild`. Before this they were not delivered at all, so
  // `el.textContent = ''` disconnected a subtree of custom elements without
  // telling any of them and without an observer seeing a childList record --
  // which is exactly the leak `disconnectedCallback` exists to let a page
  // avoid. One record for the batch, like the insertion side.
  std::vector<dom::Node*> removed;
  for (const std::unique_ptr<dom::Node>& child : parent.Children()) {
    removed.push_back(child.get());
  }
  if (removed.empty()) {
    return;
  }
  // Before the detach, not after: asking whether a node is in the document has
  // to be asked while the answer is still yes.
  for (dom::Node* child : removed) {
    NotifyConnection(*child, false);
  }
  if (record) {
    RecordMutation(parent, "childList", {}, Value::Null(), {}, removed);
  }
  while (parent.FirstChild() != nullptr) {
    RangesWillRemove(*interpreter_, *parent.FirstChild());
    NodeIteratorsWillRemove(*interpreter_, *parent.FirstChild());
    std::unique_ptr<dom::Node> owned = parent.Detach(parent.FirstChild());
    if (owned == nullptr) {
      break;
    }
    detached_.push_back(std::move(owned));
  }
}

js::Value DomBindings::AdoptClone(std::unique_ptr<dom::Node> clone,
                                  dom::Document& node_document) {
  // Owned here until something appends it, exactly like a created node: a
  // clone has no parent, and a node's owner is its parent. The node document
  // is the argument rather than the source's, which is the difference between
  // `cloneNode` (same document) and `importNode` (the receiver's).
  return AdoptUnattached(std::move(clone), node_document);
}

bool DomBindings::DetachFromTree(dom::Node& child, bool record) {
  dom::Node* parent = child.Parent();
  if (parent == nullptr) {
    return false;
  }
  // Before the detach, not after: `disconnectedCallback` runs on a node that
  // is on its way out, and asking whether it is in the document has to be
  // asked while the answer is still yes.
  NotifyConnection(child, false);
  if (record) {
    RecordMutation(*parent, "childList", {}, Value::Null(), {}, {&child});
  }
  // A live range whose boundary is inside this subtree collapses onto the gap
  // it is about to leave. Before the detach, because the fixup needs the index
  // the node still has.
  RangesWillRemove(*interpreter_, child);
  // And a NodeIterator whose reference is inside it moves to the gap, which is
  // the other half of the same rule and has to be asked before the detach for
  // the same reason. See NodeIterators.h.
  NodeIteratorsWillRemove(*interpreter_, child);
  std::unique_ptr<dom::Node> owned = parent->Detach(&child);
  if (owned == nullptr) {
    return false;
  }
  detached_.push_back(std::move(owned));
  return true;
}

js::Value DomBindings::InsertNodeBefore(dom::Node& parent, dom::Node* child,
                                        dom::Node* reference, bool record, bool preserve) {
  // "If reference child is node, then set reference child to node's next
  // sibling" -- DOM pre-insert step 3. Without it, `a.insertBefore(b, b)`
  // detaches `b`, finds its reference gone, and appends: the node *moves*, to
  // the end, for a call every specification says is a no-op.
  if (reference != nullptr && reference == child) {
    reference = NextSiblingOf(*child);
  }
  // Inserting a fragment inserts its *children* and leaves it empty. That is
  // what a fragment is for -- a framework assembles a subtree in one and
  // places it in a single operation -- and it has to happen here rather than
  // at each caller, because this is the funnel appendChild, insertBefore and
  // replaceChild all come through.
  //
  // The fragment itself is not inserted and stays owned where it was, so a
  // page that keeps a reference to it and fills it again gets an empty
  // fragment rather than a detached one.
  //
  // The batch is announced the same way an `innerHTML` insertion is, and
  // through the same code: before this, appending a fragment fired no
  // `connectedCallback` and produced no childList record at all, so a framework
  // that assembled its subtree in a fragment -- which is the whole reason to
  // use one -- got neither.
  if (child != nullptr && child->IsDocumentFragment()) {
    InsertFragmentChildren(parent, *child, reference, record);
    return WrapperFor(child);
  }
  // A node with a parent is moved rather than refused, now that detaching is
  // possible: `parent.appendChild(existing)` is how a page reorders a list,
  // and it only works because the node survives leaving its old parent.
  // A move is a removal followed by an insertion, and the removal half owes the same
  // `disconnectedCallback` and the same childList record a `removeChild` owes -- before the
  // detach, while "is this in the document" still answers yes. Reactions are script and script can
  // move the node again, so where it lives is re-read afterwards rather than remembered.
  if (child->Parent() != nullptr && !preserve) {
    NotifyConnection(*child, false);
  }
  std::unique_ptr<dom::Node> owned;
  if (dom::Node* old_parent = child->Parent(); old_parent != nullptr) {
    RecordMutation(*old_parent, "childList", {}, Value::Null(), {}, {child});
    // A move is a removal followed by an insertion for live ranges too.
    RangesWillRemove(*interpreter_, *child);
    NodeIteratorsWillRemove(*interpreter_, *child);
    owned = old_parent->Detach(child);
  } else {
    for (std::size_t i = 0; i < unattached_.size(); ++i) {
      if (unattached_[i].get() == child) {
        owned = std::move(unattached_[i]);
        unattached_.erase(unattached_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    if (owned == nullptr) {
      for (std::size_t i = 0; i < detached_.size(); ++i) {
        if (detached_[i].get() == child) {
          owned = std::move(detached_[i]);
          detached_.erase(detached_.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
      }
    }
    if (owned == nullptr && child->NodeDocument() != nullptr) {
      // **A node another document made, being inserted into this one.** Its
      // `unique_ptr` is parked in the binding layer that created it, not in
      // this one, so without this the whole insertion silently answered null --
      // `document.body.appendChild(frame.contentWindow.document.createElement('div'))`
      // did nothing at all. It only became reachable when `contentWindow`
      // started being the child's real window (ADR 0042 §5): before that,
      // `contentWindow.document` was the embedder's own document and no node
      // ever crossed. The DOM calls this step "adopt", and `Node::Append` does
      // its other half -- rewriting the node document of the whole subtree.
      if (DomBindings* home = BindingsForDocument(*child->NodeDocument());
          home != nullptr && home != this) {
        owned = home->TakeUnattached(child);
      }
    }
  }
  if (owned == nullptr) {
    return Value::Null();  // not a node this layer can give away
  }
  if (reference != nullptr && reference->Parent() == &parent) {
    parent.InsertBefore(std::move(owned), reference);
  } else {
    parent.Append(std::move(owned));
  }
  // Boundary points in this parent past where the node landed shift along by
  // one. After the insertion, because the index is read off the tree.
  RangesDidInsert(*interpreter_, parent, IndexIn(*child), 1);
  // Connected now, if this put it in the document. The subtree as well as the
  // node: appending a detached tree connects everything in it, and a custom
  // element three levels down is as connected as the root is.
  if (!preserve) {
    NotifyConnection(*child, true);
  }
  if (child->IsElement()) {
    const auto& element = static_cast<const dom::Element&>(*child);
    if (element.TagName() == "script") {
      if (InTrustedScriptContext()) {
        csp_trusted_scripts_.insert(&element);
      }
      // Flush when the element is already trusted (e.g. createElement marked it)
      // even if this append is outside a trusted context — otherwise a script
      // stamped under trust and appended a tick later never enters CollectInserted.
      if (IsCspTrustedScript(element) && trusted_script_flush_) {
        trusted_script_flush_();
      }
    } else if (element.TagName() == "iframe" && element.GetAttribute("src") == nullptr) {
      // es-module-shims feature detection inserts a hidden iframe with `srcdoc` after registering a
      // `message` listener. Without `srcdoc`/`contentDocument`, the iframe never posts back; answer
      // with the same synthetic `esms` tuple the trusted-insert path used in session 53.
      MaybeCompleteEsmsFeatureDetection();
    }
  }
  // The record goes to observers of the *parent*, because childList is about
  // a node's children changing rather than about the child.
  if (record) {
    RecordMutation(parent, "childList", {}, Value::Null(), {child}, {});
  }
  return WrapperFor(child);
}

void DomBindings::NotifyConnection(dom::Node& node, bool connected) {
  const Value registry = CustomElementRegistry();
  if (!registry.IsObject() || registry.object->Keys().empty()) {
    return;  // no custom elements defined: nothing to tell, and no walk to do
  }
  // Parent-walking misses nodes inside a shadow root: the root has no parent.
  // ConnectedDocument crosses through the host, which is what makes a stamped
  // custom element in a component tree count as connected -- and it is the
  // *connected* document rather than the node document on purpose, because a
  // reaction is owed only to a node that is actually in this page's tree.
  if (node.ConnectedDocument() != document_) {
    return;
  }
  // The node and everything under it, because a reaction is owed to each.
  if (node.IsElement()) {
    RunElementReaction(static_cast<dom::Element&>(node),
                       connected ? "connectedCallback" : "disconnectedCallback");
  }
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    NotifyConnection(*child, connected);
  }
}


// `moveBefore`, and it is here rather than beside the other ParentNode queries
// because it is a *mutation*: it is the insertion below with two things taken
// away.
void DomBindings::InstallAtomicMove(const js::Value& target) {
  // `moveBefore`: an **atomic move**, and the whole difference from
  // `insertBefore` is what it refuses and what it does not run. A node moved
  // this way never leaves the document, so no `disconnectedCallback` and no
  // `connectedCallback` fire -- which is the point, because a reconnect is
  // where a custom element loses whatever it had built.
  //
  // Buying that costs refusals. Both ends must already be connected and in the
  // same tree, and only an Element or CharacterData may move: everything the
  // method promises to preserve depends on there being nothing to tear down,
  // and a node crossing a document boundary has to be torn down.
  //
  // On ParentNode rather than Node -- `"moveBefore" in textNode` is false --
  // for the reason every other ParentNode method is here.
  const Value native = interpreter_->NewNativeValue(
      "moveBefore", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    // `moveBefore(Node node, Node? child)`: two required arguments, the second
    // nullable rather than optional. A missing reference is a TypeError and
    // not an append -- the same rule `insertBefore` follows.
    if (!RequireArguments(call, "Node", "moveBefore", 2)) {
      return call.ThrownValue();
    }
    dom::Node* child = NodeOf(call.arguments[0]);
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "moveBefore requires a node");
    }
    const Value reference_argument = call.arguments[1];
    dom::Node* reference = NodeOf(reference_argument);
    if (reference == nullptr && !reference_argument.IsNull() &&
        !reference_argument.IsUndefined()) {
      return call.Throw("TypeError", "moveBefore's reference must be a Node or null");
    }
    // Pre-move validity, step 2: **the same shadow-including root**, which is
    // the check that makes the move atomic and which every later step assumes.
    //
    // Not "both connected", which is the obvious reading and is wrong in both
    // directions: two disconnected nodes under one detached root move fine,
    // and a connected node and a disconnected one never share a root. Being in
    // the document is a consequence of the rule rather than the rule.
    if (ShadowIncludingRootOf(*self) != ShadowIncludingRootOf(*child)) {
      return ThrowDom(call, "HierarchyRequestError",
                      "moveBefore needs both nodes in the same tree");
    }
    // Step 5: only an Element or CharacterData. A doctype or a fragment has
    // state this method cannot promise anything about.
    switch (child->GetKind()) {
      case dom::Node::Kind::Element:
      case dom::Node::Kind::Text:
      case dom::Node::Kind::Comment:
      case dom::Node::Kind::ProcessingInstruction:
        break;
      case dom::Node::Kind::Document:
      case dom::Node::Kind::DocumentType:
      case dom::Node::Kind::DocumentFragment:
        return ThrowDom(call, "HierarchyRequestError",
                        "only an Element or CharacterData can be moved");
    }
    // Steps 3, 4, 6 and 7 are the ordinary pre-insertion validity: the parent
    // must be able to have children, the node must not contain the parent, the
    // reference must be this parent's child, and a document's one element child
    // and one doctype still hold.
    if (const char* refusal = PreInsertionError(*self, *child, reference); refusal != nullptr) {
      return ThrowDom(call, refusal, "moveBefore would not produce a valid tree");
    }
    owner->InsertNodeBefore(*self, child, reference, /*record=*/true, /*preserve=*/true);
    return Value::Undefined();
  });
  if (!native.IsObject()) {
    return;
  }
  native.object->Set(kOwnerSlot, OwnerValue(this));
  target.object->Set("moveBefore", native);
  // **`moveBefore.length` is 2, and it is load-bearing rather than cosmetic.**
  // WPT's shared pre-insertion helper branches on it -- `parent[method].length
  // > 1` decides whether to pass the reference at all, because passing null
  // blindly would move nodes before the validation it is testing. A length of
  // 0 sent every one of those cases down the one-argument path and got a
  // TypeError where the test wanted a HierarchyRequestError.
  SetFunctionLength(native, 2);
}


}  // namespace microbrowser::bindings

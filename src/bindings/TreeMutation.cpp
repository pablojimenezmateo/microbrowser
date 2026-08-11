#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
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

// DOM §4.2.3, "ensure pre-insertion validity": the WebIDL error name this
// insertion must be refused with, or null when it is allowed.
//
// One function for all three of `appendChild`, `insertBefore` and
// `replaceChild`, because the specification has one and three copies is three
// chances to disagree. Before this the checks were absent: appending a document
// to an element, or anything at all to a text node, quietly built a tree no
// other browser would produce -- and the *reason* a page cares is that the DOM
// answers these with an exception it can catch, not with a corrupt tree it
// cannot see.
// `replacing` is null for an insertion and the outgoing child for a
// `replaceChild`. The DOM states the two algorithms separately and they differ
// in exactly that: every "does the parent already have one" question below
// must not count the node that is on its way out.
const char* PreInsertionError(const dom::Node& parent, const dom::Node& node,
                              const dom::Node* reference, const dom::Node* replacing = nullptr) {
  // 1. Only these three can have children.
  switch (parent.GetKind()) {
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentFragment:
    case dom::Node::Kind::Element:
      break;
    case dom::Node::Kind::DocumentType:
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
      return "HierarchyRequestError";
  }
  // 2. A node cannot be inserted into itself or into its own descendant. The
  // walk is up from the parent, which is bounded by the tree's depth -- and it
  // is the check that stops a page turning its document into a cycle.
  for (const dom::Node* walk = &parent; walk != nullptr; walk = walk->Parent()) {
    if (walk == &node) {
      return "HierarchyRequestError";
    }
  }
  // 3. The reference node has to be a child of this parent. `null` means
  // "append", which is always in range.
  if (reference != nullptr && reference->Parent() != &parent) {
    return "NotFoundError";
  }
  // 4. And only these four kinds can be inserted at all.
  switch (node.GetKind()) {
    case dom::Node::Kind::DocumentFragment:
    case dom::Node::Kind::DocumentType:
    case dom::Node::Kind::Element:
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
      break;
    case dom::Node::Kind::Document:
      return "HierarchyRequestError";
  }
  // 5. Text does not belong directly in a document, and a doctype belongs
  // nowhere else.
  const bool parent_is_document = parent.GetKind() == dom::Node::Kind::Document;
  if (node.IsText() && parent_is_document) {
    return "HierarchyRequestError";
  }
  if (node.GetKind() == dom::Node::Kind::DocumentType && !parent_is_document) {
    return "HierarchyRequestError";
  }
  if (!parent_is_document) {
    return nullptr;
  }
  // 6. **A document has at most one element child and at most one doctype, and
  // the doctype comes first.** These were left out until 2026-08-11 on the
  // argument that "a page that reaches them is doing something no page does",
  // and the suite priced that at 41 subtests across Node-insertBefore.html and
  // Node-replaceChild.html. They are also the constraints that keep
  // `document.documentElement` a question with one answer.
  //
  // `replacing` is the child being replaced, which is excluded from every count
  // below: `replaceChild(newHtml, oldHtml)` is legal precisely because the
  // element that is in the way is the one going out.
  const auto has_child = [&parent, replacing](dom::Node::Kind kind, const dom::Node* except) {
    for (const std::unique_ptr<dom::Node>& child : parent.Children()) {
      if (child.get() != except && child.get() != replacing && child->GetKind() == kind) {
        return true;
      }
    }
    return false;
  };
  // "A doctype is following `child`" / "an element is preceding `child`".
  const auto sibling_of_kind = [&parent, reference](dom::Node::Kind kind, bool after) {
    bool seen_reference = false;
    for (const std::unique_ptr<dom::Node>& child : parent.Children()) {
      if (child.get() == reference) {
        seen_reference = true;
        continue;
      }
      if (child->GetKind() == kind && seen_reference == after) {
        return true;
      }
    }
    return false;
  };
  // "child is a doctype, or a doctype is following child" -- the first half of
  // which is an *insertion* rule only: when `child` is being replaced it is on
  // its way out and cannot be in the way of anything.
  const auto doctype_is_in_the_way = [&sibling_of_kind, reference, replacing]() {
    if (reference == nullptr) {
      return false;
    }
    if (replacing == nullptr && reference->GetKind() == dom::Node::Kind::DocumentType) {
      return true;
    }
    return sibling_of_kind(dom::Node::Kind::DocumentType, true);
  };
  switch (node.GetKind()) {
    case dom::Node::Kind::DocumentFragment: {
      std::size_t elements = 0;
      for (const std::unique_ptr<dom::Node>& child : node.Children()) {
        if (child->IsElement()) {
          ++elements;
        } else if (child->IsText()) {
          return "HierarchyRequestError";
        }
      }
      if (elements > 1) {
        return "HierarchyRequestError";
      }
      if (elements == 1 && (has_child(dom::Node::Kind::Element, nullptr) ||
                            doctype_is_in_the_way())) {
        return "HierarchyRequestError";
      }
      return nullptr;
    }
    case dom::Node::Kind::Element:
      if (has_child(dom::Node::Kind::Element, nullptr) || doctype_is_in_the_way()) {
        return "HierarchyRequestError";
      }
      return nullptr;
    case dom::Node::Kind::DocumentType:
      if (has_child(dom::Node::Kind::DocumentType, nullptr)) {
        return "HierarchyRequestError";
      }
      if (reference == nullptr ? has_child(dom::Node::Kind::Element, nullptr)
                               : sibling_of_kind(dom::Node::Kind::Element, false)) {
        return "HierarchyRequestError";
      }
      return nullptr;
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
    case dom::Node::Kind::Document:
      return nullptr;
  }
  return nullptr;
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
    if (const dom::DocumentFragment* content = static_cast<const dom::Element&>(node).Content()) {
      source = content;
      destination = static_cast<dom::Element&>(*copy).Content();
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
      getter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kOwnerSlot, PointerValue(this));
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
        return self == nullptr ? Value::Undefined() : Value::String(self->TextContent());
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        const std::string text = js::ToString(Argument(call.arguments, 0));
        if (IsCharacterDataNode(*self)) {
          owner->SetCharacterData(self, text);
          return Value::Undefined();
        }
        owner->ClearChildren(*self);
        if (!text.empty()) {
          owner->AppendTextTo(*self, text);
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
      native.object->Set(kOwnerSlot, PointerValue(this));
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
    const Value removed = owner->WrapperFor(stale);
    owner->DetachFromTree(*stale);
    owner->InsertNodeBefore(*self, fresh, reference);
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

  // ParentNode: `append`, `prepend`, and `replaceChildren`. reddit's
  // `ac-render-template` hoists `<template for="…">` markup with
  // `target.replaceChildren(template.content.cloneNode(true))` -- without
  // `replaceChildren` the call throws, the template never moves, and the feed
  // stays empty while the sidebar card (server-rendered elsewhere) still paints.
  const auto insert_argument = [this](dom::Node& parent, dom::Node* reference,
                                      const js::Value& argument) {
    if (dom::Node* child = NodeOf(argument)) {
      (void)InsertNodeBefore(parent, child, reference);
      return;
    }
    const js::Value text = CreateText(js::ToString(argument), NodeDocumentOf(parent));
    if (dom::Node* node = NodeOf(text)) {
      (void)InsertNodeBefore(parent, node, reference);
    }
  };
  method("append", [insert_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "append called on a non-node");
    }
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      insert_argument(*self, nullptr, Argument(call.arguments, i));
    }
    return Value::Undefined();
  });
  method("prepend", [insert_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "prepend called on a non-node");
    }
    dom::Node* reference = self->FirstChild();
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      insert_argument(*self, reference, Argument(call.arguments, i));
    }
    return Value::Undefined();
  });
  method("replaceChildren", [this, insert_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "replaceChildren called on a non-node");
    }
    ClearChildren(*self);
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      insert_argument(*self, nullptr, Argument(call.arguments, i));
    }
    return Value::Undefined();
  });
  // ChildNode: `before` and `after`, which insert siblings.
  //
  // The DOM defines both against a *viable* sibling -- the nearest one that is
  // not itself in the argument list -- and that is not a detail: `x.after(y)`
  // where `y` is already the next sibling has to insert after `y`'s old
  // position, not before it, or the call is a no-op that looks like a bug in
  // the page. Both were simply absent, which cost 90 subtests across
  // ChildNode-before.html and ChildNode-after.html.
  const auto is_argument = [](const NativeCall& call, const dom::Node* node) {
    for (const js::Value& argument : call.arguments) {
      if (NodeOf(argument) == node) {
        return true;
      }
    }
    return false;
  };
  method("before", [insert_argument, is_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "before called on a non-node");
    }
    dom::Node* parent = self->Parent();
    if (parent == nullptr) {
      // "If parent is null, then return" -- the same rule replaceWith follows.
      return Value::Undefined();
    }
    // The DOM's "viable previous sibling", then insert before *its next
    // sibling* -- which is the first child when there is no viable one.
    const std::vector<std::unique_ptr<dom::Node>>& children = parent->Children();
    std::size_t reference_index = 0;
    for (std::size_t i = 0; i < children.size(); ++i) {
      if (children[i].get() == self) {
        break;
      }
      if (!is_argument(call, children[i].get())) {
        reference_index = i + 1;
      }
    }
    dom::Node* reference =
        reference_index < children.size() ? children[reference_index].get() : nullptr;
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      insert_argument(*parent, reference, Argument(call.arguments, i));
    }
    return Value::Undefined();
  });
  method("after", [insert_argument, is_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "after called on a non-node");
    }
    dom::Node* parent = self->Parent();
    if (parent == nullptr) {
      return Value::Undefined();
    }
    // The viable next sibling is the reference to insert before.
    dom::Node* reference = nullptr;
    bool past_self = false;
    for (const std::unique_ptr<dom::Node>& child : parent->Children()) {
      if (child.get() == self) {
        past_self = true;
        continue;
      }
      if (past_self && !is_argument(call, child.get())) {
        reference = child.get();
        break;
      }
    }
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      insert_argument(*parent, reference, Argument(call.arguments, i));
    }
    return Value::Undefined();
  });
  // ChildNode: swap this node out for one or more nodes in the same parent.
  method("replaceWith", [insert_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "replaceWith called on a non-node");
    }
    dom::Node* parent = self->Parent();
    if (parent == nullptr) {
      // DOM §"replace this with nodes": "If parent is null, then return." Not
      // an exception -- this threw a NotFoundError until the specification was
      // read next to it, and a page that calls `replaceWith` on a node it has
      // already detached is doing something ordinary.
      return Value::Undefined();
    }
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      insert_argument(*parent, self, Argument(call.arguments, i));
    }
    owner->DetachFromTree(*self);
    return Value::Undefined();
  });

}

void DomBindings::ClearChildren(dom::Node& parent) {
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
  RecordMutation(parent, "childList", {}, Value::Null(), {}, removed);
  while (parent.FirstChild() != nullptr) {
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

bool DomBindings::DetachFromTree(dom::Node& child) {
  dom::Node* parent = child.Parent();
  if (parent == nullptr) {
    return false;
  }
  // Before the detach, not after: `disconnectedCallback` runs on a node that
  // is on its way out, and asking whether it is in the document has to be
  // asked while the answer is still yes.
  NotifyConnection(child, false);
  RecordMutation(*parent, "childList", {}, Value::Null(), {}, {&child});
  std::unique_ptr<dom::Node> owned = parent->Detach(&child);
  if (owned == nullptr) {
    return false;
  }
  detached_.push_back(std::move(owned));
  return true;
}

js::Value DomBindings::InsertNodeBefore(dom::Node& parent, dom::Node* child,
                                        dom::Node* reference) {
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
    InsertFragmentChildren(parent, *child, reference);
    return WrapperFor(child);
  }
  // A node with a parent is moved rather than refused, now that detaching is
  // possible: `parent.appendChild(existing)` is how a page reorders a list,
  // and it only works because the node survives leaving its old parent.
  // A move is a removal followed by an insertion, and the removal half owes the same
  // `disconnectedCallback` and the same childList record a `removeChild` owes -- before the
  // detach, while "is this in the document" still answers yes. Reactions are script and script can
  // move the node again, so where it lives is re-read afterwards rather than remembered.
  if (child->Parent() != nullptr) {
    NotifyConnection(*child, false);
  }
  std::unique_ptr<dom::Node> owned;
  if (dom::Node* old_parent = child->Parent(); old_parent != nullptr) {
    RecordMutation(*old_parent, "childList", {}, Value::Null(), {}, {child});
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
  }
  if (owned == nullptr) {
    return Value::Null();  // not a node this layer can give away
  }
  if (reference != nullptr && reference->Parent() == &parent) {
    parent.InsertBefore(std::move(owned), reference);
  } else {
    parent.Append(std::move(owned));
  }
  // Connected now, if this put it in the document. The subtree as well as the
  // node: appending a detached tree connects everything in it, and a custom
  // element three levels down is as connected as the root is.
  NotifyConnection(*child, true);
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
  RecordMutation(parent, "childList", {}, Value::Null(), {child}, {});
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

js::Value DomBindings::AdoptInto(dom::Node& parent, dom::Node* child) {
  for (std::size_t i = 0; i < unattached_.size(); ++i) {
    if (unattached_[i].get() != child) {
      continue;
    }
    std::unique_ptr<dom::Node> owned = std::move(unattached_[i]);
    unattached_.erase(unattached_.begin() + static_cast<std::ptrdiff_t>(i));
    parent.Append(std::move(owned));
    return WrapperFor(child);
  }
  // Not one of ours to give away. Appending it would mean taking it from its
  // current owner, which is the detach this slice deliberately cannot do.
  return Value::Null();
}


}  // namespace microbrowser::bindings

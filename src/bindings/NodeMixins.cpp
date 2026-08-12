// The ParentNode and ChildNode mixins: `append`, `prepend`, `replaceChildren`,
// `before`, `after` and `replaceWith`.
//
// Split from TreeMutation.cpp, which holds the DOM's own mutation methods and
// the ownership machinery under them. These six are one specification section
// rather than six methods -- every one of them begins with "converting nodes
// into a node" (DOM §4.2.6) and ends in a pre-insertion, and the shared half is
// most of the file. Keeping them together is what stops the next `moveBefore`
// from being a seventh copy of it.

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/WebIdl.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

void DomBindings::InstallNodeMixins(const js::Value& wrapper) {
  // Every one of these is variadic, so every one has an IDL `length` of 0 --
  // see SetFunctionLength for why a native carrying its arity is part of the
  // contract rather than a nicety.
  const auto method = [this, &wrapper](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      SetFunctionLength(native, 0);
      wrapper.object->Set(name, native);
    }
  };

  // "Converting nodes into a node": `(Node or DOMString)...` becomes *one* node
  // -- the single argument when there is exactly one, a DocumentFragment
  // holding all of them otherwise -- which is then **pre-inserted**, validity
  // check and all.
  //
  // These are load-bearing on real pages, not corner API: reddit's
  // `ac-render-template` hoists `<template for="…">` markup with
  // `target.replaceChildren(template.content.cloneNode(true))`, and without
  // `replaceChildren` the call throws, the template never moves, and the feed
  // stays empty while the sidebar card (server-rendered elsewhere) still paints.
  //
  // That is not bookkeeping, and inserting the arguments one at a time is not
  // a simplification of it. Two things are lost. The insertion stops being
  // atomic, so `doc.append(a, b)` on a document that already has an element
  // half-succeeds where it must throw and change nothing. And nothing is
  // validated: `body.append(body)` built a **cycle**, which is not a wrong
  // tree but a hang -- every walk from the root loops forever. Three files in
  // dom/nodes (ParentNode-append, ParentNode-prepend,
  // ParentNode-replaceChildren) ran this process to the wall-clock budget and
  // reported no subtest at all, and append-on-Document/prepend-on-Document
  // with them.
  //
  // The fragment is a **stack** object, unlike every other node this module
  // makes. It is never handed to script -- inserting a fragment inserts its
  // children and leaves it empty -- so registering it in `unattached_` would
  // be a per-call allocation that lives until navigation, and
  // `list.replaceChildren()` is how a page clears a list.
  //
  // What it costs is this destructor. Anything still inside the fragment when
  // the call unwinds -- a refused insertion, where the arguments were gathered
  // before the validity check said no -- must go to `detached_`, because script
  // holds wrappers for those nodes and freeing one under a wrapper is a
  // use-after-free reachable from a page. Draining here rather than at each
  // throw is not tidiness: the node being pre-inserted is *sometimes the page's
  // own* DocumentFragment, and emptying that one on a refusal would silently
  // destroy a subtree the page built.
  struct StackFragment {
    DomBindings& bindings;
    dom::DocumentFragment fragment;
    ~StackFragment() { bindings.ClearChildren(fragment, false); }
  };
  const auto convert_nodes = [this](NativeCall& call, dom::Document& node_document,
                                    dom::DocumentFragment& holder) -> dom::Node* {
    std::vector<dom::Node*> nodes;
    nodes.reserve(call.arguments.size());
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      const js::Value argument = Argument(call.arguments, i);
      if (dom::Node* node = NodeOf(argument)) {
        nodes.push_back(node);
        continue;
      }
      // A DOMString, through the conversion that can run the page's own
      // `toString` and therefore can throw. Half a mutation is not an option:
      // nothing has been moved yet at this point.
      std::string text;
      if (!ToDomString(call, argument, text)) {
        return nullptr;
      }
      dom::Node* made = NodeOf(CreateText(text, node_document));
      if (made == nullptr) {
        return nullptr;
      }
      nodes.push_back(made);
    }
    if (nodes.size() == 1) {
      return nodes[0];
    }
    // Zero arguments included: an empty fragment pre-inserts nothing, which is
    // exactly what `el.append()` does, and it still asks the validity question
    // about the parent.
    for (dom::Node* node : nodes) {
      (void)InsertNodeBefore(holder, node, nullptr);
    }
    return &holder;
  };
  // A fragment goes in through `InsertFragmentChildren` rather than
  // `InsertNodeBefore`, which is not an optimisation: `InsertNodeBefore`
  // answers with `WrapperFor(child)`, and taking a JavaScript wrapper on the
  // stack fragment above would cache a pointer that dies with this call --
  // and the wrapper cache is keyed by address, so the next node allocated
  // there would answer as that fragment. The two do the same work otherwise;
  // `InsertNodeBefore` dispatches to this function for a fragment itself.
  const auto insert_node = [this](dom::Node& parent, dom::Node& node, dom::Node* reference,
                                  bool record = true) {
    if (node.IsDocumentFragment()) {
      InsertFragmentChildren(parent, node, reference, record);
      return;
    }
    (void)InsertNodeBefore(parent, &node, reference, record);
  };
  // The pre-insert half, shared by all six: refuse or insert. The return value
  // is what the native returns, because every one of the six answers
  // `undefined` when it succeeds.
  const auto pre_insert = [this, insert_node](NativeCall& call, dom::Node& parent,
                                              dom::Node& node, dom::Node* reference,
                                              const char* what) -> Value {
    if (const char* refusal = PreInsertionError(parent, node, reference); refusal != nullptr) {
      return ThrowDom(call, refusal, std::string(what) + " would not produce a valid tree");
    }
    insert_node(parent, node, reference);
    return Value::Undefined();
  };
  method("append", [convert_nodes, pre_insert](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "append called on a non-node");
    }
    StackFragment holder{*owner, {}};
    dom::Node* node = convert_nodes(call, owner->NodeDocumentOf(*self), holder.fragment);
    if (node == nullptr) {
      return call.ThrownValue();
    }
    return pre_insert(call, *self, *node, nullptr, "append");
  });
  method("prepend", [convert_nodes, pre_insert](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "prepend called on a non-node");
    }
    StackFragment holder{*owner, {}};
    dom::Node* node = convert_nodes(call, owner->NodeDocumentOf(*self), holder.fragment);
    if (node == nullptr) {
      return call.ThrownValue();
    }
    // The first child is read *after* the conversion, because the conversion
    // may have taken it: `parent.prepend(parent.firstChild, x)` moves it.
    return pre_insert(call, *self, *node, self->FirstChild(), "prepend");
  });
  method("replaceChildren", [this, convert_nodes, insert_node](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "replaceChildren called on a non-node");
    }
    StackFragment holder{*owner, {}};
    dom::Node* node = convert_nodes(call, owner->NodeDocumentOf(*self), holder.fragment);
    if (node == nullptr) {
      return call.ThrownValue();
    }
    // Validity *before* the removal, which is the difference between this and
    // the other five: a refused `doc.replaceChildren(a, b)` must leave the
    // document's children where they were rather than emptying it first.
    if (const char* refusal = PreInsertionError(*self, *node, nullptr, nullptr, true);
        refusal != nullptr) {
      return ThrowDom(call, refusal, "replaceChildren would not produce a valid tree");
    }
    // "Replace all with node within this", which is one record for both halves
    // -- see `replaceChild` in TreeMutation.cpp for why.
    const std::vector<dom::Node*> removed = ChildrenOf(*self);
    const std::vector<dom::Node*> added = InsertedNodesOf(*node);
    ClearChildren(*self, false);
    insert_node(*self, *node, nullptr, false);
    if (!added.empty() || !removed.empty()) {
      RecordMutation(*self, "childList", {}, Value::Null(), added, removed);
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
  //
  // The viable sibling is found *before* the conversion and used *after* it,
  // because the conversion is what removes the argument nodes from this
  // parent: `child.before(x, y, z)` where `y` is already a sibling has to
  // land all three where `y` used to be.
  const auto is_argument = [](const NativeCall& call, const dom::Node* node) {
    for (const js::Value& argument : call.arguments) {
      if (NodeOf(argument) == node) {
        return true;
      }
    }
    return false;
  };
  method("before", [convert_nodes, pre_insert, is_argument](NativeCall& call) -> Value {
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
    dom::Node* viable = nullptr;
    for (const std::unique_ptr<dom::Node>& child : parent->Children()) {
      if (child.get() == self) {
        break;
      }
      if (!is_argument(call, child.get())) {
        viable = child.get();
      }
    }
    StackFragment holder{*owner, {}};
    dom::Node* node = convert_nodes(call, owner->NodeDocumentOf(*self), holder.fragment);
    if (node == nullptr) {
      return call.ThrownValue();
    }
    // "If viable previous sibling is null, set it to parent's first child;
    // otherwise to its next sibling", and then insert before that.
    dom::Node* reference = viable == nullptr ? parent->FirstChild() : NextSiblingOf(*viable);
    return pre_insert(call, *parent, *node, reference, "before");
  });
  method("after", [convert_nodes, pre_insert, is_argument](NativeCall& call) -> Value {
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
    StackFragment holder{*owner, {}};
    dom::Node* node = convert_nodes(call, owner->NodeDocumentOf(*self), holder.fragment);
    if (node == nullptr) {
      return call.ThrownValue();
    }
    return pre_insert(call, *parent, *node, reference, "after");
  });
  // ChildNode: swap this node out for one or more nodes in the same parent.
  method("replaceWith", [this, convert_nodes, pre_insert, insert_node,
                         is_argument](NativeCall& call) -> Value {
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
    StackFragment holder{*owner, {}};
    dom::Node* node = convert_nodes(call, owner->NodeDocumentOf(*self), holder.fragment);
    if (node == nullptr) {
      return call.ThrownValue();
    }
    if (self->Parent() != parent) {
      // The conversion moved `this` into the fragment, because a page may name
      // the node it is replacing among its replacements:
      // `child.replaceWith(x, child)` is legal and puts both back. The
      // fragment goes in front of the viable next sibling and carries `this`
      // with it; there is nothing left to replace.
      if (node->Parent() != nullptr) {
        return Value::Undefined();
      }
      return pre_insert(call, *parent, *node, reference, "replaceWith");
    }
    // "Replace this with node within parent" -- the DOM's replace algorithm,
    // which excludes the node on its way out from every "does the parent
    // already have one" question.
    if (const char* refusal = PreInsertionError(*parent, *node, self, self); refusal != nullptr) {
      return ThrowDom(call, refusal, "replaceWith would not produce a valid tree");
    }
    dom::Node* after_self = NextSiblingOf(*self);
    if (after_self == node) {
      after_self = NextSiblingOf(*node);
    }
    // The same order `replaceChild` uses, and for the same reason.
    const std::vector<dom::Node*> added = InsertedNodesOf(*node);
    if (node->Parent() != nullptr) {
      DetachFromTree(*node);
    }
    std::vector<dom::Node*> removed_nodes;
    if (self->Parent() != nullptr) {
      removed_nodes.push_back(self);
      DetachFromTree(*self, false);
    }
    insert_node(*parent, *node, after_self, false);
    RecordMutation(*parent, "childList", {}, Value::Null(), added, removed_nodes);
    return Value::Undefined();
  });

  // **`[Unscopable]`, which is what these six carry in the IDL and the reason
  // they could be added to the DOM at all.**
  //
  // An event handler content attribute runs with the element in scope, so
  // `onclick="remove()"` on a page written before `ChildNode.remove` existed
  // meant the page's own global `remove`. Adding a method to Element would
  // have silently rebound every such handler; the `@@unscopables` object is
  // how the language was taught to keep the old meaning. A page can read it,
  // and `Element.prototype[Symbol.unscopables].remove` being undefined is a
  // page finding a DOM from before 2015.
  //
  // Here rather than on Element, because this file installs the six on
  // `Node.prototype` -- so the object hangs where the methods do, and
  // `Element.prototype[Symbol.unscopables]` finds it up the chain.
  // `Symbol` is a *binding* in the global scope rather than an own property of
  // the global object -- the shape every builtin has here -- so the scope is
  // asked first and the object second.
  const Value* symbol_constructor = interpreter_->GlobalScope()->Lookup("Symbol");
  if (symbol_constructor == nullptr) {
    symbol_constructor = interpreter_->Global()->GetOwn("Symbol");
  }
  if (symbol_constructor == nullptr || !symbol_constructor->IsObject()) {
    return;
  }
  const Value* unscopables_symbol = symbol_constructor->object->GetOwn("unscopables");
  if (unscopables_symbol == nullptr || !unscopables_symbol->IsSymbol()) {
    return;
  }
  const Value unscopables = interpreter_->NewObjectValue();
  if (!unscopables.IsObject()) {
    return;
  }
  for (const char* name : {"after", "append", "before", "prepend", "remove", "replaceChildren",
                           "replaceWith"}) {
    unscopables.object->Set(name, Value::Bool(true));
  }
  wrapper.object->Set(js::PropertyKey::Symbol(unscopables_symbol->object), unscopables);
}

}  // namespace microbrowser::bindings

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <cstddef>
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
      auto made = std::make_unique<dom::Element>(element.TagName());
      for (const dom::Attribute& attribute : element.Attributes()) {
        made->SetAttribute(attribute.name, attribute.value);
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
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentType:
      // Cloning a document or a doctype is not something a page does, and
      // producing an approximation of one would be worse than refusing.
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
        if (self->IsText() || self->GetKind() == dom::Node::Kind::Comment) {
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

  const auto method = [this, &wrapper](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
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
    return owner->AdoptClone(CloneDomNode(*self, js::ToBoolean(Argument(call.arguments, 0))));
  });
  method("removeChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* child = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "removeChild requires a node");
    }
    if (child->Parent() != self) {
      // The spec's NotFoundError. A node that is not a child here is a caller
      // bug, and removing it from wherever it actually is would be worse.
      return call.Throw("TypeError", "the node to remove is not a child of this node");
    }
    const Value wrapper_for_child = owner->WrapperFor(child);
    owner->DetachFromTree(*child);
    // The removed node is returned, still usable -- a page removes a node and
    // appends it somewhere else, and that only works because it is alive.
    return wrapper_for_child;
  });
  method("remove", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner != nullptr && self != nullptr) {
      owner->DetachFromTree(*self);
    }
    return Value::Undefined();
  });
  method("insertBefore", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* child = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "insertBefore requires a node");
    }
    dom::Node* reference = NodeOf(Argument(call.arguments, 1));
    return owner->InsertNodeBefore(*self, child, reference);
  });
  method("replaceChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* fresh = NodeOf(Argument(call.arguments, 0));
    dom::Node* stale = NodeOf(Argument(call.arguments, 1));
    if (owner == nullptr || self == nullptr || fresh == nullptr || stale == nullptr) {
      return call.Throw("TypeError", "replaceChild requires two nodes");
    }
    if (stale->Parent() != self) {
      return call.Throw("TypeError", "the node to replace is not a child of this node");
    }
    // In before out, so the new node lands where the old one was rather than
    // at the end -- which is the entire difference from remove-then-append.
    const Value inserted = owner->InsertNodeBefore(*self, fresh, stale);
    if (inserted.IsObject()) {
      const Value removed = owner->WrapperFor(stale);
      owner->DetachFromTree(*stale);
      return removed;
    }
    return Value::Null();
  });

  method("appendChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* child = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "appendChild requires a node");
    }
    // A node that already has a parent is *moved*, which is how a page
    // reorders a list. That works now because detaching hands the node over
    // rather than destroying it.
    return owner->InsertNodeBefore(*self, child, nullptr);
  });

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
    const js::Value text = CreateText(js::ToString(argument));
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
  // ChildNode: swap this node out for one or more nodes in the same parent.
  method("replaceWith", [insert_argument](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "replaceWith called on a non-node");
    }
    dom::Node* parent = self->Parent();
    if (parent == nullptr) {
      return call.Throw("NotFoundError", "replaceWith on a node with no parent");
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

js::Value DomBindings::AdoptClone(std::unique_ptr<dom::Node> clone) {
  if (clone == nullptr) {
    return Value::Null();
  }
  // Owned here until something appends it, exactly like a created node: a
  // clone has no parent, and a node's owner is its parent.
  dom::Node* raw = clone.get();
  unattached_.push_back(std::move(clone));
  return WrapperFor(raw);
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
  // OwnerDocument crosses through the host, which is what makes a stamped
  // custom element in a component tree count as connected.
  if (node.OwnerDocument() != document_) {
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

js::Value DomBindings::CreateElement(const std::string& tag_name) {
  if (tag_name.empty()) {
    return Value::Null();
  }
  auto element = std::make_unique<dom::Element>(tag_name);
  dom::Element* raw = element.get();
  // Held here rather than handed to script, because a node's owner is its
  // parent and this one has none yet. Script gets the wrapper; the node stays
  // owned by C++ until something appends it.
  unattached_.push_back(std::move(element));
  const js::Value wrapper = WrapperFor(raw);
  // CSP `'strict-dynamic'`: a script created while a permitted script runs is
  // trusted before it is inserted. YouTube's player loader (`GXC`) does
  // createElement → set src → appendChild; marking only at append missed the
  // create half when the append's trust bit had already dropped (TD-0024).
  if (tag_name == "script" && InTrustedScriptContext()) {
    MarkCspTrustedScript(*raw);
  }
  // Upgraded here rather than on insertion, because the specification says a
  // custom element is constructed when it is created -- a page that does
  // `document.createElement('my-thing')` and reads a property its constructor
  // set expects it to be there before anything is appended.
  UpgradeElement(*raw);
  return wrapper;
}

js::Value DomBindings::CreateText(const std::string& text) {
  auto node = std::make_unique<dom::Text>(text);
  dom::Node* raw = node.get();
  unattached_.push_back(std::move(node));
  return WrapperFor(raw);
}

js::Value DomBindings::CreateDocumentFragment() {
  auto node = std::make_unique<dom::DocumentFragment>();
  dom::Node* raw = node.get();
  unattached_.push_back(std::move(node));
  return WrapperFor(raw);
}

js::Value DomBindings::CreateComment(const std::string& data) {
  auto node = std::make_unique<dom::Comment>(data);
  dom::Node* raw = node.get();
  unattached_.push_back(std::move(node));
  return WrapperFor(raw);
}

js::Value DomBindings::AppendTextTo(dom::Node& parent, const std::string& text) {
  auto node = std::make_unique<dom::Text>(text);
  dom::Node* raw = node.get();
  parent.Append(std::move(node));
  return WrapperFor(raw);
}

namespace {

std::string CharacterDataOf(const dom::Node* node) {
  if (node == nullptr) {
    return {};
  }
  if (node->IsText()) {
    return static_cast<const dom::Text*>(node)->Data();
  }
  if (node->GetKind() == dom::Node::Kind::Comment) {
    return static_cast<const dom::Comment*>(node)->Data();
  }
  return {};
}

}  // namespace

bool DomBindings::SetCharacterData(dom::Node* node, std::string data) {
  if (node == nullptr) {
    return false;
  }
  if (!node->IsText() && node->GetKind() != dom::Node::Kind::Comment) {
    return false;
  }
  // Polymer (and youtube's kevlar) schedules ASAP work by observing a detached
  // text node with `{characterData:true}` and bumping its `textContent`. Without
  // a characterData record that observer never fires, `_.Ub` never runs, and
  // the lazy-list autofill chain stops after the initial `shownItems` slice.
  const Value old_value = Value::String(CharacterDataOf(node));
  if (node->IsText()) {
    static_cast<dom::Text*>(node)->SetData(std::move(data));
  } else {
    static_cast<dom::Comment*>(node)->SetData(std::move(data));
  }
  RecordMutation(*node, "characterData", {}, old_value, {}, {});
  return true;
}

void DomBindings::InstallCharacterData(const js::Value& target) {
  const auto accessor = [this, &target](const char* name, js::NativeFunction get,
                                      js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  // Polymer's text bindings set `textNode.data` after stamping. Without a
  // setter the binding token stays literal in the tree -- which is why
  // youtube.com painted `[[errorMessage]]` rather than the string.
  accessor(
      "data",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        return self == nullptr ? Value::Undefined() : Value::String(CharacterDataOf(self));
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        if (!owner->SetCharacterData(self, js::ToString(Argument(call.arguments, 0)))) {
          return call.Throw("TypeError", "data can only be set on a text or comment node");
        }
        return Value::Undefined();
      });

  accessor(
      "length",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        return self == nullptr ? Value::Undefined()
                               : Value::Number(static_cast<double>(CharacterDataOf(self).size()));
      },
      [](NativeCall& call) {
        return call.Throw("TypeError", "length is read-only");
      });
}

}  // namespace microbrowser::bindings

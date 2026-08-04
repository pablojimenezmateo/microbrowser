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

namespace {

using js::NativeCall;
using js::Value;

// A copy of `node`, with its children when `deep`.
//
// Copied rather than shared: a clone is a new node, and two parents pointing
// at one node is the shape of every "it moved when I edited the copy" bug.
std::unique_ptr<dom::Node> CopyNode(const dom::Node& node, bool deep) {
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
    case dom::Node::Kind::DocumentFragment:
      // A fragment clones to an empty fragment, and `deep` fills it below --
      // which is what makes `template.content.cloneNode(true)` the way a page
      // stamps out a repeated subtree.
      copy = std::make_unique<dom::DocumentFragment>();
      break;
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentType:
      // Cloning a document or a doctype is not something a page does, and
      // producing an approximation of one would be worse than refusing.
      return nullptr;
  }
  if (!deep) {
    return copy;
  }
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    if (std::unique_ptr<dom::Node> child_copy = CopyNode(*child, true)) {
      copy->Append(std::move(child_copy));
    }
  }
  return copy;
}

}  // namespace

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

  // `textContent` both ways. Setting it drops every child and puts one text
  // node in their place, which is the cheap and safe way a page replaces the
  // contents of an element -- and the reason it could not exist until removal
  // did.
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
        owner->ClearChildren(*self);
        const std::string text = js::ToString(Argument(call.arguments, 0));
        if (!text.empty()) {
          owner->AppendTextTo(*self, text);
        }
        return Value::Undefined();
      });

  // `innerHTML` and `outerHTML`, readable only.
  //
  // Writing either means running the HTML parser on a string from script into
  // a live tree. That is not merely a bigger feature: a *fragment* parses
  // differently depending on where it is going -- `<td>` inside a table is a
  // cell and anywhere else is nothing -- and this parser parses documents. A
  // setter that ignored that would build wrong trees quietly, which is worse
  // than not having one. See ADR 0008.
  const auto read_only = [this, &wrapper](const char* name, js::NativeFunction get) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      wrapper.object->DefineAccessor(name, getter.object, nullptr);
    }
  };
  read_only("innerHTML", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return self == nullptr ? Value::Undefined() : Value::String(self->SerializeChildren());
  });
  read_only("outerHTML", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return self == nullptr ? Value::Undefined() : Value::String(self->Serialize());
  });

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
    return owner->AdoptClone(CopyNode(*self, js::ToBoolean(Argument(call.arguments, 0))));
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

}

void DomBindings::ClearChildren(dom::Node& parent) {
  // Detached rather than destroyed, one at a time from the front, for the
  // reason every removal here is: script may still hold a wrapper for any of
  // them.
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
  if (child != nullptr && child->IsDocumentFragment()) {
    while (dom::Node* first = child->FirstChild()) {
      std::unique_ptr<dom::Node> moved = child->Detach(first);
      if (moved == nullptr) {
        break;
      }
      parent.InsertBefore(std::move(moved), reference);
    }
    return WrapperFor(child);
  }
  // A node with a parent is moved rather than refused, now that detaching is
  // possible: `parent.appendChild(existing)` is how a page reorders a list,
  // and it only works because the node survives leaving its old parent.
  std::unique_ptr<dom::Node> owned;
  if (child->Parent() != nullptr) {
    owned = child->Parent()->Detach(child);
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
  return WrapperFor(child);
}

void DomBindings::NotifyConnection(dom::Node& node, bool connected) {
  const Value registry = CustomElementRegistry();
  if (!registry.IsObject() || registry.object->Keys().empty()) {
    return;  // no custom elements defined: nothing to tell, and no walk to do
  }
  bool in_document = false;
  for (const dom::Node* at = &node; at != nullptr; at = at->Parent()) {
    in_document = in_document || at == document_;
  }
  // For a connect, the node has to be in the document now. For a disconnect,
  // it has to be in it *still* -- this runs before the detach, so a node being
  // removed from a detached subtree correctly gets no reaction at all.
  if (!in_document) {
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

}  // namespace microbrowser::bindings

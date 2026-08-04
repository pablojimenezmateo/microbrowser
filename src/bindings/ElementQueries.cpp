// Searching and walking, from an element rather than from the document.
//
// `querySelector` existed only on `document`, which is half the API: a page
// that has an element and wants something inside it writes
// `container.querySelector('.row')`, and every framework does. The same was
// true of `contains`, `getElementsByTagName` and the element-only traversal
// accessors -- `firstElementChild` and its three siblings, which are what a
// walk that must skip whitespace text nodes uses.
//
// A separate translation unit because DomBindings.cpp is near its module's
// line cap, and the cap means a missing file rather than a bigger one. The
// split is by subject and not by size: everything here answers "which nodes",
// and nothing here changes the tree.

#include <functional>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// The elements under `root`, in document order, that `matches` accepts.
// `root` itself is not one of them -- a query is over descendants, which is
// the difference between `querySelector` and `matches`.
void EachDescendantElement(dom::Node& root,
                           const std::function<void(dom::Element&)>& visit) {
  for (const std::unique_ptr<dom::Node>& child : root.Children()) {
    if (child->IsElement()) {
      visit(static_cast<dom::Element&>(*child));
    }
    EachDescendantElement(*child, visit);
  }
}

// The element siblings either side of `node`, since `nextSibling` gives
// whatever is next including a text node.
dom::Node* ElementSibling(dom::Node* node, int step) {
  if (node == nullptr || node->Parent() == nullptr) {
    return nullptr;
  }
  const std::vector<std::unique_ptr<dom::Node>>& siblings = node->Parent()->Children();
  for (std::size_t i = 0; i < siblings.size(); ++i) {
    if (siblings[i].get() != node) {
      continue;
    }
    auto at = static_cast<std::ptrdiff_t>(i);
    for (;;) {
      at += step;
      if (at < 0 || at >= static_cast<std::ptrdiff_t>(siblings.size())) {
        return nullptr;
      }
      if (siblings[static_cast<std::size_t>(at)]->IsElement()) {
        return siblings[static_cast<std::size_t>(at)].get();
      }
    }
  }
  return nullptr;
}

bool IsInclusiveDescendant(const dom::Node* candidate, const dom::Node* root) {
  for (const dom::Node* at = candidate; at != nullptr; at = at->Parent()) {
    if (at == root) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Node: what every node can answer, whatever kind it is.
void DomBindings::InstallNodeQueries(const js::Value& target) {
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };

  // Inclusive, which is the specification's and the surprising half: a node
  // contains itself. A polyfill that walks up asking `root.contains(node)`
  // depends on it terminating.
  method("contains", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    dom::Node* other = NodeOf(Argument(call.arguments, 0));
    return Value::Bool(self != nullptr && other != nullptr &&
                       IsInclusiveDescendant(other, self));
  });
  method("hasChildNodes", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(self != nullptr && !self->Children().empty());
  });
  accessor("ownerDocument", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null() : owner->WrapperFor(owner->document_);
  });
  // Whether the node is in the document rather than floating: script that made
  // an element and has not appended it yet reads false, which is what a
  // framework checks before it does layout-dependent work.
  accessor("isConnected", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(owner != nullptr && self != nullptr &&
                       IsInclusiveDescendant(self, owner->document_));
  });
}

// ParentNode: searching a subtree and walking its element children. Shared by
// Element and Document, which is why it is its own function rather than part
// of either -- `document.querySelector` and `container.querySelector` are the
// same operation from different roots, and two copies would be two chances to
// disagree about what `.a` means.
void DomBindings::InstallParentQueries(const js::Value& target) {
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };

  // --- Searching within a subtree -------------------------------------------
  //
  // The same selector support the document-level versions have, and
  // deliberately the same code path: two implementations of "what does `.a`
  // mean" would be two chances to disagree.

  method("querySelector", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return Value::Null();
    }
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    dom::Element* found = nullptr;
    EachDescendantElement(*self, [&](dom::Element& element) {
      if (found == nullptr && Matches(element, selector)) {
        found = &element;
      }
    });
    return owner->WrapperFor(found);
  });
  method("querySelectorAll", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> found;
    if (owner != nullptr && self != nullptr) {
      const std::string selector = js::ToString(Argument(call.arguments, 0));
      EachDescendantElement(*self, [&](dom::Element& element) {
        if (Matches(element, selector)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("getElementsByTagName", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> found;
    if (owner != nullptr && self != nullptr) {
      const std::string wanted = LowerCase(js::ToString(Argument(call.arguments, 0)));
      EachDescendantElement(*self, [&](dom::Element& element) {
        // `*` means every element, which is what a walk over "all of them"
        // is written as.
        if (wanted == "*" || element.TagName() == wanted) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("getElementsByClassName", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> found;
    if (owner != nullptr && self != nullptr) {
      const std::string wanted = js::ToString(Argument(call.arguments, 0));
      EachDescendantElement(*self, [&](dom::Element& element) {
        if (Matches(element, "." + wanted)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });

  // --- Walking, elements only ----------------------------------------------
  //
  // The four that skip text nodes. Without them a walk over `firstChild` and
  // `nextSibling` stops on the whitespace between two tags, which is the
  // single most common way a hand-written tree walk goes wrong.

  accessor("firstElementChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return Value::Null();
    }
    for (const std::unique_ptr<dom::Node>& child : self->Children()) {
      if (child->IsElement()) {
        return owner->WrapperFor(child.get());
      }
    }
    return Value::Null();
  });
  accessor("lastElementChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return Value::Null();
    }
    const std::vector<std::unique_ptr<dom::Node>>& children = self->Children();
    for (std::size_t i = children.size(); i > 0; --i) {
      if (children[i - 1]->IsElement()) {
        return owner->WrapperFor(children[i - 1].get());
      }
    }
    return Value::Null();
  });
  accessor("nextElementSibling", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null()
                            : owner->WrapperFor(ElementSibling(NodeOf(call.self), 1));
  });
  accessor("previousElementSibling", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null()
                            : owner->WrapperFor(ElementSibling(NodeOf(call.self), -1));
  });
  // Null at the root, where `parentNode` is the document -- which is the whole
  // difference between the two and why both exist.
  accessor("parentElement", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || self->Parent() == nullptr ||
        !self->Parent()->IsElement()) {
      return Value::Null();
    }
    return owner->WrapperFor(self->Parent());
  });

}

// Element only: the things a document has no answer for. Kept apart rather
// than installed everywhere and left returning undefined, because a name that
// exists and answers nothing is what ADR 0012 refuses.
void DomBindings::InstallElementIdentity(const js::Value& target) {
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  // The tag without a namespace prefix. The same as `tagName` lowercased here,
  // because this parser produces no prefixed names -- written out because the
  // two are different in a document with XML in it, and a reader should not
  // have to guess whether that was considered.
  accessor("localName", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    return Value::String(static_cast<dom::Element*>(self)->TagName());
  });
  // A method, not an accessor: `el.hasAttributes()` is a call.
  method("hasAttributes", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(self != nullptr && self->IsElement() &&
                       !static_cast<dom::Element*>(self)->Attributes().empty());
  });
  // The attributes as `{name, value}` records, in document order. Not a live
  // NamedNodeMap: a page that mutates this array changes nothing, and a live
  // one would need a proxy per element for a surface almost nothing reads.
  accessor("attributes", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> out;
    if (self != nullptr && self->IsElement()) {
      for (const dom::Attribute& attribute : static_cast<dom::Element*>(self)->Attributes()) {
        const Value entry = call.interpreter.NewObjectValue();
        if (entry.IsObject()) {
          entry.object->Set("name", Value::String(attribute.name));
          entry.object->Set("value", Value::String(attribute.value));
          out.push_back(entry);
        }
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });
}

}  // namespace microbrowser::bindings

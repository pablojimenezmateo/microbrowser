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
#include "css/StyleSheet.h"

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
  // framework checks before it does layout-dependent work. OwnerDocument
  // crosses shadow roots; parent-walking does not, and Polymer gates enable
  // on `isConnected` inside stamped trees.
  accessor("isConnected", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(owner != nullptr && self != nullptr &&
                       self->OwnerDocument() == owner->document_);
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
    const std::vector<css::Selector> compiled = css::ParseSelectorList(selector);
    dom::Element* found = nullptr;
    EachDescendantElement(*self, [&](dom::Element& element) {
      if (found == nullptr && MatchesSelectorList(element, compiled)) {
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
      const std::vector<css::Selector> compiled = css::ParseSelectorList(selector);
      EachDescendantElement(*self, [&](dom::Element& element) {
        if (MatchesSelectorList(element, compiled)) {
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

  // ParentNode.children -- elements only, unlike Node.childNodes. Must be an
  // *own* property of Element/Document/DocumentFragment.prototype: ShadyDOM
  // (webcomponents-all-noPatch) does
  // `Object.getOwnPropertyDescriptor(Element.prototype, "children")` and
  // reinstalls it as `__shady_native_children`. An inherited descriptor from
  // Node.prototype is invisible to that capture, so Polymer.dom(node).children
  // stayed undefined and youtube's stamper crashed on `I.children[index]`.
  accessor("children", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> children;
    if (owner != nullptr && self != nullptr) {
      for (const std::unique_ptr<dom::Node>& child : self->Children()) {
        if (child->IsElement()) {
          children.push_back(owner->WrapperFor(child.get()));
        }
      }
    }
    return call.interpreter.NewArrayValue(std::move(children));
  });

  // Elements only, and a count rather than a live collection: `children.length`
  // allocates an array of wrappers for a question that is an integer. youtube's
  // Polymer path asks this of every host it stamps.
  accessor("childElementCount", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr) {
      return Value::Number(0);
    }
    double count = 0;
    for (const std::unique_ptr<dom::Node>& child : self->Children()) {
      if (child->IsElement()) {
        count += 1.0;
      }
    }
    return Value::Number(count);
  });
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
  // NamedNodeMap shape: length, item, getNamedItem, indexed Attrs, and
  // Symbol.iterator. Used to be a plain Array of `{name, value}` records --
  // enough for `attributes[0].name`, and wrong for the one call youtube's
  // property binder makes: `attributes.getNamedItem("class-name")`. That
  // threw, the custom-element reaction aborted, and the stamp stopped.
  //
  // Fresh per read, like classList: the map holds the element and re-reads
  // the attribute list on every method, so a setAttribute between two reads
  // is visible. Not a Proxy-backed live map -- almost nothing mutates through
  // the map itself, and getNamedItem/item are what pages actually call.
  accessor("attributes", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    auto& element = static_cast<dom::Element&>(*self);
    const Value map = call.interpreter.NewObjectValue();
    if (!map.IsObject()) {
      return map;
    }
    map.object->Set(kNodeSlot, PointerValue(&element));

    const auto make_attr = [](js::Interpreter& interpreter,
                              const dom::Attribute& attribute) {
      const Value entry = interpreter.NewObjectValue();
      if (entry.IsObject()) {
        entry.object->Set("name", Value::String(attribute.name));
        entry.object->Set("value", Value::String(attribute.value));
        // Attr's historical aliases. Cheap, and stops a page that probes
        // `nodeName` after getNamedItem from seeing undefined.
        entry.object->Set("nodeName", Value::String(attribute.name));
        entry.object->Set("localName", Value::String(attribute.name));
      }
      return entry;
    };

    const Value length = call.interpreter.NewNativeValue("length", [](NativeCall& inner) {
      dom::Node* node = NodeOf(inner.self);
      if (node == nullptr || !node->IsElement()) {
        return Value::Number(0);
      }
      return Value::Number(
          static_cast<double>(static_cast<dom::Element*>(node)->Attributes().size()));
    });
    if (length.IsObject()) {
      length.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->DefineAccessor("length", length.object, nullptr);
    }

    const Value item = call.interpreter.NewNativeValue("item", [make_attr](NativeCall& inner) {
      dom::Node* node = NodeOf(inner.self);
      if (node == nullptr || !node->IsElement()) {
        return Value::Null();
      }
      const auto& attributes = static_cast<dom::Element*>(node)->Attributes();
      const double index = js::ToNumber(Argument(inner.arguments, 0));
      if (!(index >= 0) || index >= static_cast<double>(attributes.size())) {
        return Value::Null();
      }
      return make_attr(inner.interpreter, attributes[static_cast<std::size_t>(index)]);
    });
    if (item.IsObject()) {
      item.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->Set("item", item);
    }

    const Value get_named =
        call.interpreter.NewNativeValue("getNamedItem", [make_attr](NativeCall& inner) {
          dom::Node* node = NodeOf(inner.self);
          if (node == nullptr || !node->IsElement()) {
            return Value::Null();
          }
          const std::string name = js::ToString(Argument(inner.arguments, 0));
          for (const dom::Attribute& attribute :
               static_cast<dom::Element*>(node)->Attributes()) {
            if (attribute.name == name) {
              return make_attr(inner.interpreter, attribute);
            }
          }
          return Value::Null();
        });
    if (get_named.IsObject()) {
      get_named.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->Set("getNamedItem", get_named);
    }

    // Indexed Attrs for `attributes[0]` and for Closure's length-based
    // fallback iterator when Symbol.iterator is missing. Snapshot of this
    // read; getNamedItem/item re-read.
    const auto& attributes = element.Attributes();
    for (std::size_t i = 0; i < attributes.size(); ++i) {
      map.object->Set(std::to_string(i), make_attr(call.interpreter, attributes[i]));
    }

    // `for (const attr of element.attributes)` and Closure's `_.A(map)`,
    // which prefers Symbol.iterator over the length fallback.
    const Value iterate =
        call.interpreter.NewNativeValue("[Symbol.iterator]", [make_attr](NativeCall& inner) {
          dom::Node* node = NodeOf(inner.self);
          std::vector<Value> out;
          if (node != nullptr && node->IsElement()) {
            for (const dom::Attribute& attribute :
                 static_cast<dom::Element*>(node)->Attributes()) {
              out.push_back(make_attr(inner.interpreter, attribute));
            }
          }
          const Value entries = inner.interpreter.NewArrayValue(std::move(out));
          if (!entries.IsObject()) {
            return Value::Undefined();
          }
          const js::Value* protocol = entries.object->Get(
              js::PropertyKey::Symbol(inner.interpreter.SymbolIterator()));
          if (protocol == nullptr) {
            return Value::Undefined();
          }
          const js::Result made = inner.interpreter.CallFunction(*protocol, entries, {});
          return made.completion == js::Completion::Throw ? Value::Undefined() : made.value;
        });
    if (iterate.IsObject()) {
      iterate.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->Set(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()), iterate);
    }
    return map;
  });

  // Attr by name, or null. Same Attr shape getNamedItem returns; the binder
  // that needed getNamedItem does not call this, but pages that probe both
  // spellings deserve one answer.
  method("getAttributeNode", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Null();
    }
    const std::string name = js::ToString(Argument(call.arguments, 0));
    for (const dom::Attribute& attribute : static_cast<dom::Element*>(self)->Attributes()) {
      if (attribute.name == name) {
        const Value entry = call.interpreter.NewObjectValue();
        if (entry.IsObject()) {
          entry.object->Set("name", Value::String(attribute.name));
          entry.object->Set("value", Value::String(attribute.value));
          entry.object->Set("nodeName", Value::String(attribute.name));
          entry.object->Set("localName", Value::String(attribute.name));
        }
        return entry;
      }
    }
    return Value::Null();
  });
}

}  // namespace microbrowser::bindings

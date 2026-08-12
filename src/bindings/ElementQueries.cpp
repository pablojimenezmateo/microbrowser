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
#include "bindings/WebIdl.h"
#include "css/StyleSheet.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// "A and B are equal": the DOM's structural comparison, recursive over
// children. Bounded by the same reasoning `dom::Node`'s destructor is -- a tree
// deep enough to overflow this is a tree deep enough to overflow being freed --
// so no explicit limit is added here that the tree does not already carry.
bool AreEqualNodes(const dom::Node& a, const dom::Node& b) {
  if (a.GetKind() != b.GetKind()) {
    return false;
  }
  switch (a.GetKind()) {
    case dom::Node::Kind::DocumentType: {
      const auto& left = static_cast<const dom::DocumentType&>(a);
      const auto& right = static_cast<const dom::DocumentType&>(b);
      if (left.Name() != right.Name() || left.PublicId() != right.PublicId() ||
          left.SystemId() != right.SystemId()) {
        return false;
      }
      break;
    }
    case dom::Node::Kind::Element: {
      const auto& left = static_cast<const dom::Element&>(a);
      const auto& right = static_cast<const dom::Element&>(b);
      if (!(left.Namespace() == right.Namespace()) || left.Prefix() != right.Prefix() ||
          left.LocalName() != right.LocalName() ||
          left.Attributes().size() != right.Attributes().size()) {
        return false;
      }
      for (const dom::Attribute& attribute : left.Attributes()) {
        const dom::Attribute* match =
            right.GetAttributeNS(attribute.name_space, attribute.LocalName());
        if (match == nullptr || match->value != attribute.value) {
          return false;
        }
      }
      break;
    }
    case dom::Node::Kind::ProcessingInstruction: {
      const auto& left = static_cast<const dom::ProcessingInstruction&>(a);
      const auto& right = static_cast<const dom::ProcessingInstruction&>(b);
      if (left.Target() != right.Target() || left.Data() != right.Data()) {
        return false;
      }
      break;
    }
    case dom::Node::Kind::Text:
      if (static_cast<const dom::Text&>(a).Data() != static_cast<const dom::Text&>(b).Data()) {
        return false;
      }
      break;
    case dom::Node::Kind::Comment:
      if (static_cast<const dom::Comment&>(a).Data() !=
          static_cast<const dom::Comment&>(b).Data()) {
        return false;
      }
      break;
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentFragment:
      break;
  }
  if (a.Children().size() != b.Children().size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.Children().size(); ++i) {
    if (!AreEqualNodes(*a.Children()[i], *b.Children()[i])) {
      return false;
    }
  }
  return true;
}

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

// One attribute, as a page sees it.
//
// Not a `Node` -- an `Attr` in the DOM is one, with a `value` that writes back
// through to the element and an identity that survives being removed and
// re-attached, and this is a record of what the attribute said when it was
// read. That gap is written down rather than hidden: `attributes[0].value = 'x'`
// sets a property on this object and does not touch the tree, and
// `el.attributes[0] === el.getAttributeNode('x')` is false where every browser
// says true. Closing it is the named remainder of task C4 in
// docs/wpt-tasks.json; it wants a per-element table of materialised Attrs and
// a detach at every attribute removal, which is a design rather than an
// addition. It is one function because it used to be two, and they disagreed --
// `getAttributeNode` answered a `localName` that was the whole qualified name.
//
// `ownerElement`, though, is an *accessor* rather than a stored value, and that
// is the one piece of the real thing that costs nothing. It re-asks the element
// whether it still carries this attribute, so a record taken before a
// `removeAttribute` answers null afterwards -- which is what the DOM says and
// what `attributes.js`'s `attributes_are` checks on every attribute it is given.
js::Value MakeAttrImpl(DomBindings& owner, js::Interpreter& interpreter, dom::Element* element,
                       const dom::Attribute& attribute) {
  const js::Value entry = interpreter.NewObjectValue();
  if (!entry.IsObject()) {
    return entry;
  }
  entry.object->Set("name", js::Value::String(attribute.name));
  entry.object->Set("value", js::Value::String(attribute.value));
  // `nodeName` is an Attr's qualified name, which is the same string as `name`.
  entry.object->Set("nodeName", js::Value::String(attribute.name));
  entry.object->Set("nodeValue", js::Value::String(attribute.value));
  // An Attr's `textContent` is its value: it is a node with no children, and
  // CharacterData's rule -- the data itself -- is the one that applies.
  entry.object->Set("textContent", js::Value::String(attribute.value));
  entry.object->Set("localName", js::Value::String(std::string(attribute.LocalName())));
  entry.object->Set("prefix", attribute.Prefix().empty()
                                  ? js::Value::Null()
                                  : js::Value::String(std::string(attribute.Prefix())));
  entry.object->Set("namespaceURI",
                    attribute.name_space.IsNone()
                        ? js::Value::Null()
                        : js::Value::String(std::string(attribute.name_space.Uri())));
  // Every attribute in this tree came from markup or from a call; there is no
  // DTD to default one, so `specified` is true for all of them -- which is what
  // the DOM now says unconditionally anyway.
  entry.object->Set("specified", js::Value::Bool(true));
  // The element is captured as a pointer and the name by value, and the getter
  // looks the pair up again on every read. Holding the wrapper instead would
  // make this object keep an element alive that the page has dropped, which is
  // the shape of every DOM leak.
  // `nodeType` is 2, which is the one thing about an Attr a page checks that is
  // not a string: `ATTRIBUTE_NODE`. It answers even though this is not a Node
  // here, because the number is what a page switches on.
  entry.object->Set("nodeType", js::Value::Number(2));
  // The element is captured as a pointer and the name by value, and the getter
  // looks the pair up again on every read. Holding the wrapper instead would
  // make this object keep an element alive that the page has dropped, which is
  // the shape of every DOM leak.
  //
  // **Null when there is no element at all**, which is what
  // `document.createAttribute` produces: an Attr exists before anything
  // carries it, and `attr.ownerElement === null` is the test for that.
  dom::Element* holder = element;
  DomBindings* bindings = &owner;
  const dom::NamespaceRef name_space = attribute.name_space;
  const std::string local(attribute.LocalName());
  const js::Value getter = interpreter.NewNativeValue(
      "ownerElement", [holder, bindings, name_space, local](js::NativeCall&) {
        if (holder == nullptr || holder->GetAttributeNS(name_space, local) == nullptr) {
          return js::Value::Null();
        }
        return bindings->WrapperFor(holder);
      });
  if (getter.IsObject()) {
    entry.object->DefineAccessor("ownerElement", getter.object, nullptr);
  }
  // The three namespace lookups every Node answers, which an Attr has to as
  // well -- and a **disconnected** one answers null and false, because there is
  // no element above it to carry an `xmlns`. They are on the record rather than
  // inherited because this object is not a Node; leaving them off made
  // `document.createAttribute('x').lookupNamespaceURI(null)` a TypeError the
  // moment `createAttribute` started answering at all, which is a regression a
  // missing method introduced by a method arriving.
  struct Lookup {
    const char* name;
    bool boolean;
  };
  for (const Lookup& lookup : {Lookup{"lookupNamespaceURI", false},
                               Lookup{"lookupPrefix", false},
                               Lookup{"isDefaultNamespace", true}}) {
    const char* name = lookup.name;
    const bool boolean = lookup.boolean;
    const js::Value native = interpreter.NewNativeValue(
        name, [name, boolean](js::NativeCall& inner) -> js::Value {
          // Delegated to the element when there is one, so an attached Attr
          // answers what its element answers rather than a second opinion.
          const js::Value* owner_element =
              inner.self.IsObject() ? inner.self.object->Get("ownerElement") : nullptr;
          if (owner_element != nullptr && owner_element->IsObject()) {
            const js::Value method =
                inner.interpreter.GetPropertyValue(*owner_element, name);
            if (method.IsObject() && method.object->IsCallable()) {
              const js::Result answered = inner.interpreter.CallFunction(
                  method, *owner_element, {Argument(inner.arguments, 0)});
              return answered.IsAbrupt() ? inner.ThrowValue(answered.value) : answered.value;
            }
          }
          return boolean ? js::Value::Bool(false) : js::Value::Null();
        });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(&owner));
      entry.object->Set(name, native);
    }
  }
  return entry;
}

js::Value MakeAttr(DomBindings& owner, js::Interpreter& interpreter, dom::Element& element,
                   const dom::Attribute& attribute) {
  return MakeAttrImpl(owner, interpreter, &element, attribute);
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

// The element a namespace lookup starts from: the node itself, its document
// element, or its parent element.
//
// The DOM states this per node type -- a Document looks at its document
// element, a DocumentType or a parentless DocumentFragment looks at nothing,
// and everything else walks up. Null means "no namespace, whatever was asked",
// which is why a lookup on a fragment answers null for every prefix.
const dom::Element* NamespaceLookupRoot(const dom::Node* node) {
  if (node == nullptr) {
    return nullptr;
  }
  switch (node->GetKind()) {
    case dom::Node::Kind::Element:
      return static_cast<const dom::Element*>(node);
    case dom::Node::Kind::Document:
      return static_cast<const dom::Document*>(node)->DocumentElement();
    case dom::Node::Kind::DocumentType:
    case dom::Node::Kind::DocumentFragment:
      return nullptr;
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
      break;
  }
  for (const dom::Node* at = node->Parent(); at != nullptr; at = at->Parent()) {
    if (at->IsElement()) {
      return static_cast<const dom::Element*>(at);
    }
  }
  return nullptr;
}

// "Locate a namespace" for `prefix`, walking up from `element`.
//
// The element's own namespace answers when its prefix is the one asked for,
// and otherwise its `xmlns` declarations do: `xmlns="…"` for the null prefix
// and `xmlns:p="…"` for the prefix `p`. An empty declaration
// (`xmlns=""`) is a real answer of *no* namespace, which is why this returns a
// `NamespaceRef` and a found flag rather than an empty string that could mean
// either.
bool LocateNamespace(const dom::Element* element, std::string_view prefix,
                     dom::NamespaceRef& found) {
  for (const dom::Element* at = element; at != nullptr;) {
    if (at->Prefix() == prefix && !at->Namespace().IsNone()) {
      found = at->Namespace();
      return true;
    }
    for (const dom::Attribute& attribute : at->Attributes()) {
      const bool declares_prefix = attribute.name_space == dom::NamespaceRef::kXmlns &&
                                   attribute.Prefix() == "xmlns" &&
                                   attribute.LocalName() == prefix;
      const bool declares_default = attribute.name_space == dom::NamespaceRef::kXmlns &&
                                    attribute.Prefix().empty() &&
                                    attribute.LocalName() == "xmlns" && prefix.empty();
      if (declares_prefix || declares_default) {
        found = dom::NamespaceRef(attribute.value);
        return true;
      }
    }
    const dom::Node* parent = at->Parent();
    at = parent != nullptr && parent->IsElement() ? static_cast<const dom::Element*>(parent)
                                                  : nullptr;
  }
  return false;
}

}  // namespace

// An `Attr` with no element behind it -- what `document.createAttribute` makes.
// Declared in BindingSupport.h so DocumentBindings.cpp can reach the one
// builder rather than growing a second one that disagrees, which is the bug
// `getAttributeNode` and `attributes[i]` had before they were merged.
js::Value MakeDetachedAttr(DomBindings& owner, js::Interpreter& interpreter,
                           const dom::Attribute& attribute) {
  return MakeAttrImpl(owner, interpreter, nullptr, attribute);
}

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

  // Null at the root, where `parentNode` is the document -- which is the whole
  // difference between the two and why both exist.
  //
  // **On Node rather than on Element**, which is where the DOM puts it and is
  // not a detail: a detached Comment answered `undefined` here, and a page
  // testing `node.parentElement === null` to decide whether a node is in a
  // tree took the wrong branch for every node that is not an element.
  accessor("parentElement", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || self->Parent() == nullptr ||
        !self->Parent()->IsElement()) {
      return Value::Null();
    }
    return owner->WrapperFor(self->Parent());
  });

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
  // Structural equality, which is a different question from identity and is the
  // only way to state "these two subtrees are the same" in a test. It is what
  // most of `domparsing/createContextualFragment.html` is written in: every
  // case there builds the tree it expects by hand and compares.
  //
  // Attributes are compared as *sets* -- same count, and every one of this
  // element's matched by namespace, local name and value on the other. Order is
  // deliberately not part of it, which is the DOM's rule and not a shortcut: the
  // order attributes were written in is observable through
  // `getAttributeNames`, and two elements that differ only in it are equal.
  method("isEqualNode", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    dom::Node* other = NodeOf(Argument(call.arguments, 0));
    return Value::Bool(self != nullptr && other != nullptr && AreEqualNodes(*self, *other));
  });
  method("isSameNode", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(self != nullptr && self == NodeOf(Argument(call.arguments, 0)));
  });

  // **`compareDocumentPosition` -- 2,805 failing subtests in `dom/ranges/`
  // alone**, which is not where anyone would look for it. `dom/common.js`
  // builds its expected answers with it, so every Range test that compares two
  // positions was failing on a *Node* method rather than on anything about
  // ranges.
  //
  // The bit set is a position rather than an ordering, and the awkward part is
  // that the specification names the *argument* node1 and the receiver node2 --
  // so `a.compareDocumentPosition(b)` returning CONTAINS means **b contains a**.
  // Reading it the other way round is the classic bug here, so the names below
  // are the specification's.
  method("compareDocumentPosition", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Node", "compareDocumentPosition", 1)) {
      return call.ThrownValue();
    }
    dom::Node* node2 = NodeOf(call.self);
    dom::Node* node1 = NodeOf(call.arguments[0]);
    if (node2 == nullptr || node1 == nullptr) {
      return call.Throw("TypeError", "compareDocumentPosition needs a Node");
    }
    constexpr double kDisconnected = 0x01;
    constexpr double kPreceding = 0x02;
    constexpr double kFollowing = 0x04;
    constexpr double kContains = 0x08;
    constexpr double kContainedBy = 0x10;
    constexpr double kImplementationSpecific = 0x20;
    if (node1 == node2) {
      return Value::Number(0);
    }
    if (RootOf(*node1) != RootOf(*node2)) {
      // "with the constraint that this is to be consistent": the pair must get
      // opposite answers from the two orders of the call, and the same answer
      // every time. Pointer order is an arbitrary but *stable* total order over
      // the two nodes, which is exactly what that constraint asks for -- and
      // the specification says implementation-specific out loud, which is what
      // the extra bit is admitting to.
      return Value::Number(kDisconnected + kImplementationSpecific +
                           (std::less<const dom::Node*>{}(node1, node2) ? kPreceding : kFollowing));
    }
    if (IsInclusiveDescendant(node2, node1)) {
      return Value::Number(kContains + kPreceding);
    }
    if (IsInclusiveDescendant(node1, node2)) {
      return Value::Number(kContainedBy + kFollowing);
    }
    // Neither contains the other, so the two are ordered by where they sit --
    // the same tree-order function every boundary point is compared with.
    return Value::Number(ComparePoints(*node1, 0, *node2, 0) < 0 ? kPreceding : kFollowing);
  });
  // The *node document*, which is stored on the node rather than derived from
  // where it happens to be: a node script created and never inserted has one,
  // and so does a node it removed. Null for a Document, which is the DOM's own
  // answer and the one thing this is not derived from the field.
  accessor("ownerDocument", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr ||
        self->GetKind() == dom::Node::Kind::Document) {
      return Value::Null();
    }
    return owner->WrapperFor(self->NodeDocument());
  });
  // Whether the node is in the document rather than floating: script that made
  // an element and has not appended it yet reads false, which is what a
  // framework checks before it does layout-dependent work. ConnectedDocument
  // crosses shadow roots; parent-walking does not, and Polymer gates enable
  // on `isConnected` inside stamped trees.
  accessor("isConnected", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    // "Its shadow-including root is a document", and *any* document -- a node
    // inside `createHTMLDocument()`'s tree is connected too. Comparing against
    // the page's own document was the same one-document assumption
    // `ownerDocument` above carried.
    return Value::Bool(owner != nullptr && self != nullptr &&
                       self->ConnectedDocument() != nullptr);
  });

  // The three namespace lookups. They are Node methods rather than Element
  // ones because the question a comment inside an XML subtree asks -- "what
  // does `p:` mean here?" -- is about where it *is*, not about what it is.
  method("lookupNamespaceURI", [](NativeCall& call) {
    const Value argument = Argument(call.arguments, 0);
    // "" is normalised to null, so `lookupNamespaceURI('')` asks about the
    // default namespace rather than about a prefix that cannot exist.
    const std::string prefix =
        argument.IsNull() || argument.IsUndefined() ? std::string() : js::ToString(argument);
    dom::NamespaceRef found;
    if (!LocateNamespace(NamespaceLookupRoot(NodeOf(call.self)), prefix, found) ||
        found.IsNone()) {
      return Value::Null();
    }
    return Value::String(std::string(found.Uri()));
  });
  method("isDefaultNamespace", [](NativeCall& call) {
    const Value argument = Argument(call.arguments, 0);
    const dom::NamespaceRef wanted(
        argument.IsNull() || argument.IsUndefined() ? std::string() : js::ToString(argument));
    dom::NamespaceRef found;
    (void)LocateNamespace(NamespaceLookupRoot(NodeOf(call.self)), "", found);
    return Value::Bool(found == wanted);
  });
  // The mirror: which prefix, here, means this namespace. Null for the default
  // one, because "" is not a prefix -- an element in the default namespace has
  // no prefix at all, and answering "" would be a name a page could paste into
  // a qualified name and produce `:local`.
  method("lookupPrefix", [](NativeCall& call) {
    const Value argument = Argument(call.arguments, 0);
    if (argument.IsNull() || argument.IsUndefined()) {
      return Value::Null();
    }
    const dom::NamespaceRef wanted(js::ToString(argument));
    if (wanted.IsNone()) {
      return Value::Null();
    }
    for (const dom::Element* at = NamespaceLookupRoot(NodeOf(call.self)); at != nullptr;) {
      if (at->Namespace() == wanted && !at->Prefix().empty()) {
        return Value::String(std::string(at->Prefix()));
      }
      for (const dom::Attribute& attribute : at->Attributes()) {
        if (attribute.name_space == dom::NamespaceRef::kXmlns &&
            attribute.Prefix() == "xmlns" && dom::NamespaceRef(attribute.value) == wanted) {
          return Value::String(std::string(attribute.LocalName()));
        }
      }
      const dom::Node* parent = at->Parent();
      at = parent != nullptr && parent->IsElement()
               ? static_cast<const dom::Element*>(parent)
               : nullptr;
    }
    return Value::Null();
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

  // `moveBefore`, the atomic move, in TreeMutation.cpp -- which is where
  // every other thing that changes the tree lives, and where the insertion it
  // is written in terms of already is.
  InstallAtomicMove(target);

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
      // `*` means every element, which is what a walk over "all of them" is
      // written as. Everything else goes through MatchesTagName, where the
      // HTML-element-matches-case-insensitively rule lives.
      const std::string wanted = js::ToString(Argument(call.arguments, 0));
      const std::string lowered = LowerCase(wanted);
      EachDescendantElement(*self, [&](dom::Element& element) {
        if (MatchesTagName(element, wanted, lowered)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("getElementsByTagNameNS", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> found;
    if (owner != nullptr && self != nullptr) {
      const NamespaceQuery wanted(Argument(call.arguments, 0), Argument(call.arguments, 1));
      EachDescendantElement(*self, [&](dom::Element& element) {
        if (wanted.Matches(element)) {
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
  // The three halves of an element's name that `tagName` is not. They are read
  // off the element rather than derived from the tag name, because deriving is
  // exactly the bug: `<xml:lang>` in an HTML document is one element whose
  // whole local name contains a colon, and a `prefix` found by looking for one
  // would answer `xml` about an element that has no prefix at all.
  accessor("localName", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    return Value::String(std::string(static_cast<dom::Element*>(self)->LocalName()));
  });
  accessor("prefix", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const std::string_view prefix = static_cast<dom::Element*>(self)->Prefix();
    // Null rather than "" for no prefix: a page writes `if (el.prefix)`, and
    // an empty string would answer the same -- but `assert_equals(el.prefix,
    // null)` is what the DOM says and what every other engine reports.
    return prefix.empty() ? Value::Null() : Value::String(std::string(prefix));
  });
  accessor("namespaceURI", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const dom::NamespaceRef& name_space = static_cast<dom::Element*>(self)->Namespace();
    return name_space.IsNone() ? Value::Null() : Value::String(std::string(name_space.Uri()));
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

    const Value item = call.interpreter.NewNativeValue("item", [](NativeCall& inner) {
      DomBindings* holder = OwnerOf(inner);
      dom::Node* node = NodeOf(inner.self);
      if (holder == nullptr || node == nullptr || !node->IsElement()) {
        return Value::Null();
      }
      auto& owning = static_cast<dom::Element&>(*node);
      const auto& attributes = owning.Attributes();
      const double index = js::ToNumber(Argument(inner.arguments, 0));
      if (!(index >= 0) || index >= static_cast<double>(attributes.size())) {
        return Value::Null();
      }
      return MakeAttr(*holder, inner.interpreter, owning,
                      attributes[static_cast<std::size_t>(index)]);
    });
    if (item.IsObject()) {
      item.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->Set("item", item);
    }

    const Value get_named =
        call.interpreter.NewNativeValue("getNamedItem", [](NativeCall& inner) {
          DomBindings* holder = OwnerOf(inner);
          dom::Node* node = NodeOf(inner.self);
          if (holder == nullptr || node == nullptr || !node->IsElement()) {
            return Value::Null();
          }
          auto& named = static_cast<dom::Element&>(*node);
          const std::string name =
              AttributeNameFor(named, js::ToString(Argument(inner.arguments, 0)));
          for (const dom::Attribute& attribute : named.Attributes()) {
            if (attribute.name == name) {
              return MakeAttr(*holder, inner.interpreter, named, attribute);
            }
          }
          return Value::Null();
        });
    if (get_named.IsObject()) {
      get_named.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->Set("getNamedItem", get_named);
    }

    const Value get_named_ns =
        call.interpreter.NewNativeValue("getNamedItemNS", [](NativeCall& inner) -> Value {
          DomBindings* holder = OwnerOf(inner);
          dom::Node* node = NodeOf(inner.self);
          if (holder == nullptr || node == nullptr || !node->IsElement()) {
            return Value::Null();
          }
          dom::NamespaceRef name_space;
          std::string local;
          if (!ToNamespaceAndLocalName(inner, Argument(inner.arguments, 0),
                                       Argument(inner.arguments, 1), name_space, local)) {
            return inner.ThrownValue();
          }
          auto& owning = static_cast<dom::Element&>(*node);
          const dom::Attribute* found = owning.GetAttributeNS(name_space, local);
          return found == nullptr ? Value::Null()
                                  : MakeAttr(*holder, inner.interpreter, owning, *found);
        });
    if (get_named_ns.IsObject()) {
      get_named_ns.object->Set(kOwnerSlot, PointerValue(owner));
      map.object->Set("getNamedItemNS", get_named_ns);
    }

    // Indexed Attrs for `attributes[0]` and for Closure's length-based
    // fallback iterator when Symbol.iterator is missing. Snapshot of this
    // read; getNamedItem/item re-read.
    const auto& attributes = element.Attributes();
    for (std::size_t i = 0; i < attributes.size(); ++i) {
      map.object->Set(std::to_string(i), MakeAttr(*owner, call.interpreter, element, attributes[i]));
    }

    // `for (const attr of element.attributes)` and Closure's `_.A(map)`,
    // which prefers Symbol.iterator over the length fallback.
    const Value iterate =
        call.interpreter.NewNativeValue("[Symbol.iterator]", [](NativeCall& inner) {
          DomBindings* holder = OwnerOf(inner);
          dom::Node* node = NodeOf(inner.self);
          std::vector<Value> out;
          if (holder != nullptr && node != nullptr && node->IsElement()) {
            auto& owning = static_cast<dom::Element&>(*node);
            for (const dom::Attribute& attribute : owning.Attributes()) {
              out.push_back(MakeAttr(*holder, inner.interpreter, owning, attribute));
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
    DomBindings* holder = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (holder == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Null();
    }
    auto& element = static_cast<dom::Element&>(*self);
    const std::string name =
        AttributeNameFor(element, js::ToString(Argument(call.arguments, 0)));
    for (const dom::Attribute& attribute : element.Attributes()) {
      if (attribute.name == name) {
        return MakeAttr(*holder, call.interpreter, element, attribute);
      }
    }
    return Value::Null();
  });
  method("getAttributeNodeNS", [](NativeCall& call) {
    DomBindings* holder = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (holder == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Null();
    }
    dom::NamespaceRef name_space;
    std::string local;
    if (!ToNamespaceAndLocalName(call, Argument(call.arguments, 0),
                                 Argument(call.arguments, 1), name_space, local)) {
      return call.ThrownValue();
    }
    auto& element = static_cast<dom::Element&>(*self);
    const dom::Attribute* found = element.GetAttributeNS(name_space, local);
    return found == nullptr ? Value::Null()
                            : MakeAttr(*holder, call.interpreter, element, *found);
  });
}

}  // namespace microbrowser::bindings

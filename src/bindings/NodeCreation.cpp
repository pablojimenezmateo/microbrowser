// The node-*creation* half of the DOM bindings: everything that makes a node,
// and everything that owns one before a parent does.
//
// Split out of TreeMutation.cpp, which is about moving nodes that already
// exist. The two are different jobs and the file had grown past the module's
// translation-unit cap holding both.
//
// The one rule this file exists to keep in one place: **every node gets a node
// document the moment it is made**, and `node_document` is a parameter with no
// default anywhere here. A default would silently name the page's own
// document, which is exactly the answer `document.implementation
// .createHTMLDocument()` exists to make wrong -- and `ownerDocument` is asked
// of nodes that have never been inserted, so there is no tree to derive it
// from later.

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/WebIdl.h"

#include <memory>
#include <string>
#include <utility>

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

js::Value DomBindings::AdoptUnattached(std::unique_ptr<dom::Node> node,
                                       dom::Document& node_document) {
  if (node == nullptr) {
    return Value::Null();
  }
  dom::Node* raw = node.get();
  // The node document is set before anything can see the node, because
  // `ownerDocument` is asked of a node that has never been inserted and there
  // is no tree to derive it from.
  raw->SetNodeDocument(&node_document);
  // Held here rather than handed to script, because a node's owner is its
  // parent and this one has none yet. Script gets the wrapper; the node stays
  // owned by C++ until something appends it.
  unattached_.push_back(std::move(node));
  return WrapperFor(raw);
}

js::Value DomBindings::CreateElement(const std::string& tag_name,
                                     dom::Document& node_document) {
  // `createElement` in an HTML document makes an HTML element with no prefix,
  // which is the only kind this parser produces too.
  QualifiedName name;
  name.name_space = dom::NamespaceRef::kHtml;
  name.qualified = tag_name;
  return CreateElementNS(std::move(name), node_document);
}

js::Value DomBindings::CreateElementNS(QualifiedName name, dom::Document& node_document) {
  const std::string tag_name = name.qualified;
  if (tag_name.empty()) {
    return Value::Null();
  }
  auto element = std::make_unique<dom::Element>(std::move(name.name_space),
                                                std::move(name.qualified), name.prefix_length);
  dom::Element* raw = element.get();
  const js::Value wrapper = AdoptUnattached(std::move(element), node_document);
  // CSP `'strict-dynamic'`: a script created while a permitted script runs is
  // trusted before it is inserted. YouTube's player loader (`GXC`) does
  // createElement → set src → appendChild; marking only at append missed the
  // create half when the append's trust bit had already dropped (TD-0024).
  if (tag_name == "script" && raw->Namespace().IsHtml() && InTrustedScriptContext()) {
    MarkCspTrustedScript(*raw);
  }
  // Upgraded here rather than on insertion, because the specification says a
  // custom element is constructed when it is created -- a page that does
  // `document.createElement('my-thing')` and reads a property its constructor
  // set expects it to be there before anything is appended.
  //
  // HTML namespace only: a custom element is an HTML element, and
  // `createElementNS('http://FOO', 'my-thing')` is a foreign element that
  // happens to have a hyphen in its name.
  if (raw->Namespace().IsHtml()) {
    UpgradeElement(*raw);
  }
  return wrapper;
}

js::Value DomBindings::CreateText(const std::string& text, dom::Document& node_document) {
  return AdoptUnattached(std::make_unique<dom::Text>(text), node_document);
}

js::Value DomBindings::CreateDocumentFragment(dom::Document& node_document) {
  return AdoptUnattached(std::make_unique<dom::DocumentFragment>(), node_document);
}

js::Value DomBindings::CreateComment(const std::string& data, dom::Document& node_document) {
  return AdoptUnattached(std::make_unique<dom::Comment>(data), node_document);
}

js::Value DomBindings::AppendTextTo(dom::Node& parent, const std::string& text) {
  auto node = std::make_unique<dom::Text>(text);
  dom::Node* raw = node.get();
  parent.Append(std::move(node));
  return WrapperFor(raw);
}

bool IsCharacterDataNode(const dom::Node& node) {
  switch (node.GetKind()) {
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
      return true;
    case dom::Node::Kind::Element:
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentType:
    case dom::Node::Kind::DocumentFragment:
      return false;
  }
  return false;
}

std::string CharacterDataOf(const dom::Node* node) {
  if (node == nullptr) {
    return {};
  }
  switch (node->GetKind()) {
    case dom::Node::Kind::Text:
      return static_cast<const dom::Text*>(node)->Data();
    case dom::Node::Kind::Comment:
      return static_cast<const dom::Comment*>(node)->Data();
    case dom::Node::Kind::ProcessingInstruction:
      return static_cast<const dom::ProcessingInstruction*>(node)->Data();
    default:
      return {};
  }
}

bool DomBindings::SetCharacterData(dom::Node* node, std::string data) {
  if (node == nullptr) {
    return false;
  }
  if (!IsCharacterDataNode(*node)) {
    return false;
  }
  // Polymer (and youtube's kevlar) schedules ASAP work by observing a detached
  // text node with `{characterData:true}` and bumping its `textContent`. Without
  // a characterData record that observer never fires, `_.Ub` never runs, and
  // the lazy-list autofill chain stops after the initial `shownItems` slice.
  const Value old_value = Value::String(CharacterDataOf(node));
  if (node->IsText()) {
    static_cast<dom::Text*>(node)->SetData(std::move(data));
  } else if (node->GetKind() == dom::Node::Kind::Comment) {
    static_cast<dom::Comment*>(node)->SetData(std::move(data));
  } else {
    static_cast<dom::ProcessingInstruction*>(node)->SetData(std::move(data));
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

  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->SetHidden(name, native);
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
      [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        std::string data;
        if (!ToDomString(call, Argument(call.arguments, 0), data)) {
          return call.ThrownValue();
        }
        if (!owner->SetCharacterData(self, std::move(data))) {
          return call.Throw("TypeError", "data can only be set on a text or comment node");
        }
        return Value::Undefined();
      });

  // **Code units, not bytes.** `length` is the number of UTF-16 code units in
  // the data, because that is what a DOMString is; measuring `std::string::size`
  // made every non-ASCII text node report its byte count, so `"é".length` was 2
  // and every offset computed from it addressed the wrong character. The same
  // measurement runs through all five mutation methods below -- an offset is an
  // offset in the same units `length` counts, or none of them agree.
  accessor(
      "length",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        return self == nullptr
                   ? Value::Undefined()
                   : Value::Number(static_cast<double>(DomStringLength(CharacterDataOf(self))));
      },
      [](NativeCall& call) { return call.Throw("TypeError", "length is read-only"); });

  // The five mutation operations, which were simply absent -- `data` and
  // `length` were the whole of CharacterData here. Each is "replace data" with
  // different arguments, and they are written that way rather than five times:
  // the specification defines the other four in terms of it, and the ordering
  // that matters (bounds check, then the write, then the record) is in one
  // place.
  //
  // `offset` and `count` are `unsigned long`, so a negative argument wraps to
  // an enormous one and lands in the IndexSizeError below rather than clamping
  // to zero. `substringData(-1, 0)` throwing is not pedantry -- it is the
  // difference between a page's bounds check running and it silently reading
  // from position 4294967295.
  const auto replace_data = [](NativeCall& call, const char* operation,
                               std::uint32_t offset, std::uint32_t count,
                               const std::string& insertion) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", std::string(operation) + " called on a non-CharacterData");
    }
    const std::string data = CharacterDataOf(self);
    const std::size_t length = DomStringLength(data);
    if (offset > length) {
      return ThrowDom(call, "IndexSizeError", "the offset is larger than the node's length");
    }
    const std::size_t end = std::min<std::size_t>(length, static_cast<std::size_t>(offset) + count);
    std::string rewritten = DomSubstring(data, 0, offset);
    rewritten += insertion;
    rewritten += DomSubstring(data, end, length);
    if (!owner->SetCharacterData(self, std::move(rewritten))) {
      return call.Throw("TypeError", std::string(operation) + " called on a non-CharacterData");
    }
    return Value::Undefined();
  };

  method("substringData", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "CharacterData", "substringData", 2)) {
      return call.ThrownValue();
    }
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    if (!ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, offset) ||
        !ToUnsignedLong(call, call.arguments[1], IntegerRange::Modulo, count)) {
      return call.ThrownValue();
    }
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr) {
      return call.Throw("TypeError", "substringData called on a non-CharacterData");
    }
    const std::string data = CharacterDataOf(self);
    const std::size_t length = DomStringLength(data);
    if (offset > length) {
      return ThrowDom(call, "IndexSizeError", "the offset is larger than the node's length");
    }
    const std::size_t end = std::min<std::size_t>(length, static_cast<std::size_t>(offset) + count);
    return Value::String(DomSubstring(data, offset, end));
  });

  method("appendData", [replace_data](NativeCall& call) -> Value {
    if (!RequireArguments(call, "CharacterData", "appendData", 1)) {
      return call.ThrownValue();
    }
    std::string insertion;
    if (!ToDomString(call, call.arguments[0], insertion)) {
      return call.ThrownValue();
    }
    dom::Node* self = NodeOf(call.self);
    const std::size_t length = self == nullptr ? 0 : DomStringLength(CharacterDataOf(self));
    return replace_data(call, "appendData", static_cast<std::uint32_t>(length), 0, insertion);
  });

  method("insertData", [replace_data](NativeCall& call) -> Value {
    if (!RequireArguments(call, "CharacterData", "insertData", 2)) {
      return call.ThrownValue();
    }
    std::uint32_t offset = 0;
    std::string insertion;
    if (!ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, offset) ||
        !ToDomString(call, call.arguments[1], insertion)) {
      return call.ThrownValue();
    }
    return replace_data(call, "insertData", offset, 0, insertion);
  });

  method("deleteData", [replace_data](NativeCall& call) -> Value {
    if (!RequireArguments(call, "CharacterData", "deleteData", 2)) {
      return call.ThrownValue();
    }
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    if (!ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, offset) ||
        !ToUnsignedLong(call, call.arguments[1], IntegerRange::Modulo, count)) {
      return call.ThrownValue();
    }
    return replace_data(call, "deleteData", offset, count, std::string());
  });

  method("replaceData", [replace_data](NativeCall& call) -> Value {
    if (!RequireArguments(call, "CharacterData", "replaceData", 3)) {
      return call.ThrownValue();
    }
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
    std::string insertion;
    if (!ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, offset) ||
        !ToUnsignedLong(call, call.arguments[1], IntegerRange::Modulo, count) ||
        !ToDomString(call, call.arguments[2], insertion)) {
      return call.ThrownValue();
    }
    return replace_data(call, "replaceData", offset, count, insertion);
  });
}

}  // namespace microbrowser::bindings

// The DOM's type hierarchy, as prototypes a page can name.
//
// Every wrapper used to carry its own copy of every method and accessor. That
// cost one native function object per property per node, and -- the reason
// this exists -- it made `instanceof` unanswerable, because there was no
// shared object for a chain to lead to. `document.createElement('div')`
// produced something with the right properties and nothing to be an instance
// *of*, so `el instanceof HTMLElement` was false and `class X extends
// HTMLElement` could not be written at all. That last one is not an edge case:
// it is how every custom element is declared, and it is the error youtube.com
// stops on.
//
// The chain is the specification's, one link per name a page can reach:
//
//   Node <- Element <- HTMLElement <- HTMLDivElement, HTMLAnchorElement, ...
//   Node <- Text
//   Node <- Document
//
// Per-tag interfaces are built on demand rather than up front. A page that
// only ever makes a `<div>` should pay for one, and the list of tags with
// their own interface in the real DOM is over a hundred long.
//
// ADR 0012 is why this is first: it is structural rather than additive, and
// everything after it assumes an element already has a prototype to hang from.

#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::Value;

namespace {

// Which tag gets which interface.
//
// Deliberately not the full table. These are the tags whose interface a page
// is likely to *name* -- in an `instanceof`, or as the base of a custom
// element -- rather than every tag that has one in the specification. An
// unlisted tag gets HTMLElement, which is the right answer for it and not a
// fallback.
struct TagInterface {
  std::string_view tag;
  const char* interface;
};

constexpr TagInterface kTagInterfaces[] = {
    {"div", "HTMLDivElement"},           {"span", "HTMLSpanElement"},
    {"a", "HTMLAnchorElement"},          {"img", "HTMLImageElement"},
    {"input", "HTMLInputElement"},       {"button", "HTMLButtonElement"},
    {"select", "HTMLSelectElement"},     {"option", "HTMLOptionElement"},
    {"textarea", "HTMLTextAreaElement"}, {"form", "HTMLFormElement"},
    {"script", "HTMLScriptElement"},     {"style", "HTMLStyleElement"},
    {"link", "HTMLLinkElement"},         {"table", "HTMLTableElement"},
    {"tr", "HTMLTableRowElement"},       {"td", "HTMLTableCellElement"},
    {"th", "HTMLTableCellElement"},      {"ul", "HTMLUListElement"},
    {"ol", "HTMLOListElement"},          {"li", "HTMLLIElement"},
    {"p", "HTMLParagraphElement"},       {"h1", "HTMLHeadingElement"},
    {"h2", "HTMLHeadingElement"},        {"h3", "HTMLHeadingElement"},
    {"h4", "HTMLHeadingElement"},        {"h5", "HTMLHeadingElement"},
    {"h6", "HTMLHeadingElement"},        {"canvas", "HTMLCanvasElement"},
    {"video", "HTMLVideoElement"},       {"audio", "HTMLAudioElement"},
    {"iframe", "HTMLIFrameElement"},     {"template", "HTMLTemplateElement"},
};

const char* InterfaceForTag(std::string_view tag) {
  for (const TagInterface& entry : kTagInterfaces) {
    if (entry.tag == tag) {
      return entry.interface;
    }
  }
  return "";
}

}  // namespace

js::Value DomBindings::MakeInterface(const char* name, const js::Value& parent) {
  if (const Value* existing = interfaces_.object->GetOwn(name)) {
    return *existing;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return Value::Undefined();
  }
  if (parent.IsObject()) {
    prototype.object->SetPrototype(parent.object);
  }

  // The constructor exists so that the *name* resolves and `instanceof` has
  // something to ask about. Calling one directly is a TypeError in a browser
  // too -- `new HTMLDivElement()` is not how an element is made -- so throwing
  // is the honest implementation rather than a limitation.
  //
  // It is still a function, which is what lets `class X extends HTMLElement`
  // be written. What that class cannot yet do is be *registered*: a custom
  // element needs `customElements.define` and the upgrade lifecycle, which is
  // a later item in ADR 0012.
  const std::string message = std::string("Illegal constructor: ") + name;
  const Value constructor =
      interpreter_->NewNativeValue(name, [message](js::NativeCall& call) {
        return call.Throw("TypeError", message);
      });
  if (!constructor.IsObject()) {
    return Value::Undefined();
  }
  constructor.object->Set("prototype", prototype);
  prototype.object->Set("constructor", constructor);
  interpreter_->Global()->Set(name, constructor);
  // Also a binding in the global *scope*, so a bare `HTMLElement` resolves the
  // way `globalThis.HTMLElement` does -- one namespace rather than two that
  // happen to overlap, which is the rule the engine's own globals follow.
  interpreter_->GlobalScope()->Declare(name, constructor, false);
  interfaces_.object->Set(name, prototype);
  return prototype;
}

void DomBindings::EnsureInterfaces() {
  if (interfaces_.IsObject()) {
    return;
  }
  interfaces_ = interpreter_->NewObjectValue();
  if (!interfaces_.IsObject()) {
    return;
  }
  // Rooted through the global for the same reason the wrapper cache is: a
  // `js::Value` in a C++ field is invisible to the collector, and a prototype
  // that is collected leaves every element that inherits from it pointing at
  // reclaimed memory.
  interpreter_->Global()->Set("#domInterfaces", interfaces_);

  const Value node = MakeInterface("Node", Value::Undefined());
  InstallNodeInterface(node);
  const Value element = MakeInterface("Element", node);
  InstallElementInterface(element);
  const Value html_element = MakeInterface("HTMLElement", element);
  // Every per-tag interface, up front rather than when its tag is first seen.
  // Lazily was tempting and wrong: `x instanceof HTMLAnchorElement` has to
  // answer *false* on a page with no anchor in it, and a name that does not
  // exist until the tag does throws a ReferenceError instead. A page tests for
  // a type before it has one far more often than after.
  for (const TagInterface& entry : kTagInterfaces) {
    MakeInterface(entry.interface, html_element);
  }
  MakeInterface("Text", node);
  MakeInterface("Comment", node);
  MakeInterface("Document", node);
}

js::Value DomBindings::PrototypeFor(const dom::Node& node) {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  const auto named = [this](const char* name) -> Value {
    const Value* found = interfaces_.object->GetOwn(name);
    return found == nullptr ? Value::Undefined() : *found;
  };

  switch (node.GetKind()) {
    case dom::Node::Kind::Text:
      return named("Text");
    case dom::Node::Kind::Comment:
      return named("Comment");
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentType:
      return named("Document");
    case dom::Node::Kind::Element:
      break;
  }

  const char* interface = InterfaceForTag(static_cast<const dom::Element&>(node).TagName());
  return *interface == '\0' ? named("HTMLElement") : named(interface);
}

}  // namespace microbrowser::bindings

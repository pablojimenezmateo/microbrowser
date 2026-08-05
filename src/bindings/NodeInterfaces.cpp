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
//   Node <- CharacterData <- Text, Comment
//   Node <- Document, DocumentFragment
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
  DomBindings* self = this;
  const Value constructor =
      interpreter_->NewNativeValue(name, [message, self](js::NativeCall& call) {
        // The one case where calling an interface is legal: `super()` inside a
        // custom element's constructor, during an upgrade. Returning the
        // element the document already has is what makes the class run *on*
        // it -- a derived class's `this` is whatever its base produced. See
        // CustomElements.cpp.
        const Value pending = self->PendingUpgrade();
        if (pending.IsObject()) {
          return pending;
        }
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
  InstallNodeQueries(node);
  const Value element = MakeInterface("Element", node);
  InstallElementInterface(element);
  InstallParentQueries(element);
  InstallElementIdentity(element);
  InstallGeometry(element);
  const Value html_element = MakeInterface("HTMLElement", element);
  // On HTMLElement rather than Element, which is where the specification puts
  // them: focus is an HTML concept, and an SVG element in this tree is an
  // Element with no HTML semantics at all.
  InstallFocus(html_element);
  // Every per-tag interface, up front rather than when its tag is first seen.
  // Lazily was tempting and wrong: `x instanceof HTMLAnchorElement` has to
  // answer *false* on a page with no anchor in it, and a name that does not
  // exist until the tag does throws a ReferenceError instead. A page tests for
  // a type before it has one far more often than after.
  for (const TagInterface& entry : kTagInterfaces) {
    MakeInterface(entry.interface, html_element);
  }
  // Text and Comment share a base, and it is not decoration: a polyfill that
  // patches `data` or `length` patches CharacterData once rather than both.
  const Value character_data = MakeInterface("CharacterData", node);
  MakeInterface("Text", character_data);
  MakeInterface("Comment", character_data);
  // A fragment is a ParentNode: script queries the subtree it is building
  // before it inserts it, which is most of the reason to build it detached.
  InstallParentQueries(MakeInterface("DocumentFragment", node));
  // A Document is a ParentNode too: `document.querySelector` and
  // `container.querySelector` are one operation from two roots.
  InstallParentQueries(MakeInterface("Document", node));
  // `new Image()` is `document.createElement('img')` with a nicer spelling,
  // and `new Image(w, h)` sets the two attributes. Honest to have: it is the
  // element's constructor and nothing more. That a *detached* image does not
  // fetch is the synchronous-loading gap in ADR 0011, which an `<img>` added
  // by script after the load has equally -- not something this introduces.
  DomBindings* self = this;
  const Value image = interpreter_->NewNativeValue("Image", [self](js::NativeCall& call) {
    const Value made = self->CreateElement("img");
    if (made.IsObject() && !call.arguments.empty()) {
      if (dom::Node* made_node = NodeOf(made)) {
        auto& img = static_cast<dom::Element&>(*made_node);
        img.SetAttribute("width", js::ToString(call.arguments[0]));
        if (call.arguments.size() > 1) {
          img.SetAttribute("height", js::ToString(call.arguments[1]));
        }
      }
    }
    return made;
  });
  if (image.IsObject()) {
    // Its prototype is HTMLImageElement's, so `new Image() instanceof
    // HTMLImageElement` is true -- which is what a page checks.
    if (const Value* prototype = interfaces_.object->GetOwn("HTMLImageElement")) {
      image.object->Set("prototype", *prototype);
    }
    interpreter_->Global()->Set("Image", image);
    interpreter_->GlobalScope()->Declare("Image", image, false);
  }

  // After every interface exists, because a reflected property lands on the
  // prototype of the tag it belongs to.
  InstallReflections();

  InstallFormApis();
  InstallCustomElements();
  InstallMutationObserver();
  if (geometry_ != nullptr) {
    // Absent, not stubbed, when nothing can answer a geometry question. An
    // IntersectionObserver that exists and never fires is what sends a feed
    // down the native path into a wall; a missing name sends it to a polyfill
    // that works. ADR 0012, and the same rule getBoundingClientRect follows.
    InstallViewObservers();
  }
  // Absent for the same reason when there is no loader behind this layer, and
  // it is the place that rule matters most: a page that finds `fetch` and gets
  // a rejection has no fallback path left. InstallFetch answers that itself.
  InstallFetch();
  InstallWindowEvents();
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
    case dom::Node::Kind::DocumentFragment:
      return named("DocumentFragment");
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

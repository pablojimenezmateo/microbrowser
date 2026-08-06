// `document` itself: the methods and properties that hang off it rather than
// off a node.
//
// Split out because DomBindings.cpp reached the module's line cap, and the cap
// is written to mean a missing translation unit rather than a bigger file. The
// seam is a real one: everything here is *the document as an object* -- what a
// page reaches through the `document` global -- while the rest of the module
// is about nodes in general.

#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "css/StyleSheet.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

void DomBindings::Install() {
  const Value document = WrapperFor(document_);
  if (!document.IsObject()) {
    return;
  }
  // Everything below goes on `Document.prototype` rather than on this one
  // wrapper -- the same move Node and Element made, and for a second reason
  // here: `document.implementation.createHTMLDocument()` makes a *real* second
  // document, and a method that only existed as an own property of the page's
  // own wrapper would be missing from it entirely.
  //
  // The wrapper itself when there is no interface object, which is the
  // out-of-memory path and not a mode.
  const Value document_interface = DocumentInterface();
  const Value& target = document_interface.IsObject() ? document_interface : document;

  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };

  method("getElementById", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    return owner->WrapperFor(
        FindElementIn(*owner->DocumentOf(call.self), [&wanted](const dom::Element& element) {
          const std::string* id = element.GetAttribute("id");
          return id != nullptr && *id == wanted;
        }));
  });
  method("getElementsByTagName", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const std::string wanted = LowerCase(js::ToString(Argument(call.arguments, 0)));
    std::vector<Value> found;
    if (owner != nullptr) {
      ForEachElementIn(*owner->DocumentOf(call.self), [&](dom::Element& element) {
        if (wanted == "*" || element.TagName() == wanted) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("querySelector", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    const std::vector<css::Selector> compiled = css::ParseSelectorList(selector);
    return owner->WrapperFor(FindElementIn(
        *owner->DocumentOf(call.self), [&compiled](const dom::Element& element) {
          return MatchesSelectorList(element, compiled);
        }));
  });
  method("querySelectorAll", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    const std::vector<css::Selector> compiled = css::ParseSelectorList(selector);
    std::vector<Value> found;
    if (owner != nullptr) {
      ForEachElementIn(*owner->DocumentOf(call.self), [&](dom::Element& element) {
        if (MatchesSelectorList(element, compiled)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    // An array, not a NodeList. A page indexes it, takes its length and
    // spreads it, and all three work -- what it does not get is the live
    // collection a NodeList is, which nothing here could keep up to date
    // anyway.
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("getElementsByClassName", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const std::string selector = "." + js::ToString(Argument(call.arguments, 0));
    const std::vector<css::Selector> compiled = css::ParseSelectorList(selector);
    std::vector<Value> found;
    if (owner != nullptr) {
      ForEachElementIn(*owner->DocumentOf(call.self), [&](dom::Element& element) {
        if (MatchesSelectorList(element, compiled)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("createTextNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null()
                            : owner->CreateText(js::ToString(Argument(call.arguments, 0)));
  });
  method("createElement", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateElement(LowerCase(js::ToString(Argument(call.arguments, 0))));
  });
  // The namespace is accepted and ignored, which is honest for this parser:
  // it produces one tree with no XML in it, so `createElementNS(HTML_NS, 'div')`
  // and `createElement('div')` describe the same element. A page that asks for
  // a genuinely foreign namespace gets an HTML element and not a wrong answer
  // about one -- SVG is rendered from its own decoder, not from the DOM.
  method("createElementNS", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateElement(LowerCase(js::ToString(Argument(call.arguments, 1))));
  });
  method("createDocumentFragment", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null() : owner->CreateDocumentFragment();
  });
  // `importNode` and `adoptNode` -- the two ways a node crosses documents.
  //
  // Polymer stamps every custom-element template with
  // `shadowRoot.appendChild(document.importNode(template.content, true))`.
  // Without `importNode` that call throws, the template never arrives, and a
  // page of upgraded hosts paints nothing. youtube.com was exactly that:
  // two `ytd-*` elements, no shadow trees, a white frame. ShadyDOM's polyfill
  // left `__shady_importNode` on the prototype and nothing under the public
  // name, because it wraps a native that was not there to wrap.
  //
  // Same clone as `cloneNode`, on purpose: two answers to "copy this subtree"
  // is how a stamp and a clone diverge. Documents and shadow roots refuse --
  // importing either is NotSupportedError / HierarchyRequestError, and a
  // TypeError here matches every other DOM refusal in this module.
  method("importNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || node == nullptr) {
      return call.Throw("TypeError", "importNode requires a Node");
    }
    if (node->GetKind() == dom::Node::Kind::Document) {
      return call.Throw("TypeError", "Document nodes cannot be imported");
    }
    if (node->IsDocumentFragment() &&
        static_cast<const dom::DocumentFragment*>(node)->Host() != nullptr) {
      return call.Throw("TypeError", "ShadowRoot nodes cannot be imported");
    }
    // Deep by default in the sense that Polymer always passes true; the
    // specification defaults to false, same as cloneNode.
    const bool deep = call.arguments.size() < 2 ? false : js::ToBoolean(Argument(call.arguments, 1));
    std::unique_ptr<dom::Node> copy = CloneDomNode(*node, deep);
    if (copy == nullptr) {
      return call.Throw("TypeError", "this node type cannot be imported");
    }
    return owner->AdoptClone(std::move(copy));
  });
  // Already owned by this bindings instance -- there is one DomBindings per
  // page, and every document it makes shares it -- so adopt is identity for a
  // node that is already here. A Document or a ShadowRoot still refuses, so a
  // page that probes with those gets a throw rather than a silent wrong keep.
  method("adoptNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || node == nullptr) {
      return call.Throw("TypeError", "adoptNode requires a Node");
    }
    if (node->GetKind() == dom::Node::Kind::Document) {
      return call.Throw("TypeError", "Document nodes cannot be adopted");
    }
    if (node->IsDocumentFragment() &&
        static_cast<const dom::DocumentFragment*>(node)->Host() != nullptr) {
      return call.Throw("TypeError", "ShadowRoot nodes cannot be adopted");
    }
    return owner->WrapperFor(node);
  });
  method("createComment", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateComment(js::ToString(Argument(call.arguments, 0)));
  });
  method("createEvent", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null() : owner->CreateLegacyEvent();
  });
  // `readyState`, and now it moves.
  //
  // It used to answer "complete" always, on the reasoning that scripts run
  // after the parse here so reporting "loading" would be a lie. That was the
  // wrong half of the trade: the two states are read by pages that write
  // `if (readyState === 'loading') addEventListener('DOMContentLoaded', go);
  // else go()`, and answering "complete" while `DOMContentLoaded` had not yet
  // fired sent the *other* half of them -- the ones that only listen -- into a
  // wait for an event that never came. reddit's interstitial is one of those.
  //
  // So the lifecycle is real: "loading" while the scripts run, "interactive"
  // when DOMContentLoaded fires, "complete" when the load does. What is still
  // a deviation is *when* the scripts run relative to the parse, which
  // PageScript.h records.
  //
  // The accessor is shared and the *state* is per-document: it reads a hidden
  // slot on its own receiver, so a document made by `createHTMLDocument` --
  // which is finished the moment it exists -- answers "complete" without the
  // page's own lifecycle touching it.
  const Value ready = interpreter_->NewNativeValue("readyState", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::String(std::string("loading"));
    }
    const Value* state = call.self.object->GetOwn(kReadyStateSlot);
    return state == nullptr ? Value::String(std::string("loading")) : *state;
  });
  if (ready.IsObject()) {
    document.object->SetHidden(kReadyStateSlot, Value::String(std::string("loading")));
    target.object->DefineAccessor("readyState", ready.object, nullptr);
  }

  // `document.body` and `document.documentElement`, as accessors so they
  // follow the tree rather than freezing whatever it looked like at install.
  const auto element_accessor = [this, &target](const char* name, const char* tag) {
    const Value native =
        interpreter_->NewNativeValue(name, [tag](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          return owner == nullptr
                     ? Value::Null()
                     : owner->WrapperFor(FindElementIn(
                           *owner->DocumentOf(call.self), [tag](const dom::Element& element) {
                             return element.TagName() == tag;
                           }));
        });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  element_accessor("body", "body");
  element_accessor("documentElement", "html");
  // Empty when nothing is stored for script yet. Reading `undefined` makes
  // `document.cookie.match(...)` throw before the page can handle a missing jar.
  const Value cookie_getter = interpreter_->NewNativeValue(
      "cookie", [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->cookies_ == nullptr) {
          return Value::String(std::string());
        }
        return Value::String(owner->cookies_->DocumentCookie());
      });
  const Value cookie_setter = interpreter_->NewNativeValue(
      "cookie", [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->cookies_ == nullptr || call.arguments.empty()) {
          return Value::Undefined();
        }
        (void)owner->cookies_->SetDocumentCookie(js::ToString(call.arguments[0]));
        return Value::Undefined();
      });
  if (cookie_getter.IsObject() && cookie_setter.IsObject()) {
    cookie_getter.object->Set(kOwnerSlot, PointerValue(this));
    cookie_setter.object->Set(kOwnerSlot, PointerValue(this));
    target.object->DefineAccessor("cookie", cookie_getter.object, cookie_setter.object);
  }
  InstallActiveElement(document);

  // `document.head`, `document.title` and the two element accessors, as
  // accessors so they follow the tree rather than freezing what it looked like
  // at install.
  element_accessor("head", "head");
  const Value title_getter = interpreter_->NewNativeValue("title", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::String(std::string());
    }
    dom::Element* title = FindElementIn(
        *owner->DocumentOf(call.self),
        [](const dom::Element& element) { return element.TagName() == "title"; });
    return Value::String(title == nullptr ? std::string() : title->TextContent());
  });
  if (title_getter.IsObject()) {
    title_getter.object->Set(kOwnerSlot, PointerValue(this));
    target.object->DefineAccessor("title", title_getter.object, nullptr);
  }

  InstallTreeWalkers(target);
  InstallImplementation(target);
  InstallMessageChannel();
  InstallRange();

  interpreter_->GlobalScope()->Declare("document", document, false);
  InstallEventConstructors();
  InstallObjectUrls();
  InstallWindow();
}

void DomBindings::SetReadyState(const char* state) {
  const Value document = WrapperFor(document_);
  if (document.IsObject()) {
    document.object->SetHidden(kReadyStateSlot, Value::String(std::string(state)));
  }
}

void DomBindings::InstallImplementation(const js::Value& document_interface) {
  const Value implementation = interpreter_->NewObjectValue();
  if (!implementation.IsObject() || !document_interface.IsObject()) {
    return;
  }
  const auto method = [this, &implementation](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      implementation.object->Set(name, native);
    }
  };

  // **A real second document, not a second view of this one.**
  //
  // youtube's `webcomponents-all-noPatch.js` makes one at module scope --
  // `Sd = document.implementation.createHTMLDocument("inert")` -- and parses
  // markup into elements created from it, which is what every sanitizer does
  // too. The point of the API is that the result is *inert*: nothing in it is
  // rendered, and nothing a page puts in it can reach the page.
  //
  // Handing back the page's own document under a new name would satisfy the
  // call and break exactly that, which is the stub ADR 0012 calls worse than
  // an absence. So this builds `<html><head><title>…</title></head><body>` in
  // a fresh `dom::Document`, and every `document.*` query resolves against its
  // receiver -- see DocumentOf.
  method("createHTMLDocument", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    auto made = std::make_unique<dom::Document>();
    dom::Document* raw = made.get();
    // Owned here for the life of the page, like every other node script made
    // and nothing appended: a node's owner is its parent, and a document has
    // none. A wrapper holds a raw pointer, so the node may not outlive it.
    owner->unattached_.push_back(std::move(made));

    auto& html = static_cast<dom::Element&>(raw->Append(std::make_unique<dom::Element>("html")));
    auto& head = static_cast<dom::Element&>(html.Append(std::make_unique<dom::Element>("head")));
    // The title is created whenever the argument was given at all, including
    // as the empty string -- which is the specification's distinction and not
    // a corner: `createHTMLDocument()` with no argument has no title element.
    if (!Argument(call.arguments, 0).IsUndefined()) {
      dom::Node& title = head.Append(std::make_unique<dom::Element>("title"));
      title.Append(std::make_unique<dom::Text>(js::ToString(Argument(call.arguments, 0))));
    }
    html.Append(std::make_unique<dom::Element>("body"));
    const Value wrapper = owner->WrapperFor(raw);
    if (wrapper.IsObject()) {
      // Finished the moment it exists: there is no load behind it and no
      // parser running in it, so "loading" would be a state nothing could
      // ever leave.
      wrapper.object->SetHidden(kReadyStateSlot, Value::String(std::string("complete")));
    }
    return wrapper;
  });

  // A doctype node, which a page passes to `createDocument` and reads
  // `.name` off. Public and qualified ids are accepted and dropped: this
  // parser has no XML in it, so there is nothing behind them to be right
  // about, and `name` is the only field anything reads.
  method("createDocumentType", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    auto made = std::make_unique<dom::DocumentType>(js::ToString(Argument(call.arguments, 0)));
    dom::Node* raw = made.get();
    owner->unattached_.push_back(std::move(made));
    return owner->WrapperFor(raw);
  });

  // True for everything, which is what the DOM standard says to answer and
  // is not a shortcut: `hasFeature` was deprecated precisely because engines
  // disagreed about it, and the specified behaviour is now to return true.
  method("hasFeature", [](NativeCall&) { return Value::Bool(true); });

  // On the interface, so every document has one -- including the ones this
  // makes, which is what lets a page nest the call.
  document_interface.object->Set("implementation", implementation);
}

}  // namespace microbrowser::bindings

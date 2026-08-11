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
#include "bindings/WebIdl.h"
#include "css/StyleSheet.h"
#include "js/Heap.h"

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
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    const std::string lowered = LowerCase(wanted);
    std::vector<Value> found;
    if (owner != nullptr) {
      ForEachElementIn(*owner->DocumentOf(call.self), [&](dom::Element& element) {
        if (MatchesTagName(element, wanted, lowered)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("getElementsByTagNameNS", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const NamespaceQuery wanted(Argument(call.arguments, 0), Argument(call.arguments, 1));
    std::vector<Value> found;
    if (owner != nullptr) {
      ForEachElementIn(*owner->DocumentOf(call.self), [&](dom::Element& element) {
        if (wanted.Matches(element)) {
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
                            : owner->CreateText(js::ToString(Argument(call.arguments, 0)),
                                                *owner->DocumentOf(call.self));
  });
  method("createElement", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (!RequireArguments(call, "Document", "createElement", 1)) {
      return call.ThrownValue();
    }
    std::string name;
    if (!ToDomString(call, call.arguments[0], name)) {
      return call.ThrownValue();
    }
    // A name that cannot be written back out as markup is refused, rather than
    // becoming an element no serialiser can produce and no parser can read.
    if (!IsValidLocalName(name, NameKind::Element)) {
      return ThrowDom(call, "InvalidCharacterError",
                      "'" + name + "' is not a valid element name");
    }
    if (owner == nullptr) {
      return Value::Null();
    }
    // ASCII-lower-cased, and only ASCII: `createElement("İnput")` must not
    // become an `<input>`, which is exactly what a locale-aware fold would do.
    return owner->CreateElement(LowerCase(name), *owner->DocumentOf(call.self));
  });
  // The namespace is kept, and with it the prefix. **Nothing is lower-cased**:
  // `createElement` folds a name because an HTML document's element names are
  // case-insensitive, and `createElementNS` does not, because
  // `createElementNS(SVG_NS, 'linearGradient')` names an element that only
  // exists spelled that way. That difference is most of what `dom/nodes/case.html`
  // tests, and it is why these two are not one method with a defaulted
  // argument.
  method("createElementNS", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (!RequireArguments(call, "Document", "createElementNS", 2)) {
      return call.ThrownValue();
    }
    QualifiedName name;
    if (!ToQualifiedName(call, call.arguments[0], call.arguments[1], NameKind::Element, name)) {
      return call.ThrownValue();
    }
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateElementNS(std::move(name), *owner->DocumentOf(call.self));
  });
  method("createDocumentFragment", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null()
                            : owner->CreateDocumentFragment(*owner->DocumentOf(call.self));
  });
  // `<?target data?>`. Legal in an HTML document even though the HTML parser
  // cannot produce one -- the DOM allows it and only refuses the two things
  // that would make the result unserializable: a target that is not a name,
  // and data containing `?>`.
  method("createProcessingInstruction", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (!RequireArguments(call, "Document", "createProcessingInstruction", 2)) {
      return call.ThrownValue();
    }
    std::string instruction_target;
    std::string data;
    if (!ToDomString(call, call.arguments[0], instruction_target) ||
        !ToDomString(call, call.arguments[1], data)) {
      return call.ThrownValue();
    }
    if (!IsValidLocalName(instruction_target, NameKind::Element)) {
      return ThrowDom(call, "InvalidCharacterError",
                      "'" + instruction_target + "' is not a valid processing instruction target");
    }
    if (data.find("?>") != std::string::npos) {
      return ThrowDom(call, "InvalidCharacterError",
                      "a processing instruction's data may not contain '?>'");
    }
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->AdoptUnattached(
        std::make_unique<dom::ProcessingInstruction>(std::move(instruction_target),
                                                     std::move(data)),
        *owner->DocumentOf(call.self));
  });
  // A CDATA section is XML-only, and the DOM says an HTML document throws
  // rather than making one. Every document this browser has is an HTML
  // document, so this is the whole implementation rather than a stub of one --
  // and a page that feature-detects gets a real refusal, not a node no
  // serializer here could write back out.
  method("createCDATASection", [](NativeCall& call) -> Value {
    return ThrowDom(call, "NotSupportedError",
                    "createCDATASection is not available in an HTML document");
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
    // The *receiver's* document, which is the whole difference between this
    // and `cloneNode`: importing is copying into the document that was asked.
    return owner->AdoptClone(std::move(copy), *owner->DocumentOf(call.self));
  });
  // Every document a page holds is owned by this one bindings instance, so
  // adoption never moves ownership -- what it moves is the *node document*,
  // which is now a real answer rather than one document for everything. The
  // node is removed from its old parent first, because that is what the DOM's
  // adopt does and because a node in a tree whose document is a different one
  // is the inconsistency the whole node-document field exists to prevent.
  method("adoptNode", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || node == nullptr) {
      return call.Throw("TypeError", "adoptNode requires a Node");
    }
    if (node->GetKind() == dom::Node::Kind::Document) {
      return ThrowDom(call, "NotSupportedError", "a Document cannot be adopted");
    }
    if (node->IsDocumentFragment() &&
        static_cast<const dom::DocumentFragment*>(node)->Host() != nullptr) {
      return ThrowDom(call, "HierarchyRequestError", "a ShadowRoot cannot be adopted");
    }
    dom::Document& into = *owner->DocumentOf(call.self);
    if (node->Parent() != nullptr) {
      owner->DetachFromTree(*node);
    }
    node->SetNodeDocument(&into);
    return owner->WrapperFor(node);
  });
  method("createComment", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateComment(js::ToString(Argument(call.arguments, 0)),
                                *owner->DocumentOf(call.self));
  });
  method("createEvent", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (!RequireArguments(call, "Document", "createEvent", 1)) {
      return call.ThrownValue();
    }
    std::string name;
    if (!ToDomString(call, call.arguments[0], name)) {
      return call.ThrownValue();
    }
    // A name outside the DOM's legacy table is a NotSupportedError, and that
    // includes the *pluralised* forms of interfaces that have no legacy alias
    // ("FocusEvents") and the interfaces that exist but were added after the
    // table closed ("CloseEvent"). Answering with a plain Event for those
    // would hand a page an object of the wrong type under a name it chose.
    const char* interface = DomBindings::LegacyEventInterface(name);
    if (interface == nullptr) {
      return ThrowDom(call, "NotSupportedError",
                      "'" + name + "' is not a legacy event interface name");
    }
    return owner == nullptr ? Value::Null() : owner->CreateLegacyEvent(interface);
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

  // `documentElement` is the **first element child**, not the first `<html>`
  // in the tree. The two agree on every parsed page and disagree the moment a
  // page replaces the document element -- `doc.replaceChild(x, doc
  // .documentElement)` left `documentElement` answering null, because there was
  // no longer an element called `html` anywhere.
  const Value document_element =
      interpreter_->NewNativeValue("documentElement", [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        return owner == nullptr ? Value::Null()
                                : owner->WrapperFor(owner->DocumentOf(call.self)
                                                        ->DocumentElement());
      });
  if (document_element.IsObject()) {
    document_element.object->Set(kOwnerSlot, PointerValue(this));
    target.object->DefineAccessor("documentElement", document_element.object, nullptr);
  }

  // `document.doctype`: the first DocumentType child, or null. An accessor for
  // the reason the two above are: a page can remove it.
  const Value doctype = interpreter_->NewNativeValue("doctype", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    for (const std::unique_ptr<dom::Node>& child : owner->DocumentOf(call.self)->Children()) {
      if (child->GetKind() == dom::Node::Kind::DocumentType) {
        return owner->WrapperFor(child.get());
      }
    }
    return Value::Null();
  });
  if (doctype.IsObject()) {
    doctype.object->Set(kOwnerSlot, PointerValue(this));
    target.object->DefineAccessor("doctype", doctype.object, nullptr);
  }

  // `document.defaultView`: the window a document is displayed in, or null for
  // one that is in no window at all.
  //
  // It was simply missing, and the cost of that is larger than it looks:
  // `doc.defaultView.DOMException`, `doc.defaultView.getComputedStyle(el)` and
  // `ownerDocument.defaultView` are how a script that was handed a *node*
  // reaches the global its constructors live in -- which is the only correct
  // way to write that once more than one document can exist. Every one of them
  // was `undefined.something`, and web-platform-tests' `assert_throws_dom`
  // takes the global that way on every negative test it runs.
  //
  // Null for a document `createHTMLDocument` made, which is the distinction
  // the property exists to draw: that document has no browsing context, so it
  // has no window, and a page that treats the two the same ends up asking one
  // document's global about another's nodes.
  {
    const Value native = interpreter_->NewNativeValue("defaultView", [](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr || owner->DocumentOf(call.self) != owner->MainDocument()) {
        return Value::Null();
      }
      return Value::Obj(call.interpreter.Global());
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor("defaultView", native.object, nullptr);
    }
  }

  // `document.all` (HTML obsolete / [[IsHTMLDDA]]). Absent, `undefined !==
  // document.all` is false and polymer_resin's `!Z && Z !== document.all`
  // treats every undefined sink as the innocuous string `"zClosurez"` -- which
  // made `HTMLElement.hidden = undefined` set the attribute and blanked
  // youtube search (`hidden="[[data.hideContents]]"`).
  {
    js::Object* all = interpreter_->GetHeap().AllocateObject(js::Object::Kind::HTMLAllCollection);
    if (all != nullptr) {
      // Seed Object.prototype the same way every other plain object does —
      // NewObject() is private to the interpreter.
      const Value seed = interpreter_->NewObjectValue();
      if (seed.IsObject()) {
        all->SetPrototype(seed.object->Prototype());
      }
      // A data property rather than a capturing accessor: the Value is on the
      // prototype map, so the collector sees it. Overwriting `document.all` is
      // a page's own act and not a reason to keep a second copy.
      target.object->Set("all", Value::Obj(all));
    }
  }

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
  InstallBroadcastChannel();
  InstallIndexedDb();
  InstallRange();
  InstallPageVisibility(target);

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

// The document a `DOMImplementation` call was made against: the one its
// receiver was handed out for, and the page's own only when a page has taken
// the method off and called it on something else.
dom::Document* ImplementationDocumentOf(const js::Value& self) {
  if (!self.IsObject()) {
    return nullptr;
  }
  const js::Object* behind = BehindProxies(self.object);
  const js::Value* slot = behind == nullptr ? nullptr
                                            : behind->GetOwn(kImplementationDocumentSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Document*>(static_cast<std::uintptr_t>(slot->number));
}

void DomBindings::InstallImplementation(const js::Value& document_interface) {
  // A real interface with a real name, so that `document.implementation
  // instanceof DOMImplementation` answers -- and so that the per-document
  // objects below share one set of methods rather than one per document.
  const Value implementation = MakeInterface("DOMImplementation", Value::Undefined());
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

    // The doctype first, which is step 2 of the algorithm and not decoration:
    // `doc.doctype` is read by every test of the document mutation constraints,
    // and a document with no doctype makes "inserting a doctype if there
    // already is one should throw" pass for the wrong reason.
    raw->Append(std::make_unique<dom::DocumentType>("html"));
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

  // A doctype node. All three strings are kept: they are what a doctype *is*,
  // and dropping the public and system ids made `createDocumentType(n, p, s)`
  // a lossy round trip through an object whose only job is to carry them.
  //
  // The name is validated as a *qualified* name -- a doctype's name may have a
  // colon in it, which is why this is not the element rule.
  method("createDocumentType", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (!RequireArguments(call, "DOMImplementation", "createDocumentType", 3)) {
      return call.ThrownValue();
    }
    std::string name;
    std::string public_id;
    std::string system_id;
    if (!ToDomString(call, call.arguments[0], name) ||
        !ToDomString(call, call.arguments[1], public_id) ||
        !ToDomString(call, call.arguments[2], system_id)) {
      return call.ThrownValue();
    }
    if (!IsValidDoctypeName(name)) {
      return ThrowDom(call, "InvalidCharacterError",
                      "'" + name + "' is not a valid doctype name");
    }
    dom::Document* node_document = ImplementationDocumentOf(call.self);
    if (owner == nullptr || node_document == nullptr) {
      return Value::Null();
    }
    return owner->AdoptUnattached(std::make_unique<dom::DocumentType>(
                                      std::move(name), std::move(public_id),
                                      std::move(system_id)),
                                  *node_document);
  });

  // True for everything, which is what the DOM standard says to answer and
  // is not a shortcut: `hasFeature` was deprecated precisely because engines
  // disagreed about it, and the specified behaviour is now to return true.
  method("hasFeature", [](NativeCall&) { return Value::Bool(true); });

  // An accessor rather than one shared object, because the answer depends on
  // *which* document was asked: `doc.implementation.createDocumentType(…)` must
  // make a node whose `ownerDocument` is `doc`. Cached on the document wrapper
  // so `document.implementation === document.implementation`, which a page can
  // and does check.
  const Value implementation_accessor =
      interpreter_->NewNativeValue("implementation", [implementation](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || !call.self.IsObject()) {
          return Value::Undefined();
        }
        if (const Value* cached = call.self.object->GetOwn(kImplementationSlot)) {
          return *cached;
        }
        const Value made = call.interpreter.NewObjectValue();
        if (!made.IsObject()) {
          return Value::Undefined();
        }
        made.object->SetPrototype(implementation.object);
        made.object->SetHidden(kImplementationDocumentSlot,
                               PointerValue(owner->DocumentOf(call.self)));
        call.self.object->SetHidden(kImplementationSlot, made);
        return made;
      });
  if (implementation_accessor.IsObject()) {
    implementation_accessor.object->Set(kOwnerSlot, PointerValue(this));
    document_interface.object->DefineAccessor("implementation", implementation_accessor.object,
                                              nullptr);
  }
}

void DomBindings::InstallPageVisibility(const js::Value& document_interface) {
  if (!document_interface.IsObject()) {
    return;
  }

  // Page Visibility (ADR 0017 survey: 34 `visibilitychange` sites). A headless
  // snapshot and a foreground browser tab are both *visible* -- nothing here
  // hides the document yet, and answering hidden would send youtube's player
  // down a path that refuses to start.
  const Value hidden_getter = interpreter_->NewNativeValue(
      "hidden", [](NativeCall&) { return Value::Bool(false); });
  const Value visibility_getter = interpreter_->NewNativeValue(
      "visibilityState", [](NativeCall&) { return Value::String(std::string("visible")); });
  if (hidden_getter.IsObject()) {
    document_interface.object->DefineAccessor("hidden", hidden_getter.object, nullptr);
  }
  if (visibility_getter.IsObject()) {
    document_interface.object->DefineAccessor("visibilityState", visibility_getter.object, nullptr);
  }

  const Value has_focus = interpreter_->NewNativeValue("hasFocus", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Bool(false);
    }
    // The browsing context has focus whenever this page is the one being driven.
    // A finer model can key off platform focus later; youtube reads this before
    // play and a constant false is the same class of bug as hidden=true.
    (void)owner;
    return Value::Bool(true);
  });
  if (has_focus.IsObject()) {
    has_focus.object->Set(kOwnerSlot, PointerValue(this));
    document_interface.object->Set("hasFocus", has_focus);
  }

  if (geometry_ == nullptr) {
    return;
  }
  const Value element_from_point = interpreter_->NewNativeValue(
      "elementFromPoint", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->geometry_ == nullptr || call.arguments.size() < 2) {
          return Value::Null();
        }
        const float x = static_cast<float>(js::ToNumber(Argument(call.arguments, 0)));
        const float y = static_cast<float>(js::ToNumber(Argument(call.arguments, 1)));
        dom::Element* hit = owner->geometry_->ElementAtViewport(x, y);
        return owner->WrapperFor(hit);
      });
  if (element_from_point.IsObject()) {
    element_from_point.object->Set(kOwnerSlot, PointerValue(this));
    document_interface.object->Set("elementFromPoint", element_from_point);
  }
  WireTrustedScriptHooks();
}

}  // namespace microbrowser::bindings

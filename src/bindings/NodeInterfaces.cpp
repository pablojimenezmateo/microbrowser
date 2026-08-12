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
#include "bindings/LiveRanges.h"
#include "bindings/ShadowDom.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// Which tag gets which interface.
//
// Every tag HTML gives an interface of its own, and the interface's parent
// when that is not HTMLElement.
//
// It used to be a short list of "tags a page is likely to name", and the
// argument for that was wrong in a way the suite made obvious:
// `Node-cloneNode.html` asks `assert_true("HTMLAreaElement" in window)` before
// it clones an `<area>`, and 48 of its subtests failed on that line alone --
// not on cloning. A page tests for a type before it has one, and the cost of a
// missing name is a ReferenceError rather than a false `instanceof`.
//
// A tag *not* here is an HTMLElement, which is right for `<abbr>` and wrong for
// `<foo>` -- an unknown tag is an HTMLUnknownElement. That distinction needs a
// list of the tags HTML defines at all, which is C10's, not this table's.
struct TagInterface {
  std::string_view tag;
  const char* interface;
  // Null means HTMLElement. The two that are not are `<svg>`, which is an
  // Element with no HTML semantics at all, and the media pair.
  const char* parent = nullptr;
};

constexpr TagInterface kTagInterfaces[] = {
    {"a", "HTMLAnchorElement"},
    {"area", "HTMLAreaElement"},
    {"audio", "HTMLAudioElement", "HTMLMediaElement"},
    {"base", "HTMLBaseElement"},
    {"blockquote", "HTMLQuoteElement"},
    {"body", "HTMLBodyElement"},
    {"br", "HTMLBRElement"},
    {"button", "HTMLButtonElement"},
    {"canvas", "HTMLCanvasElement"},
    {"caption", "HTMLTableCaptionElement"},
    {"col", "HTMLTableColElement"},
    {"colgroup", "HTMLTableColElement"},
    {"data", "HTMLDataElement"},
    {"datalist", "HTMLDataListElement"},
    {"del", "HTMLModElement"},
    {"details", "HTMLDetailsElement"},
    {"dialog", "HTMLDialogElement"},
    {"dir", "HTMLDirectoryElement"},
    {"div", "HTMLDivElement"},
    {"dl", "HTMLDListElement"},
    {"embed", "HTMLEmbedElement"},
    {"fieldset", "HTMLFieldSetElement"},
    {"font", "HTMLFontElement"},
    {"form", "HTMLFormElement"},
    {"frame", "HTMLFrameElement"},
    {"frameset", "HTMLFrameSetElement"},
    {"h1", "HTMLHeadingElement"},
    {"h2", "HTMLHeadingElement"},
    {"h3", "HTMLHeadingElement"},
    {"h4", "HTMLHeadingElement"},
    {"h5", "HTMLHeadingElement"},
    {"h6", "HTMLHeadingElement"},
    {"head", "HTMLHeadElement"},
    {"hr", "HTMLHRElement"},
    {"html", "HTMLHtmlElement"},
    {"iframe", "HTMLIFrameElement"},
    {"img", "HTMLImageElement"},
    {"input", "HTMLInputElement"},
    {"ins", "HTMLModElement"},
    {"label", "HTMLLabelElement"},
    {"legend", "HTMLLegendElement"},
    {"li", "HTMLLIElement"},
    {"link", "HTMLLinkElement"},
    {"listing", "HTMLPreElement"},
    {"map", "HTMLMapElement"},
    {"marquee", "HTMLMarqueeElement"},
    {"menu", "HTMLMenuElement"},
    {"meta", "HTMLMetaElement"},
    {"meter", "HTMLMeterElement"},
    {"object", "HTMLObjectElement"},
    {"ol", "HTMLOListElement"},
    {"optgroup", "HTMLOptGroupElement"},
    {"option", "HTMLOptionElement"},
    {"output", "HTMLOutputElement"},
    {"p", "HTMLParagraphElement"},
    {"param", "HTMLParamElement"},
    {"picture", "HTMLPictureElement"},
    {"pre", "HTMLPreElement"},
    {"progress", "HTMLProgressElement"},
    {"q", "HTMLQuoteElement"},
    {"script", "HTMLScriptElement"},
    {"select", "HTMLSelectElement"},
    {"slot", "HTMLSlotElement"},
    {"source", "HTMLSourceElement"},
    {"span", "HTMLSpanElement"},
    {"style", "HTMLStyleElement"},
    {"table", "HTMLTableElement"},
    {"tbody", "HTMLTableSectionElement"},
    {"td", "HTMLTableCellElement"},
    {"template", "HTMLTemplateElement"},
    {"textarea", "HTMLTextAreaElement"},
    {"tfoot", "HTMLTableSectionElement"},
    {"th", "HTMLTableCellElement"},
    {"thead", "HTMLTableSectionElement"},
    {"time", "HTMLTimeElement"},
    {"title", "HTMLTitleElement"},
    {"tr", "HTMLTableRowElement"},
    {"track", "HTMLTrackElement"},
    {"ul", "HTMLUListElement"},
    {"video", "HTMLVideoElement", "HTMLMediaElement"},
    {"xmp", "HTMLPreElement"},
    // `<svg>` is an Element and not an HTMLElement. Only the root tag is
    // listed: the elements *inside* an SVG subtree are not distinguished,
    // because `src/html` has no foreign content (TreeBuilder.h says so) and so
    // this DOM has no namespace to ask about. When foreign content lands, this
    // is where its tags go.
    {"svg", "SVGElement", "Element"},
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

std::string NodeNameOf(const dom::Node& node) {
  switch (node.GetKind()) {
    case dom::Node::Kind::Element: {
      // Upper case for an HTML element in an HTML document, and *as written*
      // for anything else: `createElementNS(SVG_NS, 'linearGradient').tagName`
      // is `linearGradient`, and upper-casing it would name an element that
      // does not exist.
      //
      // **Both halves of that condition**, now that `DOMParser` can make an XML
      // document. `<div xmlns="…xhtml">` parsed as XML is an HTML-namespace
      // element whose `tagName` is `div`, and it becomes `DIV` the moment
      // `importNode` moves it into an HTML document -- so the answer depends on
      // where the element *is*, not only on what it is.
      const auto& element = static_cast<const dom::Element&>(node);
      const dom::Document* document = element.NodeDocument();
      const bool html_document = document == nullptr || document->IsHtmlDocument();
      return element.Namespace().IsHtml() && html_document
                 ? util::AsciiUpperCase(element.TagName())
                 : element.TagName();
    }
    case dom::Node::Kind::Text:
      return static_cast<const dom::Text&>(node).IsCData() ? "#cdata-section" : "#text";
    case dom::Node::Kind::Comment:
      return "#comment";
    case dom::Node::Kind::Document:
      return "#document";
    case dom::Node::Kind::DocumentFragment:
      return "#document-fragment";
    case dom::Node::Kind::ProcessingInstruction:
      // The *target*, which is the reason a processing instruction is not a
      // comment with a longer string in it.
      return static_cast<const dom::ProcessingInstruction&>(node).Target();
    case dom::Node::Kind::DocumentType:
      // A doctype's name, as written -- `html` for every page this browser
      // will meet, and not upper-cased, because it is not a tag name.
      return static_cast<const dom::DocumentType&>(node).Name();
  }
  return "#unknown";
}

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
  // A binding in the global scope *only*, not also an own property on the
  // global object. Builtins (Math, Object, …) are the same shape, and the
  // two spellings stay one namespace because GetProperty/SetProperty on the
  // global fall through to the binding when there is no own property.
  //
  // MakeInterface used to do both Set and Declare. That looked belt-and-
  // braces and was the opposite: `window.ShadowRoot = yc` (ShadyDOM) updated
  // the own property while the bare name `ShadowRoot` kept resolving to the
  // stale binding -- so Polymer stamped with `ShadowRoot.prototype.za` on
  // *our* prototype, which has no `za`, and youtube.com stayed a white page.
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

  // EventTarget is the root, and it is not decoration: the specification puts
  // `addEventListener` there rather than on Node, and a polyfill that patches
  // event dispatch patches `EventTarget.prototype` -- which is exactly what
  // youtube's webcomponents bundle does, guarded by `window.EventTarget ? ... :
  // ...` where the else branch patches Node and Window separately. A browser
  // without the name takes the branch written for browsers from before it
  // existed.
  const Value event_target = MakeInterface("EventTarget", Value::Undefined());
  InstallEventMethods(event_target);
  const Value node = MakeInterface("Node", event_target);
  // The nodeType constants live on the interface object. ShadyDOM's ShadowRoot
  // does `Object.defineProperties(proto, { nodeType: { value:
  // Node.DOCUMENT_FRAGMENT_NODE } })` at load time; without the constant that
  // value is `undefined`, `getRootNode` takes the `if (this.nodeType)` path
  // as false and returns undefined, and ShadyCSS's class scoper then throws
  // `cannot read property 'host' of undefined` on every setAttribute("class").
  // youtube.com's custom elements never finished stamping.
  if (Value* node_ctor = interpreter_->GlobalScope()->Lookup("Node")) {
    if (node_ctor->IsObject()) {
      static constexpr const char* kNames[] = {
          "ELEMENT_NODE",
          "ATTRIBUTE_NODE",
          "TEXT_NODE",
          "CDATA_SECTION_NODE",
          "ENTITY_REFERENCE_NODE",
          "ENTITY_NODE",
          "PROCESSING_INSTRUCTION_NODE",
          "COMMENT_NODE",
          "DOCUMENT_NODE",
          "DOCUMENT_TYPE_NODE",
          "DOCUMENT_FRAGMENT_NODE",
          "NOTATION_NODE",
      };
      for (int i = 0; i < 12; ++i) {
        const Value number = Value::Number(static_cast<double>(i + 1));
        node_ctor->object->Set(kNames[i], number);
        // Also on the prototype: a page that reads `Node.prototype.ELEMENT_NODE`
        // or an instance's inherited constant gets the same answer browsers do.
        if (node.IsObject()) {
          node.object->Set(kNames[i], number);
        }
      }
      // **The `compareDocumentPosition` bit names, and they are not
      // decoration.** `dom/common.js` computes every expected tree-order answer
      // with `nodeB.compareDocumentPosition(nodeA) & Node.DOCUMENT_POSITION_FOLLOWING`.
      // An undefined constant makes that `x & undefined`, which is 0, which is
      // falsy -- so the helper reported "before" for every pair of nodes in the
      // document and 930 `comparePoint` subtests failed against a *correct*
      // implementation. A missing constant does not read as missing; it reads
      // as a wrong answer somewhere else entirely.
      struct PositionBit {
        const char* name;
        double value;
      };
      static constexpr PositionBit kPositions[] = {
          {"DOCUMENT_POSITION_DISCONNECTED", 0x01},
          {"DOCUMENT_POSITION_PRECEDING", 0x02},
          {"DOCUMENT_POSITION_FOLLOWING", 0x04},
          {"DOCUMENT_POSITION_CONTAINS", 0x08},
          {"DOCUMENT_POSITION_CONTAINED_BY", 0x10},
          {"DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 0x20},
      };
      for (const PositionBit& bit : kPositions) {
        node_ctor->object->Set(bit.name, Value::Number(bit.value));
        if (node.IsObject()) {
          node.object->Set(bit.name, Value::Number(bit.value));
        }
      }
    }
  }
  InstallNodeInterface(node);
  InstallNodeQueries(node);
  const Value element = MakeInterface("Element", node);
  InstallElementInterface(element);
  InstallParentQueries(element);
  InstallElementIdentity(element);
  InstallGeometry(element);
  // `innerHTML`, `outerHTML` and `insertAdjacentHTML`, on Element because that
  // is where the specification puts them and because a Text node with an
  // `innerHTML` would be a name a page could feature-detect and then misuse.
  InstallHtmlParsing(element);
  // `attachShadow` and what a slot answers. On Element for the same reason.
  InstallShadowDom(element);
  // Web Animations: after Element exists so `animate` lands on the prototype a
  // page feature-detects. Absent when no AnimationSource (ADR 0012 / TD-0021).
  InstallWaapi(element);
  const Value html_element = MakeInterface("HTMLElement", element);
  InstallElementInternals(*this, *interpreter_, html_element);
  // On HTMLElement rather than Element, which is where the specification puts
  // them: focus is an HTML concept, and an SVG element in this tree is an
  // Element with no HTML semantics at all.
  InstallFocus(html_element);
  // `SVGElement`, before the loop below, because it is the one entry in that
  // table whose parent is Element rather than HTMLElement -- and MakeInterface
  // returns an interface that already exists, so the loop leaves it alone.
  //
  // Its prototype is empty of SVG's own geometry API, which is honest: nothing
  // in this browser produces an element that inherits from it yet. What the name
  // is for is the shape a page uses it in -- `window.SVGElement.prototype
  // .hasOwnProperty('classList')`, which is how youtube's webcomponents bundle
  // decides whether `classList` needs patching, reached unqualified off `window`
  // and therefore a TypeError rather than a ReferenceError when it is missing.
  MakeInterface("SVGElement", element);
  // `HTMLMediaElement` before the loop for the same reason: it is a parent in
  // the table, and `video instanceof HTMLMediaElement` is what a feature check
  // asks. The media API lives on it rather than on each of the two, which is
  // where the specification puts it and one prototype rather than two.
  const Value media_element = MakeInterface("HTMLMediaElement", html_element);
  InstallMediaElement(media_element);
  // Every per-tag interface, up front rather than when its tag is first seen.
  // Lazily was tempting and wrong: `x instanceof HTMLAnchorElement` has to
  // answer *false* on a page with no anchor in it, and a name that does not
  // exist until the tag does throws a ReferenceError instead. A page tests for
  // a type before it has one far more often than after.
  for (const TagInterface& entry : kTagInterfaces) {
    if (entry.parent == nullptr) {
      MakeInterface(entry.interface, html_element);
      continue;
    }
    const Value* parent = interfaces_.object->GetOwn(entry.parent);
    MakeInterface(entry.interface, parent == nullptr ? html_element : *parent);
  }
  // The reflected `DOMTokenList`s other than `classList`, each on exactly the
  // interfaces HTML gives it and no others.
  //
  // Not on Element.prototype, and that is the point: `div.relList` must be
  // *undefined*, because a page that feature-detects `el.relList` to decide
  // whether it is looking at a link would otherwise get yes for every element.
  // `classList` is the one that is universal, and it lives with the rest of
  // Element's surface for that reason.
  //
  // Each is cached on the wrapper, like `classList` and for the same reason:
  // the list is live -- it re-reads its attribute on every operation -- so
  // caching costs nothing in staleness, while not caching makes
  // `el.relList !== el.relList`, which pages compare.
  struct ReflectedTokenList {
    const char* interface;
    const char* property;
    const char* attribute;
  };
  static constexpr ReflectedTokenList kReflectedTokenLists[] = {
      {"HTMLAnchorElement", "relList", "rel"},
      {"HTMLAreaElement", "relList", "rel"},
      {"HTMLLinkElement", "relList", "rel"},
      {"HTMLLinkElement", "sizes", "sizes"},
      {"HTMLIFrameElement", "sandbox", "sandbox"},
      {"HTMLOutputElement", "htmlFor", "for"},
  };
  for (const ReflectedTokenList& entry : kReflectedTokenLists) {
    const Value* prototype = interfaces_.object->GetOwn(entry.interface);
    if (prototype == nullptr || !prototype->IsObject()) {
      continue;
    }
    const std::string slot = std::string("#tokenList:") + entry.property;
    const char* attribute = entry.attribute;
    const Value getter =
        interpreter_->NewNativeValue(entry.property, [slot, attribute](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          dom::Node* self = NodeOf(call.self);
          if (owner == nullptr || self == nullptr || !self->IsElement()) {
            return Value::Undefined();
          }
          if (call.self.IsObject()) {
            if (const Value* cached = call.self.object->GetOwn(slot)) {
              return *cached;
            }
          }
          const Value list = owner->MakeTokenList(static_cast<dom::Element&>(*self), attribute);
          if (call.self.IsObject()) {
            call.self.object->SetHidden(slot, list);
          }
          return list;
        });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      prototype->object->DefineAccessor(entry.property, getter.object, nullptr);
    }
  }
  if (js::Value* script_ctor = interpreter_->GlobalScope()->Lookup("HTMLScriptElement")) {
    if (script_ctor->IsObject()) {
      const Value supports = interpreter_->NewNativeValue("supports", [](NativeCall&) {
        return Value::Bool(false);
      });
      if (supports.IsObject()) {
        script_ctor->object->Set("supports", supports);
      }
    }
  }
  if (const Value* image_interface = interfaces_.object->GetOwn("HTMLImageElement")) {
    InstallImageElement(*image_interface);
  }
  // `<canvas>`, on its own interface. ADR 0029 §2: `getContext` and the size attributes, and nothing
  // when there is no surface behind the layer -- a `getContext` returning an object whose methods did
  // nothing would make a page draw into nothing and show it.
  if (const Value* canvas_interface = interfaces_.object->GetOwn("HTMLCanvasElement")) {
    InstallCanvas(*canvas_interface);
  }
  // `template.content`, on HTMLTemplateElement and nowhere else. It is the only
  // way a page can reach a template's markup -- the tree walks cannot, which is
  // the point of the element -- and it is read-only: the fragment is the
  // element's, and letting a page swap it would put a node with two owners in
  // the tree.
  if (const Value* template_interface = interfaces_.object->GetOwn("HTMLTemplateElement")) {
    const Value getter = interpreter_->NewNativeValue("content", [](js::NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      dom::Node* self = NodeOf(call.self);
      if (owner == nullptr || self == nullptr || !self->IsElement()) {
        return Value::Undefined();
      }
      dom::DocumentFragment* content = static_cast<dom::Element&>(*self).Content();
      return content == nullptr ? Value::Undefined() : owner->WrapperFor(content);
    });
    if (getter.IsObject() && template_interface->IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      template_interface->object->DefineAccessor("content", getter.object, nullptr);
    }
    // The four `shadowroot*` content attributes, reflected.
    //
    // They are reflection and nothing more: setting `shadowRootMode` on a
    // template already in the tree attaches no shadow root, because the parser
    // is the only thing that acts on them and it has already been past. A page
    // uses them for feature detection -- `'shadowRootMode' in
    // HTMLTemplateElement.prototype` is the documented test for declarative
    // shadow DOM -- which is the reason they must exist even though nothing
    // reads them back.
    if (template_interface->IsObject()) {
      InstallTemplateShadowReflection(*this, *interpreter_, *template_interface);
    }
  }
  // Text and Comment share a base, and it is not decoration: a polyfill that
  // patches `data` or `length` patches CharacterData once rather than both.
  const Value character_data = MakeInterface("CharacterData", node);
  InstallCharacterData(character_data);
  const Value text_interface = MakeInterface("Text", character_data);
  // `splitText`, which is the one method a Text has that a Comment does not --
  // and the one the DOM writes `Range.insertNode` in terms of, so both reach
  // the same `SplitTextNode` rather than each cutting the string its own way.
  // The live-range fixups live inside it, because a boundary past the split
  // point belongs to the *tail* node afterwards and nothing else knows that.
  if (const Value split = interpreter_->NewNativeValue(
          "splitText",
          [](js::NativeCall& call) -> Value {
            DomBindings* owner = OwnerOf(call);
            dom::Node* self = NodeOf(call.self);
            std::uint32_t offset = 0;
            if (!RequireArguments(call, "Text", "splitText", 1) ||
                !ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, offset)) {
              return call.ThrownValue();
            }
            if (owner == nullptr || self == nullptr || !self->IsText()) {
              return call.Throw("TypeError", "splitText called on a non-Text node");
            }
            auto& text = static_cast<dom::Text&>(*self);
            if (offset > DomStringLength(text.Data())) {
              return ThrowDom(call, "IndexSizeError",
                              "the offset is larger than the node's length");
            }
            // A parentless text node still splits; it just has nowhere to put
            // the tail, so the tail is a node script owns and nothing holds.
            if (text.Parent() == nullptr) {
              const std::string data = text.Data();
              const std::string tail = DomSubstring(data, offset, DomStringLength(data));
              owner->SetCharacterData(self, DomSubstring(data, 0, offset));
              return owner->AdoptUnattached(std::make_unique<dom::Text>(tail, text.IsCData()),
                                            owner->NodeDocumentOf(text));
            }
            dom::Node* parent = text.Parent();
            dom::Node* made = SplitTextNode(call.interpreter, text, offset);
            if (made == nullptr) {
              return Value::Null();
            }
            owner->RecordMutation(*parent, "childList", {}, Value::Null(), {made}, {});
            return owner->WrapperFor(made);
          });
      split.IsObject() && text_interface.IsObject()) {
    split.object->Set(kOwnerSlot, PointerValue(this));
    text_interface.object->Set("splitText", split);
  }
  MakeInterface("Comment", character_data);
  // A CDATASection is XML-only, and this browser can now hold one: an XML
  // document -- `new Document()` or `implementation.createDocument(...)` --
  // answers `createCDATASection`, and an HTML document still throws
  // NotSupportedError, which is the specification's own answer rather than a
  // limitation. The *name* was here first, because
  // youtube's `webcomponents-all-noPatch.js` does
  // `["Text","Comment","CDATASection","ProcessingInstruction"].forEach(
  //    function (a) { var b = window[a]; Object.create(b.prototype) ... })`
  // and takes a TypeError on a missing one. ADR 0012 read from the other side:
  // a missing interface object is not a fallback path, it is a wall.
  MakeInterface("CDATASection", text_interface);
  // A ProcessingInstruction, on the other hand, is now a node this browser can
  // hold: `document.createProcessingInstruction` makes one, and the HTML
  // parser still cannot.
  const Value processing_instruction = MakeInterface("ProcessingInstruction", character_data);
  if (processing_instruction.IsObject()) {
    const Value target = interpreter_->NewNativeValue("target", [](js::NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr ||
          self->GetKind() != dom::Node::Kind::ProcessingInstruction) {
        return Value::Undefined();
      }
      return Value::String(static_cast<dom::ProcessingInstruction*>(self)->Target());
    });
    if (target.IsObject()) {
      target.object->Set(kOwnerSlot, PointerValue(this));
      processing_instruction.object->DefineAccessor("target", target.object, nullptr);
    }
  }
  // `<!DOCTYPE html>` as a node. Its own interface rather than Document's,
  // which is what it shared until 2026-08-11 -- one line that made
  // `document.doctype instanceof Document` true and `instanceof DocumentType`
  // false.
  const Value document_type = MakeInterface("DocumentType", node);
  if (document_type.IsObject()) {
    const auto doctype_string = [this, &document_type](
                                    const char* name,
                                    const std::string& (dom::DocumentType::*read)() const) {
      const Value getter = interpreter_->NewNativeValue(name, [read](js::NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr || self->GetKind() != dom::Node::Kind::DocumentType) {
          return Value::Undefined();
        }
        return Value::String((static_cast<dom::DocumentType*>(self)->*read)());
      });
      if (getter.IsObject()) {
        getter.object->Set(kOwnerSlot, PointerValue(this));
        document_type.object->DefineAccessor(name, getter.object, nullptr);
      }
    };
    doctype_string("name", &dom::DocumentType::Name);
    doctype_string("publicId", &dom::DocumentType::PublicId);
    doctype_string("systemId", &dom::DocumentType::SystemId);
  }
  // A fragment is a ParentNode: script queries the subtree it is building
  // before it inserts it, which is most of the reason to build it detached.
  const Value fragment = MakeInterface("DocumentFragment", node);
  InstallParentQueries(fragment);
  // `innerHTML` on a fragment, because a shadow root *is* one and
  // `root.innerHTML = …` is how every component fills one. The context element
  // for the parse is the host -- see HtmlParsing.cpp.
  InstallHtmlParsing(fragment);
  // A shadow root *is* a DocumentFragment, and it is one with its own name --
  // which is not a distinction without a difference: a polyfill reparents a
  // fragment onto `ShadowRoot.prototype` to upgrade it in place, which is the
  // line youtube's bundle reaches. `PrototypeFor` tells the two apart by whether
  // the fragment has a host.
  const Value shadow_root = MakeInterface("ShadowRoot", fragment);
  // `host` and `mode` live on ShadowRoot, not on DocumentFragment: a plain
  // fragment has neither, and ShadyDOM's parent-chain walk is
  // `nodeType === DOCUMENT_FRAGMENT_NODE && node.host ? node.host : …`.
  // Without `host`, that walk never climbs out of the root, Polymer cannot
  // find the element it just attached, and a stamped template is stranded.
  if (shadow_root.IsObject()) {
    const Value host = interpreter_->NewNativeValue("host", [this](js::NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsDocumentFragment()) {
        return Value::Null();
      }
      dom::Element* host_element = static_cast<dom::DocumentFragment*>(self)->Host();
      return host_element == nullptr ? Value::Null() : WrapperFor(host_element);
    });
    if (host.IsObject()) {
      host.object->Set(kOwnerSlot, PointerValue(this));
      shadow_root.object->DefineAccessor("host", host.object, nullptr);
    }
    // Open or closed, read from the host that owns this root. A fragment that
    // is not a shadow root has no mode; answering null rather than guessing
    // is what keeps `fragment.mode` from looking like a real root.
    const Value mode = interpreter_->NewNativeValue("mode", [this](js::NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsDocumentFragment()) {
        return Value::Null();
      }
      dom::Element* host_element = static_cast<dom::DocumentFragment*>(self)->Host();
      if (host_element == nullptr) {
        return Value::Null();
      }
      return Value::String(std::string(host_element->ShadowIsOpen() ? "open" : "closed"));
    });
    if (mode.IsObject()) {
      mode.object->Set(kOwnerSlot, PointerValue(this));
      shadow_root.object->DefineAccessor("mode", mode.object, nullptr);
    }
    // The three booleans `attachShadow` took and `<template shadowroot*>` set.
    // Read-only and read off the root itself: they are how it was attached, and
    // nothing may change that afterwards -- `serializable` in particular decides
    // whether `getHTML` will hand this subtree out, so a setter would be a way
    // to opt a tree into serialization after the page decided otherwise.
    const auto shadow_flag = [this, &shadow_root](const char* name, dom::ShadowFlags flag) {
      const Value getter = interpreter_->NewNativeValue(name, [flag](js::NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr || !self->IsDocumentFragment()) {
          return Value::Bool(false);
        }
        const auto& root = *static_cast<dom::DocumentFragment*>(self);
        // A fragment that is not a shadow root is not delegating focus to
        // anything, so false rather than null: these are booleans in the IDL.
        return Value::Bool(root.Host() != nullptr && Any(root.Flags() & flag));
      });
      if (getter.IsObject()) {
        getter.object->Set(kOwnerSlot, PointerValue(this));
        shadow_root.object->DefineAccessor(name, getter.object, nullptr);
      }
    };
    shadow_flag("delegatesFocus", dom::ShadowFlags::DelegatesFocus);
    shadow_flag("clonable", dom::ShadowFlags::Clonable);
    shadow_flag("serializable", dom::ShadowFlags::Serializable);
    // `slotAssignment` is the one that is a string rather than a boolean, and
    // "named" is the default -- so a fragment that is not a shadow root at all
    // answers "named" too, which is what the IDL's default says.
    const Value slot_assignment =
        interpreter_->NewNativeValue("slotAssignment", [](js::NativeCall& call) {
          dom::Node* self = NodeOf(call.self);
          const bool manual = self != nullptr && self->IsDocumentFragment() &&
                              static_cast<dom::DocumentFragment*>(self)->HasManualSlotAssignment();
          return Value::String(manual ? "manual" : "named");
        });
    if (slot_assignment.IsObject()) {
      slot_assignment.object->Set(kOwnerSlot, PointerValue(this));
      shadow_root.object->DefineAccessor("slotAssignment", slot_assignment.object, nullptr);
    }
  }
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
    const Value made = self->CreateElement("img", *self->document_);
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

  // `Audio` is the same shape as `Image`: a constructible factory for
  // `<audio>`, and watch throws `ReferenceError: Audio is not defined` when the
  // name is absent (TD-0020 throw census).
  const Value audio = interpreter_->NewNativeValue("Audio", [self](js::NativeCall& call) {
    const Value made = self->CreateElement("audio", *self->document_);
    if (made.IsObject() && !call.arguments.empty()) {
      if (dom::Node* made_node = NodeOf(made)) {
        static_cast<dom::Element&>(*made_node).SetAttribute("src", js::ToString(call.arguments[0]));
      }
    }
    return made;
  });
  if (audio.IsObject()) {
    if (const Value* prototype = interfaces_.object->GetOwn("HTMLAudioElement")) {
      audio.object->Set("prototype", *prototype);
    }
    interpreter_->Global()->Set("Audio", audio);
    interpreter_->GlobalScope()->Declare("Audio", audio, false);
  }

  // `HTMLUnknownElement`, which is the interface of a tag no specification
  // names. Nothing in this browser is given it: the table above is deliberately
  // short, so "not in the table" means "no interface of its own" rather than
  // "unknown", and defaulting to this would tell a page that `<section>` is a
  // tag the HTML specification has never heard of.
  //
  // It exists as a name because that is the only way it is used. A custom
  // element whose constructor threw is reparented onto `HTMLUnknownElement
  // .prototype` -- the specification says so, and it is the recovery path in
  // every custom-elements polyfill. A page does not feature-detect this one.
  MakeInterface("HTMLUnknownElement", html_element);

  // `Window`, and the global object is an instance of it -- which is what makes
  // `window instanceof Window` and `window instanceof EventTarget` answer the
  // way a page expects, and gives `Window.prototype` somewhere real to be
  // patched. The chain is the specification's: global -> Window -> EventTarget
  // -> Object.
  //
  // The event methods stay on the global itself as well, from
  // InstallWindowEvents. Not redundant: `window` here *is* the global object, so
  // its own properties are the page's globals, and a page that writes
  // `window.addEventListener = f` has to shadow rather than fail.
  const Value window_interface = MakeInterface("Window", event_target);
  if (window_interface.IsObject()) {
    interpreter_->Global()->SetPrototype(window_interface.object);
  }

  // After every interface exists, because a reflected property lands on the
  // prototype of the tag it belongs to.
  InstallReflections();

  InstallFormApis();
  InstallCustomElements();
  if (const Value* document_interface = interfaces_.object->GetOwn("Document")) {
    const Value* shadow_root_interface = interfaces_.object->GetOwn("ShadowRoot");
    const Value empty;
    InstallConstructableStylesheets(*document_interface,
                                    shadow_root_interface != nullptr ? *shadow_root_interface
                                                                     : empty);
    InstallCssOm();
  }
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
  // And the same rule again for storage: a `localStorage` that exists and throws on
  // every write is worse than none, because `if (window.localStorage)` followed by an
  // unguarded write is the shape real code takes. InstallStorage answers that itself.
  InstallStorage();
  // And once more for sockets: a `WebSocket` that never opens is worse than none,
  // because a page waiting on `onopen` has no fallback left. InstallWebSocket answers
  // that itself.
  InstallWebSocket();
  InstallEventSource();
  // And once more for MSE. `MediaSource` present with a `buffered` that never fills is a player
  // stalled with no fallback left; absent, the same player uses `<video src>`, which this browser
  // does have. InstallMediaSource answers that itself.
  InstallMediaSource();
  // Workers, and `structuredClone` beside them because it is the same algorithm. ADR 0022 §1: absent
  // rather than broken when there is no host, because a page that finds `Worker` and gets one whose
  // `onmessage` never fires has no fallback, and one that finds nothing runs its work on the main
  // thread -- slower and correct.
  InstallWorker();
  InstallStructuredClone();
  // Absent for the same reason when there is nothing to traverse: a page that
  // finds `history.pushState` and gets nothing has already taken the branch that
  // assumes it works. InstallHistory answers that itself.
  InstallHistory();
  InstallWindowEvents();
}

js::Value DomBindings::InterfaceNamed(const char* name) {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  const Value* found = interfaces_.object->GetOwn(name);
  return found == nullptr ? Value::Undefined() : *found;
}

js::Value DomBindings::DocumentInterface() { return InterfaceNamed("Document"); }

js::Value DomBindings::PrototypeFor(const dom::Node& node) {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  const auto named = [this](const char* name) -> Value {
    // `instanceof` reads the live constructor's `.prototype`. `interfaces_`
    // holds ours from `MakeInterface`, which diverges the moment a polyfill
    // replaces `window.ShadowRoot` -- attachShadow wrappers then fail
    // `instanceof ShadowRoot` and ShadyDOM keeps the shady path on youtube.com.
    if (js::Value* constructor = interpreter_->GlobalScope()->Lookup(name)) {
      if (constructor->IsObject()) {
        if (const Value* prototype = constructor->object->Get("prototype")) {
          if (prototype->IsObject()) {
            return *prototype;
          }
        }
      }
    }
    const Value* found = interfaces_.object->GetOwn(name);
    return found == nullptr ? Value::Undefined() : *found;
  };

  switch (node.GetKind()) {
    case dom::Node::Kind::Text:
      return named(static_cast<const dom::Text&>(node).IsCData() ? "CDATASection" : "Text");
    case dom::Node::Kind::Comment:
      return named("Comment");
    case dom::Node::Kind::DocumentFragment:
      // A fragment with a host is a shadow root. That is the whole difference
      // between the two in this tree, and it is the specification's difference
      // too.
      return named(static_cast<const dom::DocumentFragment&>(node).Host() != nullptr
                       ? "ShadowRoot"
                       : "DocumentFragment");
    case dom::Node::Kind::Document:
      return named("Document");
    case dom::Node::Kind::DocumentType:
      // Its own interface. It shared Document's until 2026-08-11, which made
      // `document.doctype instanceof Document` true and `instanceof
      // DocumentType` false -- two wrong answers from one line.
      return named("DocumentType");
    case dom::Node::Kind::ProcessingInstruction:
      return named("ProcessingInstruction");
    case dom::Node::Kind::Element:
      break;
  }

  // **The tag decides the interface only inside the HTML namespace.**
  //
  // `createElementNS("http://example.com/", "a")` is not an anchor. It is an
  // `Element` with the local name `a`, and every member HTML puts on
  // `HTMLAnchorElement` -- `href`, `relList`, `target` -- must be *undefined*
  // on it. Choosing by tag name alone gave a foreign element the whole HTML
  // interface, which is not a missing feature but an invented one: a page that
  // feature-detects `el.relList` to decide whether it is looking at a link
  // gets yes for a MathML `<a>`.
  //
  // SVG has its own hierarchy and this browser has one interface for it, so an
  // SVG element is an `SVGElement`; anything else foreign is a bare `Element`.
  const auto& element = static_cast<const dom::Element&>(node);
  if (!element.Namespace().IsHtml()) {
    return named(element.Namespace().Uri() == "http://www.w3.org/2000/svg" ? "SVGElement"
                                                                        : "Element");
  }
  const char* interface = InterfaceForTag(element.TagName());
  return *interface == '\0' ? named("HTMLElement") : named(interface);
}

}  // namespace microbrowser::bindings

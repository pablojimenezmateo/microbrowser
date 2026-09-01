#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/DocumentFacts.h"
#include "bindings/FontLoading.h"
#include "bindings/WebIdl.h"
#include "html/Encoding.h"
#include "html/TreeBuilder.h"
#include "xml/XmlParser.h"
#include "xml/XmlSerializer.h"

// `DOMParser`, `XMLSerializer`, `Range.createContextualFragment`, and the
// handful of things a page reads off a document that is *not* the page.
//
// **This file only became writable when `document` stopped meaning "the one
// tree this binding layer was built around".** `DOMParser.parseFromString`
// returns a Document, and while `document.getElementById`, `document.body` and
// the rest answered about the page whatever they were called on, a second
// Document would have been an object that looked right and lied -- the stub
// ADR 0012 calls worse than an absence, which is exactly why this API was
// absent. Every `document.*` operation now resolves against its **receiver**
// (`DomBindings::DocumentOf`), which is what `createHTMLDocument` already
// relied on, and this is the second caller that proves it.
//
// Two decisions are worth reading before changing anything here.
//
// **XML is parsed, not approximated.** `parseFromString(s, "text/xml")` goes to
// `xml::ParseXml`, a real well-formedness-checking parser in a module of its
// own. The alternative that keeps suggesting itself -- run the HTML tokenizer
// over the string and call it done -- produces a tree no other browser
// produces, silently, for every input: `<foo/>` becomes an *open* `<foo>` in
// the HTML namespace, `<Foo>` is folded to lowercase, an undeclared prefix is
// part of the tag name. And the failure half is not a detail either: XML says a
// well-formedness error is fatal and this API says a fatal error is a document
// containing one `parsererror` element, so "was this well-formed" is a question
// something has to actually answer.
//
// **A parsed document is inert.** Nothing in it is fetched, no script in it
// runs, no custom element in it is upgraded, and it has no browsing context --
// `defaultView` and `location` are null, which is the distinction that stops a
// page from using one as a second window. That inertness is the whole point of
// the API: it is what every sanitizer and every feed reader is built on.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// A document's own address, when it is not the page's. Only `new Document()`
// has one -- it is `about:blank`, because a document nothing parsed and nothing
// loaded has no address of its own to inherit. A `DOMParser` document carries
// no slot and answers with the *creating* document's URL, which is the
// specification and is what makes relative URLs in parsed markup resolve
// against the page that parsed them.

// The Range boundary slot Ranges.cpp writes. Named again here rather than
// shared, because a `constexpr const char*` in a header would be the only thing
// those two files had in common -- and `createContextualFragment` belongs to
// the parsing specification rather than to Range's own.
constexpr const char* kRangeStartNodeSlot = "#rangeStartNode";

// `DOMParserSupportedType`. WebIDL enumerations are exact-match and
// case-sensitive, and anything outside the list is a TypeError before the
// string is looked at -- which is the one negative test the API has.
bool IsHtmlType(std::string_view type) { return type == "text/html"; }

bool IsSupportedType(std::string_view type) {
  return type == "text/html" || type == "text/xml" || type == "application/xml" ||
         type == "application/xhtml+xml" || type == "image/svg+xml";
}

// The content type a document reports, defaulting to `text/html` -- which is
// right for the page's own document and for anything `createHTMLDocument` made.
std::string ContentTypeOf(const Value& self) {
  if (self.IsObject()) {
    const js::Object* behind = BehindProxies(self.object);
    const js::Value* slot = behind == nullptr ? nullptr : behind->GetOwn(kContentTypeSlot);
    if (slot != nullptr && slot->IsString()) {
      return slot->AsString();
    }
  }
  return "text/html";
}

}  // namespace

void DomBindings::InstallDomParsing() {
  const Value document_interface = DocumentInterface();
  if (!document_interface.IsObject() || interpreter_ == nullptr) {
    return;
  }

  // --- what a page reads off a document that is not the page ----------------
  //
  // These were on the page's own document wrapper as *data properties*, which
  // meant a second document answered `undefined` to every one of them. They are
  // accessors on `Document.prototype` now, so the answer follows the receiver
  // like every other `document.*` operation.
  const auto document_accessor = [this, &document_interface](const char* name,
                                                             js::NativeFunction get) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      document_interface.object->DefineAccessor(name, getter.object, nullptr);
    }
  };

  // The URL, and it is the *creating* document's for anything DOMParser made:
  // a parsed document has no address of its own, so relative URLs in it resolve
  // against the page that parsed it. Three names for it, because the DOM, the
  // old DOM Level 3 and the base-URL machinery each named it something else.
  for (const char* name : {"URL", "documentURI", "baseURI"}) {
    document_accessor(name, [](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      const js::Object* behind = call.self.IsObject() ? BehindProxies(call.self.object) : nullptr;
      const js::Value* own = behind == nullptr ? nullptr : behind->GetOwn(kDocumentUrlSlot);
      if (own != nullptr && own->IsString()) {
        return Value::String(own->AsString());
      }
      return Value::String(owner == nullptr ? std::string() : owner->url_);
    });
  }

  // The encoding the document was decoded with, which for a loaded page is the
  // sniffer's answer and for anything `DOMParser` / `createHTMLDocument` made
  // is UTF-8 -- those take a string, so there is nothing left to decode.
  // Three names, and `inputEncoding` is the legacy one. Read off the wrapper
  // rather than the running realm: a parent reading `contentDocument` is not
  // in the child's realm, and the child's encoding lives on the child's document.
  for (const char* name : {"characterSet", "charset", "inputEncoding"}) {
    document_accessor(name, [](NativeCall& call) {
      html::Encoding encoding = html::Encoding::Utf8;
      if (call.self.IsObject()) {
        const js::Object* behind = BehindProxies(call.self.object);
        if (behind != nullptr) {
          encoding = DocumentEncodingOf(*behind);
        }
      }
      return Value::String(std::string(html::EncodingName(encoding)));
    });
  }

  // `document.fonts`. Only the readiness half exists -- see src/bindings/FontLoading.h for why it
  // exists at all (714 test files are blocked on it) and for what is deliberately absent.
  document_accessor("fonts", [](NativeCall& call) {
    js::Object* behind = call.self.IsObject() ? BehindProxies(call.self.object) : nullptr;
    if (behind == nullptr) {
      return Value::Undefined();
    }
    js::Object* set = FontFaceSetFor(call.interpreter, *behind);
    return set == nullptr ? Value::Undefined() : Value::Obj(set);
  });

  document_accessor("contentType",
                    [](NativeCall& call) { return Value::String(ContentTypeOf(call.self)); });

  // Quirks mode under the name a page reads it by. It comes from the doctype,
  // so a fragment of markup with none is `BackCompat` -- which is what makes
  // this observable at all from `DOMParser`.
  document_accessor("compatMode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const dom::Document* document = owner == nullptr ? nullptr : owner->DocumentOf(call.self);
    const bool quirks = document != nullptr && document->InQuirksMode();
    return Value::String(std::string(quirks ? "BackCompat" : "CSS1Compat"));
  });

  // `document.location`, which is the *window's* location and null for a
  // document with no browsing context. Null rather than absent, and the
  // difference matters here more than usual: a page that finds `undefined`
  // reads `.href` off it and throws, where null is what the specification says
  // and what `if (doc.location)` is written against.
  document_accessor("location", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr || owner->DocumentOf(call.self) != owner->MainDocument()) {
      return Value::Null();
    }
    const js::Value* location = call.interpreter.GlobalScope()->Lookup("location");
    return location == nullptr ? Value::Null() : *location;
  });

  // `new Document()` and `new DocumentFragment()`, which `MakeInterface` left
  // as the illegal-constructor stub every other interface gets -- correct for
  // `new HTMLDivElement()` and wrong for these two, which the DOM makes
  // constructible on purpose. A `new Document()` is an **XML** document: it has
  // no doctype, no documentElement and no HTML semantics, and
  // `doc.createElement("x")` in it makes an element in no namespace.
  const auto constructible = [this](const char* name, js::NativeFunction make) {
    const Value prototype = InterfaceNamed(name);
    const Value constructor = interpreter_->NewNativeValue(name, std::move(make));
    if (!prototype.IsObject() || !constructor.IsObject()) {
      return;
    }
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->Set("constructor", constructor);
    interpreter_->Global()->Set(name, constructor);
    interpreter_->GlobalScope()->Declare(name, constructor, false);
  };
  constructible("Document", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    auto made = std::make_unique<dom::Document>();
    dom::Document* raw = made.get();
    raw->SetDocumentKind(dom::Document::DocumentKind::Xml);
    owner->unattached_.push_back(std::move(made));
    const Value wrapper = owner->WrapperFor(raw);
    if (wrapper.IsObject()) {
      wrapper.object->SetHidden(kContentTypeSlot, Value::String(std::string("application/xml")));
      wrapper.object->SetHidden(kDocumentUrlSlot, Value::String(std::string("about:blank")));
      wrapper.object->SetHidden(kReadyStateSlot, Value::String(std::string("complete")));
    }
    return wrapper;
  });
  constructible("DocumentFragment", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null() : owner->CreateDocumentFragment(*owner->document_);
  });

  // `XMLDocument`, which exists so that `doc instanceof XMLDocument` is a
  // question rather than a ReferenceError. Nothing is an instance of it:
  // `parseFromString` returns a `Document` for every one of its types, which is
  // what the suite checks and what the interface's absence would have hidden.
  MakeInterface("XMLDocument", document_interface);

  // --- DOMParser -------------------------------------------------------------
  const Value parser_interface = MakeInterface("DOMParser", Value::Undefined());
  if (!parser_interface.IsObject()) {
    return;
  }
  const Value parse_from_string =
      interpreter_->NewNativeValue("parseFromString", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (!RequireArguments(call, "DOMParser", "parseFromString", 2)) {
          return call.ThrownValue();
        }
        std::string markup;
        std::string type;
        if (!ToDomString(call, call.arguments[0], markup) ||
            !ToDomString(call, call.arguments[1], type)) {
          return call.ThrownValue();
        }
        if (!IsSupportedType(type)) {
          return call.Throw("TypeError",
                            "'" + type + "' is not a valid value for DOMParserSupportedType");
        }
        if (owner == nullptr) {
          return Value::Null();
        }
        std::unique_ptr<dom::Document> made =
            IsHtmlType(type) ? html::ParseDocument(markup) : xml::ParseXml(markup).document;
        if (made == nullptr) {
          return Value::Null();
        }
        dom::Document* raw = made.get();
        // The document's *type*, which is what `createElement` and `tagName`
        // branch on. Held on the document rather than beside the content type
        // below, because both of those questions are asked from inside the tree
        // where no wrapper is in reach.
        raw->SetDocumentKind(IsHtmlType(type) ? dom::Document::DocumentKind::Html
                                             : dom::Document::DocumentKind::Xml);
        // Owned here for the life of the page, exactly like a node script made
        // and never appended: a node's owner is its parent and a document has
        // none, and a wrapper holds a raw pointer, so the tree may not outlive
        // this. See createHTMLDocument, which owns its document the same way.
        //
        // **This list only grows**, and `DOMParser` makes it much easier to
        // drive than `createHTMLDocument` did -- a feed reader parsing in a loop
        // keeps every document it has ever seen. Fixing it means teaching the
        // collector about node lifetimes, which is a decision about the heap
        // (ADR 0034) rather than about this API, and it is owed.
        owner->unattached_.push_back(std::move(made));
        const Value wrapper = owner->WrapperFor(raw);
        if (wrapper.IsObject()) {
          wrapper.object->SetHidden(kContentTypeSlot, Value::String(type));
          // Finished the moment it exists: there is no load behind it and no
          // parser still running in it.
          wrapper.object->SetHidden(kReadyStateSlot, Value::String(std::string("complete")));
        }
        return wrapper;
      });
  if (parse_from_string.IsObject()) {
    parse_from_string.object->Set(kOwnerSlot, OwnerValue(this));
    parser_interface.object->Set("parseFromString", parse_from_string);
  }
  const Value parser_constructor =
      interpreter_->NewNativeValue("DOMParser", [parser_interface](NativeCall& call) -> Value {
        const Value made = call.interpreter.NewObjectValue();
        if (made.IsObject()) {
          made.object->SetPrototype(parser_interface.object);
        }
        return made;
      });
  if (parser_constructor.IsObject()) {
    parser_constructor.object->Set("prototype", parser_interface);
    parser_interface.object->Set("constructor", parser_constructor);
    interpreter_->Global()->Set("DOMParser", parser_constructor);
    interpreter_->GlobalScope()->Declare("DOMParser", parser_constructor, false);
  }

  // --- XMLSerializer ---------------------------------------------------------
  const Value serializer_interface = MakeInterface("XMLSerializer", Value::Undefined());
  if (!serializer_interface.IsObject()) {
    return;
  }
  const Value serialize =
      interpreter_->NewNativeValue("serializeToString", [](NativeCall& call) -> Value {
        if (!RequireArguments(call, "XMLSerializer", "serializeToString", 1)) {
          return call.ThrownValue();
        }
        dom::Node* node = NodeOf(call.arguments[0]);
        if (node == nullptr) {
          return call.Throw("TypeError", "serializeToString expects a Node");
        }
        return Value::String(xml::SerializeXml(*node));
      });
  if (serialize.IsObject()) {
    serialize.object->Set(kOwnerSlot, OwnerValue(this));
    serializer_interface.object->Set("serializeToString", serialize);
  }
  const Value serializer_constructor = interpreter_->NewNativeValue(
      "XMLSerializer", [serializer_interface](NativeCall& call) -> Value {
        const Value made = call.interpreter.NewObjectValue();
        if (made.IsObject()) {
          made.object->SetPrototype(serializer_interface.object);
        }
        return made;
      });
  if (serializer_constructor.IsObject()) {
    serializer_constructor.object->Set("prototype", serializer_interface);
    serializer_interface.object->Set("constructor", serializer_constructor);
    interpreter_->Global()->Set("XMLSerializer", serializer_constructor);
    interpreter_->GlobalScope()->Declare("XMLSerializer", serializer_constructor, false);
  }

  // --- Range.createContextualFragment ---------------------------------------
  const Value range_interface = InterfaceNamed("Range");
  if (!range_interface.IsObject()) {
    return;
  }
  // `range.createContextualFragment(markup)`.
  //
  // It is the fragment parsing algorithm again, with the *context element*
  // taken from where the range starts -- and the two special cases below are
  // the whole of what makes it different from `innerHTML`:
  //
  //   * a range that starts in a Document or a DocumentFragment has no element
  //     to be a context, and
  //   * a range that starts in `<html>` uses `body`'s rules instead, which is
  //     what stops `<span>x</span>` at the top of a document from being dropped
  //     by "before head" and is the compat bug this API is famous for.
  //
  // Both land on the same substitute -- a `body` context -- so there is one
  // branch rather than two.
  const Value create = interpreter_->NewNativeValue(
      "createContextualFragment", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (!RequireArguments(call, "Range", "createContextualFragment", 1)) {
          return call.ThrownValue();
        }
        std::string markup;
        if (!ToDomString(call, call.arguments[0], markup)) {
          return call.ThrownValue();
        }
        if (owner == nullptr || !call.self.IsObject()) {
          return Value::Null();
        }
        const js::Object* behind = BehindProxies(call.self.object);
        const js::Value* slot =
            behind == nullptr ? nullptr : behind->GetOwn(kRangeStartNodeSlot);
        dom::Node* start = slot == nullptr || !slot->IsNumber()
                               ? nullptr
                               : reinterpret_cast<dom::Node*>(
                                     static_cast<std::uintptr_t>(slot->number));
        if (start == nullptr) {
          return Value::Null();
        }
        // The context element: the start node itself when it is one, its parent
        // element when it is a text node or a comment, and nothing at all for a
        // document or a fragment.
        const dom::Element* context = nullptr;
        if (start->IsElement()) {
          context = static_cast<const dom::Element*>(start);
        } else if (start->Parent() != nullptr && start->Parent()->IsElement()) {
          context = static_cast<const dom::Element*>(start->Parent());
        }
        std::string context_tag = "body";
        if (context != nullptr && !(context->Namespace().IsHtml() &&
                                    context->LocalName() == "html")) {
          context_tag = context->TagName();
        }
        dom::Document& document = owner->NodeDocumentOf(*start);
        std::unique_ptr<dom::DocumentFragment> parsed =
            html::ParseFragment(markup, context_tag, document.InQuirksMode());
        if (parsed == nullptr) {
          return Value::Null();
        }
        // The nodes are moved into a fragment this layer owns, so that what a
        // page gets back is a `DocumentFragment` wrapper over a node whose
        // lifetime is the document's -- the same ownership every unparented
        // node script makes has.
        const Value fragment = owner->CreateDocumentFragment(document);
        dom::Node* target = NodeOf(fragment);
        if (target == nullptr) {
          return Value::Null();
        }
        owner->InsertFragmentChildren(*target, *parsed, nullptr);
        return fragment;
      });
  if (create.IsObject()) {
    create.object->Set(kOwnerSlot, OwnerValue(this));
    range_interface.object->Set("createContextualFragment", create);
  }
}

}  // namespace microbrowser::bindings

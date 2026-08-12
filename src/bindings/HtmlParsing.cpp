#include <memory>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/WebIdl.h"
#include "html/TreeBuilder.h"
#include "util/PerformanceCounters.h"

// A string of markup becoming nodes: `innerHTML`, `outerHTML`,
// `insertAdjacentHTML` and `<template>`'s contents.
//
// Its own translation unit because it is the module's most hostile entry point
// and deserves to be readable in one piece. Everything a page can put in a
// string arrives here, and it arrives with a *context element* the page also
// chose -- `ParseFragment` is where the two meet, and the reason that call
// happens in one place rather than four is that "was the parser given the right
// context" has to be a question with one answer.
//
// What this deliberately does not do:
//
//   * It does not run scripts. `engine::PageScript::Collect` gathers a
//     document's scripts once, when the document is parsed, so a `<script>`
//     that arrives through `innerHTML` is an element and nothing more. That
//     matches the specification, and it is the property that stops
//     `el.innerHTML = userText` from being arbitrary code execution.
//   * It does not sanitize. There is no safe-HTML type here, and the
//     serializers in `src/dom` are not sanitizers -- a round trip through one
//     is not a security boundary and their comments say so.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where an element's HTML lives: a template's contents, and the element itself
// for everything else. Both `innerHTML` halves and `insertAdjacentHTML` ask
// this, because a template whose `innerHTML` wrote children rather than
// contents would put markup back into the document the element exists to keep
// it out of.
dom::Node& HtmlHost(dom::Element& element) {
  dom::DocumentFragment* content = element.Content();
  return content == nullptr ? static_cast<dom::Node&>(element) : static_cast<dom::Node&>(*content);
}

// The child of `parent` after `child`, or null. A scan rather than a stored
// pointer, because `dom::Node` keeps its children in a vector and a sibling
// link would be a second invariant to maintain across every insertion.
dom::Node* NextSibling(const dom::Node& parent, const dom::Node& child) {
  bool found = false;
  for (const std::unique_ptr<dom::Node>& candidate : parent.Children()) {
    if (found) {
      return candidate.get();
    }
    found = candidate.get() == &child;
  }
  return nullptr;
}

}  // namespace

void DomBindings::UpgradeSubtree(dom::Node& node) {
  const Value registry = CustomElementRegistry();
  if (!registry.IsObject() || registry.object->Keys().empty()) {
    return;  // no custom elements defined: nothing to upgrade, and no walk to do
  }
  if (node.IsElement()) {
    UpgradeElement(static_cast<dom::Element&>(node));
    // A `<template>`'s markup lives in `Content()`, not in `Children()`, so the
    // walk below never enters it. That is load-bearing: browsers leave template
    // contents inert until stamped (see InsertFragmentChildren).
  }
  // By index, re-reading the child list each time: an upgrade runs the page's
  // constructor, and a constructor that moves a node out of this subtree would
  // leave a range-for holding an iterator into a vector that has moved.
  for (std::size_t i = 0; i < node.Children().size(); ++i) {
    UpgradeSubtree(*node.Children()[i]);
  }
}

void DomBindings::InsertFragmentChildren(dom::Node& parent, dom::Node& fragment,
                                         dom::Node* reference, bool record) {
  // Upgraded first, while the nodes are still in the fragment and therefore
  // still out of the document. Two things depend on that order: a
  // `connectedCallback` is a method of the upgraded class, so an element that
  // ran one before its constructor would see a half-built object; and
  // `UpgradeElement` fires the connect itself for an element that is *already*
  // in the document, which is what `customElements.define` needs -- upgrading
  // after the move would fire it there and again below.
  //
  // Custom elements inside `<template>.content` stay inert until stamped
  // (HTML; TD-0017). Upgrading them here stripped Polymer `[[…]]` before
  // `_parseTemplate`. ShadyDOM roots are not template content — they set
  // `Host()` and still upgrade. Stamp cost after this is TD-0018.
  if (!(parent.GetKind() == dom::Node::Kind::DocumentFragment &&
        static_cast<const dom::DocumentFragment&>(parent).IsTemplateContent())) {
    for (std::size_t i = 0; i < fragment.Children().size(); ++i) {
      UpgradeSubtree(*fragment.Children()[i]);
    }
  } else {
    util::AddPerformanceCounter(util::PerfCounterId::DomTemplateContentUpgradeSkips);
  }
  // The fragment loses its children, and an observer of the *fragment* is owed
  // that. The DOM queues this record inside "insert" regardless of the suppress
  // observers flag, which only covers the record for the receiving parent --
  // the fragment being emptied is not part of the replacement the flag is
  // hiding. `record` is deliberately not consulted here.
  if (const std::vector<dom::Node*> emptied = ChildrenOf(fragment); !emptied.empty()) {
    RecordMutation(fragment, "childList", {}, Value::Null(), {}, emptied);
  }
  std::vector<dom::Node*> added;
  while (dom::Node* first = fragment.FirstChild()) {
    // The removal half, for the same reason `InsertInto` owes it: `UpgradeSubtree` above ran the
    // page's constructors, and a constructor is allowed to put its own element in the document.
    // Moving it here without saying so left a custom element connected twice over.
    NotifyConnection(*first, false);
    if (first->Parent() != &fragment) {
      continue;  // a reaction moved it out from under us; the loop re-reads the new first child
    }
    std::unique_ptr<dom::Node> moved = fragment.Detach(first);
    if (moved == nullptr) {
      break;
    }
    added.push_back(moved.get());
    parent.InsertBefore(std::move(moved), reference);
  }
  if (added.empty()) {
    return;
  }
  for (dom::Node* node : added) {
    NotifyConnection(*node, true);
  }
  // One record for the batch rather than one per node, which is what the
  // specification's "insert a node" produces for a fragment and what an
  // observer counting childList records is written against.
  if (record) {
    RecordMutation(parent, "childList", {}, Value::Null(), added, {});
  }
}

void DomBindings::InsertParsedHtml(std::string_view context_tag_name, dom::Node& parent,
                                   dom::Node* reference, const std::string& markup,
                                   bool allow_declarative_shadow_roots) {
  // Quirks mode is carried in because it changes the tree: a `<table>` does not
  // close an open `<p>` in quirks mode, and a fragment that assumed standards
  // mode would build a different tree from the document it is going into.
  std::unique_ptr<dom::DocumentFragment> parsed =
      html::ParseFragment(markup, context_tag_name, document_ != nullptr &&
                                                        document_->InQuirksMode(),
                          allow_declarative_shadow_roots);
  if (parsed == nullptr) {
    return;
  }
  InsertFragmentChildren(parent, *parsed, reference);
}

void DomBindings::InstallHtmlParsing(const js::Value& element_interface) {
  if (!element_interface.IsObject()) {
    return;
  }
  const auto accessor = [this, &element_interface](const char* name, js::NativeFunction get,
                                                   js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      setter.object->Set(kOwnerSlot, OwnerValue(this));
      element_interface.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  accessor(
      "innerHTML",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr) {
          return Value::Undefined();
        }
        // A template answers with its contents, which is where its markup is.
        return Value::String(self->IsElement()
                                 ? HtmlHost(static_cast<dom::Element&>(*self)).SerializeChildren()
                                 : self->SerializeChildren());
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        // A shadow root is a DocumentFragment, and `root.innerHTML = …` is how
        // every component fills one. Its fragment-parsing *context element* is
        // the **host**, which is what makes `<td>` inside a `<tr>`-hosted root a
        // cell rather than bare text -- ADR 0019 §1 with ADR 0020 §6's rule that
        // the context is the whole algorithm.
        dom::Node* target = self;
        std::string context = "div";
        if (self->IsElement()) {
          auto& element = static_cast<dom::Element&>(*self);
          target = &HtmlHost(element);
          context = element.TagName();
        } else if (self->GetKind() == dom::Node::Kind::DocumentFragment) {
          const dom::Element* shadow_host =
              static_cast<dom::DocumentFragment*>(self)->Host();
          if (shadow_host != nullptr) {
            context = shadow_host->TagName();
          }
        } else {
          return Value::Undefined();
        }
        // Read before anything is torn down: the conversion can call a page's
        // own `toString`, and that runs script which may move this element.
        //
        // `[LegacyNullToEmptyString]`, which is on the IDL declaration rather
        // than on DOMString: `el.innerHTML = null` empties the element, where
        // WebIDL's plain conversion would have written the four characters
        // "null" into it. `js::ToString` was doing neither -- it is a *pure*
        // conversion that cannot call a page's `toString` at all, so an object
        // became the literal "[object Object]" (see WebIdl.h).
        std::string markup;
        if (!ToDomStringOrEmptyForNull(call, Argument(call.arguments, 0), markup)) {
          return call.ThrownValue();
        }
        owner->ClearChildren(*target);
        owner->InsertParsedHtml(context, *target, nullptr, markup);
        return Value::Undefined();
      });

  accessor(
      "outerHTML",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        return self == nullptr ? Value::Undefined() : Value::String(self->Serialize());
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        dom::Node* parent = self->Parent();
        if (parent == nullptr || parent->GetKind() == dom::Node::Kind::Document) {
          // The specification's NoModificationAllowedError. A node with no
          // parent has no place to put a replacement, and silently doing
          // nothing is how a page ends up debugging the wrong line. A node
          // whose parent is the *document* is the same refusal: replacing
          // `documentElement` with a fragment would mean parsing markup with
          // no context element and inserting whatever came out beside the
          // doctype.
          return ThrowDom(call, "NoModificationAllowedError",
                            "outerHTML on a node with no element parent");
        }
        // §"the fragment parsing algorithm" for outerHTML: the context is the
        // *parent*, because that is where the nodes are going. A parent that is
        // not an element -- the document, a fragment -- uses body's rules,
        // which is what the specification says and what keeps `<td>` from
        // becoming a cell at the top of a document.
        std::string markup;
        if (!ToDomStringOrEmptyForNull(call, Argument(call.arguments, 0), markup)) {
          return call.ThrownValue();
        }
        owner->ReplaceWithParsedHtml(*parent, *self, markup);
        return Value::Undefined();
      });

  const auto method = [this, &element_interface](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      element_interface.object->Set(name, native);
    }
  };

  // `setHTMLUnsafe`: `innerHTML` plus the declarative-shadow-root opt-in, and
  // the name is the API. It is the *only* string-to-tree entry point that builds
  // shadow roots, which is why the opt-in is a parameter threaded from here
  // rather than a mode: `innerHTML` two lines up must not acquire it by accident.
  //
  // "Unsafe" is about the shadow roots and not about scripts -- a `<script>`
  // that arrives through any of these paths still does not run, because
  // `PageScript::Collect` gathers a document's scripts once when the document is
  // parsed. There is no sanitizer argument here yet, so the name is currently
  // the whole of the warning.
  method("setHTMLUnsafe", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "setHTMLUnsafe called on something that is not a node");
    }
    // The same context-element choice `innerHTML` makes, and for the same
    // reason: a shadow root parses against its *host*, so `<td>` inside a root
    // hosted by a `<tr>` is a cell rather than bare text.
    dom::Node* target = self;
    std::string context = "div";
    if (self->IsElement()) {
      auto& element = static_cast<dom::Element&>(*self);
      target = &HtmlHost(element);
      context = element.TagName();
    } else if (self->GetKind() == dom::Node::Kind::DocumentFragment) {
      const dom::Element* shadow_host = static_cast<dom::DocumentFragment*>(self)->Host();
      if (shadow_host != nullptr) {
        context = shadow_host->TagName();
      }
    } else {
      return Value::Undefined();
    }
    std::string markup;
    if (!ToDomStringOrEmptyForNull(call, Argument(call.arguments, 0), markup)) {
      return call.ThrownValue();
    }
    owner->ClearChildren(*target);
    owner->InsertParsedHtml(context, *target, nullptr, markup,
                            /*allow_declarative_shadow_roots=*/true);
    return Value::Undefined();
  });

  // `getHTML({serializableShadowRoots, shadowRoots})`: `innerHTML`'s getter with
  // a say in which shadow trees come with it. With no options it *is*
  // `innerHTML`, which is the property the suite checks in both directions --
  // a shadow root must never appear in the string unless the page asked twice
  // over, either by marking the root serializable or by naming it here.
  method("getHTML", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr) {
      return call.Throw("TypeError", "getHTML called on something that is not a node");
    }
    dom::SerializeOptions options;
    std::vector<const dom::DocumentFragment*> named;
    const Value arguments = Argument(call.arguments, 0);
    if (arguments.IsObject()) {
      if (const Value* serializable = arguments.object->Get("serializableShadowRoots")) {
        options.serializable_shadow_roots = js::ToBoolean(*serializable);
      }
      if (const Value* roots = arguments.object->Get("shadowRoots"); roots != nullptr &&
                                                                     roots->IsObject()) {
        const std::size_t count = roots->object->ElementCount();
        for (std::size_t i = 0; i < count; ++i) {
          const Value entry = roots->object->GetElement(i);
          dom::Node* node = NodeOf(entry);
          // Only an actual shadow root counts. A page naming a plain fragment
          // here is naming something that is not in the tree being serialized,
          // and silently serializing it would put nodes in the output that are
          // nowhere under the element asked about.
          if (node != nullptr && node->IsDocumentFragment() &&
              static_cast<dom::DocumentFragment*>(node)->Host() != nullptr) {
            named.push_back(static_cast<const dom::DocumentFragment*>(node));
          }
        }
      }
    }
    if (!named.empty()) {
      options.shadow_roots = &named;
    }
    // "Serialize an HTML fragment", which resolves a template to its contents
    // *and* emits the receiver's own shadow root before its children -- the
    // element being asked is a host as much as any element beneath it.
    return Value::String(dom::SerializeFragment(*self, options));
  });

  method("insertAdjacentHTML", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "insertAdjacentHTML called on a non-element");
    }
    auto& element = static_cast<dom::Element&>(*self);
    if (!RequireArguments(call, "Element", "insertAdjacentHTML", 2)) {
      return call.ThrownValue();
    }
    std::string position;
    std::string markup;
    if (!ToDomString(call, call.arguments[0], position) ||
        !ToDomString(call, call.arguments[1], markup)) {
      return call.ThrownValue();
    }
    position = LowerCase(position);

    if (position == "afterbegin" || position == "beforeend") {
      dom::Node& host = HtmlHost(element);
      owner->InsertParsedHtml(element.TagName(), host,
                              position == "afterbegin" ? host.FirstChild() : nullptr, markup);
      return Value::Undefined();
    }
    if (position != "beforebegin" && position != "afterend") {
      // The specification's SyntaxError, and it is worth throwing rather than
      // guessing: the four names are easy to mistype and a silent no-op looks
      // exactly like markup the parser dropped.
      return ThrowDom(call, "SyntaxError", "'" + position + "' is not a valid insert position");
    }
    dom::Node* parent = element.Parent();
    if (parent == nullptr || parent->GetKind() == dom::Node::Kind::Document) {
      // Both halves are the specification's, and the document one is the
      // interesting half: `document.documentElement.insertAdjacentHTML(
      // "beforebegin", …)` would put nodes beside `<html>` at the top of a
      // document, which is a tree the parser can never produce.
      return ThrowDom(call, "NoModificationAllowedError",
                        "insertAdjacentHTML '" + position + "' on a node with no element parent");
    }
    // The context is the parent, for the reason outerHTML's is.
    dom::Node* reference =
        position == "beforebegin" ? &element : NextSibling(*parent, element);
    owner->InsertAdjacentParsedHtml(*parent, reference, markup);
    return Value::Undefined();
  });

  // `insertAdjacentElement` and `insertAdjacentText`, over the DOM's "insert
  // adjacent" -- which is *not* the algorithm `insertAdjacentHTML` above uses,
  // and the difference is the only interesting thing about them: a
  // beforebegin/afterend insertion on a node with no parent returns null here
  // and throws NoModificationAllowedError there. Two algorithms, because one
  // parses markup into a context and the other places a node that already
  // exists.
  //
  // `insertAdjacentText` is the one that earned this: testharness.js's own
  // `show_results` calls it, so a *missing* four-line method meant every WPT
  // page whose tests had all run reported nothing at all. It is invisible on
  // every real page and it silently destroyed a reporting path (ADR 0040).
  const auto insert_adjacent = [](NativeCall& call, const char* operation,
                                  dom::Node* node) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement() || node == nullptr) {
      return call.Throw("TypeError", std::string(operation) + " called on a non-element");
    }
    auto& element = static_cast<dom::Element&>(*self);
    std::string position;
    if (!ToDomString(call, Argument(call.arguments, 0), position)) {
      return call.ThrownValue();
    }
    position = LowerCase(position);
    if (position == "afterbegin") {
      return owner->InsertNodeBefore(element, node, element.FirstChild());
    }
    if (position == "beforeend") {
      return owner->InsertNodeBefore(element, node, nullptr);
    }
    if (position != "beforebegin" && position != "afterend") {
      return ThrowDom(call, "SyntaxError", "'" + position + "' is not a valid insert position");
    }
    dom::Node* parent = element.Parent();
    if (parent == nullptr) {
      return Value::Null();  // and deliberately not a throw -- see above
    }
    return owner->InsertNodeBefore(
        *parent, node, position == "beforebegin" ? &element : NextSibling(*parent, element));
  };

  method("insertAdjacentElement", [insert_adjacent](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Element", "insertAdjacentElement", 2)) {
      return call.ThrownValue();
    }
    dom::Node* node = NodeOf(call.arguments[1]);
    if (node == nullptr || !node->IsElement()) {
      return call.Throw("TypeError", "insertAdjacentElement requires an element");
    }
    return insert_adjacent(call, "insertAdjacentElement", node);
  });

  method("insertAdjacentText", [insert_adjacent](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (!RequireArguments(call, "Element", "insertAdjacentText", 2)) {
      return call.ThrownValue();
    }
    std::string text;
    if (!ToDomString(call, call.arguments[1], text)) {
      return call.ThrownValue();
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    dom::Node* self = NodeOf(call.self);
    dom::Node* node = NodeOf(owner->CreateText(
        text, self == nullptr ? owner->Document() : owner->NodeDocumentOf(*self)));
    (void)insert_adjacent(call, "insertAdjacentText", node);
    // Undefined whatever happened -- the node went in or the element had no
    // parent, and the caller is told neither. That asymmetry with
    // insertAdjacentElement is the specification's.
    return call.HasThrown() ? call.ThrownValue() : Value::Undefined();
  });
}

void DomBindings::InsertAdjacentParsedHtml(dom::Node& parent, dom::Node* reference,
                                           const std::string& markup) {
  // A parent that is not an element -- the document, a detached fragment -- has
  // no tag to be a context, and the specification substitutes `body`. Made
  // rather than found, because the document's own body is not necessarily the
  // right shape and is not necessarily there at all.
  //
  // **`<html>` takes the same substitution**, and that is the half that is easy
  // to miss: `document.body.insertAdjacentHTML("afterend", "<p>")` has `<html>`
  // as its context, and parsing into an `html` context runs "before head", which
  // invents a `<head>` and a `<body>` around the `<p>` and inserts *those* --
  // leaving the document with three of each. The same rule is why
  // `createContextualFragment` on `<html>` does not auto-create a body.
  std::string context = "body";
  if (parent.IsElement()) {
    const auto& element = static_cast<dom::Element&>(parent);
    if (!(element.Namespace().IsHtml() && element.LocalName() == "html")) {
      context = element.TagName();
    }
  }
  InsertParsedHtml(context, parent, reference, markup);
}

void DomBindings::ReplaceWithParsedHtml(dom::Node& parent, dom::Node& target,
                                        const std::string& markup) {
  // In before out, so the new nodes land where the old one was rather than at
  // the end -- the same order `replaceChild` uses and for the same reason.
  InsertAdjacentParsedHtml(parent, &target, markup);
  DetachFromTree(target);
}

}  // namespace microbrowser::bindings

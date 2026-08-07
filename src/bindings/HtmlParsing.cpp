#include <memory>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
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
                                         dom::Node* reference) {
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
  std::vector<dom::Node*> added;
  while (dom::Node* first = fragment.FirstChild()) {
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
  RecordMutation(parent, "childList", {}, Value::Null(), added, {});
}

void DomBindings::InsertParsedHtml(std::string_view context_tag_name, dom::Node& parent,
                                   dom::Node* reference, const std::string& markup) {
  // Quirks mode is carried in because it changes the tree: a `<table>` does not
  // close an open `<p>` in quirks mode, and a fragment that assumed standards
  // mode would build a different tree from the document it is going into.
  std::unique_ptr<dom::DocumentFragment> parsed = html::ParseFragment(
      markup, context_tag_name, document_ != nullptr && document_->InQuirksMode());
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
      getter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kOwnerSlot, PointerValue(this));
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
        // Read before anything is torn down: `ToString` can call a page's own
        // `toString`, and that runs script which may move this element.
        const std::string markup = js::ToString(Argument(call.arguments, 0));
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
        if (parent == nullptr) {
          // The specification's NoModificationAllowedError. A node with no
          // parent has no place to put a replacement, and silently doing
          // nothing is how a page ends up debugging the wrong line.
          return call.Throw("NoModificationAllowedError",
                            "outerHTML on a node with no parent");
        }
        // §"the fragment parsing algorithm" for outerHTML: the context is the
        // *parent*, because that is where the nodes are going. A parent that is
        // not an element -- the document, a fragment -- uses body's rules,
        // which is what the specification says and what keeps `<td>` from
        // becoming a cell at the top of a document.
        const std::string markup = js::ToString(Argument(call.arguments, 0));
        owner->ReplaceWithParsedHtml(*parent, *self, markup);
        return Value::Undefined();
      });

  const auto method = [this, &element_interface](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      element_interface.object->Set(name, native);
    }
  };

  method("insertAdjacentHTML", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "insertAdjacentHTML called on a non-element");
    }
    auto& element = static_cast<dom::Element&>(*self);
    const std::string position = LowerCase(js::ToString(Argument(call.arguments, 0)));
    const std::string markup = js::ToString(Argument(call.arguments, 1));

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
      return call.Throw("SyntaxError", "'" + position + "' is not a valid insert position");
    }
    dom::Node* parent = element.Parent();
    if (parent == nullptr) {
      return call.Throw("NoModificationAllowedError",
                        "insertAdjacentHTML '" + position + "' on a node with no parent");
    }
    // The context is the parent, for the reason outerHTML's is.
    dom::Node* reference =
        position == "beforebegin" ? &element : NextSibling(*parent, element);
    owner->InsertAdjacentParsedHtml(*parent, reference, markup);
    return Value::Undefined();
  });
}

void DomBindings::InsertAdjacentParsedHtml(dom::Node& parent, dom::Node* reference,
                                           const std::string& markup) {
  // A parent that is not an element -- the document, a detached fragment -- has
  // no tag to be a context, and the specification substitutes `body`. Made
  // rather than found, because the document's own body is not necessarily the
  // right shape and is not necessarily there at all.
  InsertParsedHtml(parent.IsElement() ? static_cast<dom::Element&>(parent).TagName() : "body",
                   parent, reference, markup);
}

void DomBindings::ReplaceWithParsedHtml(dom::Node& parent, dom::Node& target,
                                        const std::string& markup) {
  // In before out, so the new nodes land where the old one was rather than at
  // the end -- the same order `replaceChild` uses and for the same reason.
  InsertAdjacentParsedHtml(parent, &target, markup);
  DetachFromTree(target);
}

}  // namespace microbrowser::bindings

// `document.body` and `document.title`. The argument for the type is in
// DocumentTree.h; this is the two algorithms.

#include <memory>
#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/DocumentTree.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

constexpr std::string_view kSvgNamespace = "http://www.w3.org/2000/svg";

bool IsHtmlElementNamed(const dom::Node& node, std::string_view tag) {
  if (!node.IsElement()) {
    return false;
  }
  const auto& element = static_cast<const dom::Element&>(node);
  return element.Namespace().IsHtml() && element.TagName() == tag;
}

// "Child text content": the concatenation of the node's *child* text nodes, and
// deliberately not `textContent`, which is every descendant's.
// `<title>a<b>c</b></title>` has the child text content "a" -- a title is not
// supposed to contain elements, and a browser that read the whole subtree would
// name the tab after markup the author did not mean as the title.
std::string ChildTextContent(const dom::Node& node) {
  std::string out;
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    if (child->GetKind() == dom::Node::Kind::Text) {
      out += static_cast<const dom::Text&>(*child).Data();
    }
  }
  return out;
}

// "Strip and collapse ASCII whitespace": leading and trailing runs removed,
// every interior run replaced by one space. It is what makes
// `<title>\n  Hello\n  world\n</title>` the tab name "Hello world" rather than
// the five lines the author indented, and it is the whole of what
// `document.title` was missing.
std::string StripAndCollapseAsciiWhitespace(std::string_view text) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
  };
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const char c : text) {
    if (is_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(c);
  }
  return out;
}

// The document element when it is an SVG root. `document.title` treats such a
// document differently from every other: its title is an SVG `<title>` *child*
// of the root rather than the first `<title>` anywhere in the tree.
dom::Element* SvgRootOf(dom::Document& document) {
  dom::Element* root = document.DocumentElement();
  if (root == nullptr || root->TagName() != "svg" || root->Namespace().Uri() != kSvgNamespace) {
    return nullptr;
  }
  return root;
}

// "The title element": the first `<title>` in the document in tree order, in
// the HTML namespace. In an SVG document it is instead an SVG `<title>` that is
// a *child* of the root -- one nested deeper titles a shape, not the document.
dom::Element* TitleElementOf(dom::Document& document) {
  if (dom::Element* svg_root = SvgRootOf(document)) {
    for (const std::unique_ptr<dom::Node>& child : svg_root->Children()) {
      if (!child->IsElement()) {
        continue;
      }
      auto* element = static_cast<dom::Element*>(child.get());
      if (element->TagName() == "title" && element->Namespace().Uri() == kSvgNamespace) {
        return element;
      }
    }
    return nullptr;
  }
  dom::Element* found = nullptr;
  document.ForEachDescendant([&found](const dom::Node& node) {
    if (found == nullptr && IsHtmlElementNamed(node, "title")) {
      found = const_cast<dom::Element*>(static_cast<const dom::Element*>(&node));
    }
  });
  return found;
}

}  // namespace

void DocumentTree::Install(const js::Value& target) {
  if (!target.IsObject() || bindings_.interpreter_ == nullptr) {
    return;
  }
  DomBindings* owner = &bindings_;

  // -- document.body --------------------------------------------------------
  const Value body_get = bindings_.interpreter_->NewNativeValue("body", [owner](NativeCall& call) {
    dom::Document* document = owner->DocumentOf(call.self);
    return document == nullptr ? Value::Null() : owner->WrapperFor(document->Body());
  });
  const Value body_set =
      bindings_.interpreter_->NewNativeValue("body", [owner](NativeCall& call) -> Value {
        dom::Document* document = owner->DocumentOf(call.self);
        if (document == nullptr) {
          return Value::Undefined();
        }
        // Two different failures, and Web IDL decides which. The setter's type
        // is `HTMLElement?`, so a *string* fails the argument conversion and is
        // a TypeError; only a wrong kind of *element* reaches the algorithm and
        // its HierarchyRequestError. One answer for both would report a tree
        // problem for what is a type problem.
        dom::Node* node = NodeOf(Argument(call.arguments, 0));
        if (node == nullptr || !node->IsElement()) {
          return call.Throw("TypeError", "document.body must be an element");
        }
        if (!(IsHtmlElementNamed(*node, "body") || IsHtmlElementNamed(*node, "frameset"))) {
          return ThrowDom(call, "HierarchyRequestError",
                          "document.body must be a body or frameset element");
        }
        dom::Element* existing = document->Body();
        if (existing == node) {
          return Value::Undefined();
        }
        if (existing != nullptr) {
          // Replace, which is what makes `document.body = frameset` turn a page
          // into a frameset document rather than give it two bodies. In before
          // out, so the tree is never without one.
          dom::Node* parent = existing->Parent();
          if (parent == nullptr) {
            return Value::Undefined();
          }
          (void)owner->InsertNodeBefore(*parent, node, existing);
          (void)owner->DetachFromTree(*existing);
          return Value::Undefined();
        }
        dom::Element* root = document->DocumentElement();
        if (root == nullptr) {
          return ThrowDom(call, "HierarchyRequestError",
                          "document.body has nowhere to go: the document has no element");
        }
        (void)owner->InsertNodeBefore(*root, node, nullptr);
        return Value::Undefined();
      });
  if (body_get.IsObject() && body_set.IsObject()) {
    body_get.object->Set(kOwnerSlot, OwnerValue(owner));
    body_set.object->Set(kOwnerSlot, OwnerValue(owner));
    target.object->DefineAccessor("body", body_get.object, body_set.object);
  }

  // -- document.title -------------------------------------------------------
  const Value title_get = bindings_.interpreter_->NewNativeValue("title", [owner](NativeCall& call) {
    dom::Document* document = owner->DocumentOf(call.self);
    if (document == nullptr) {
      return Value::String(std::string());
    }
    dom::Element* title = TitleElementOf(*document);
    return Value::String(title == nullptr
                             ? std::string()
                             : StripAndCollapseAsciiWhitespace(ChildTextContent(*title)));
  });
  const Value title_set =
      bindings_.interpreter_->NewNativeValue("title", [owner](NativeCall& call) -> Value {
        dom::Document* document = owner->DocumentOf(call.self);
        if (document == nullptr) {
          return Value::Undefined();
        }
        std::string text;
        if (!CoerceToString(call, Argument(call.arguments, 0), text)) {
          return call.ThrownValue();
        }
        dom::Element* title = TitleElementOf(*document);
        if (title == nullptr) {
          if (dom::Element* svg_root = SvgRootOf(*document)) {
            // "Insert as the first child of the document element" -- first
            // rather than appended, because an SVG title is the accessible name
            // of the graphic and reading order is what makes it one.
            auto created = std::make_unique<dom::Element>(dom::NamespaceRef::kSvg,
                                                         std::string("title"), 0);
            const dom::Node* first =
                svg_root->Children().empty() ? nullptr : svg_root->Children().front().get();
            title = static_cast<dom::Element*>(&svg_root->InsertBefore(std::move(created), first));
          } else {
            // "If the title element is null and the head element is null, then
            // return" -- the specification declines to invent a head, and so
            // does this.
            dom::Element* head = document->Head();
            if (head == nullptr) {
              return Value::Undefined();
            }
            title = static_cast<dom::Element*>(
                &head->Append(std::make_unique<dom::Element>("title")));
          }
        }
        // "String replace all": an *empty* string replaces the children with
        // nothing rather than with an empty text node, which is what makes
        // `doc.title = ''` leave `title.childNodes.length` at zero.
        owner->ClearChildren(*title);
        if (!text.empty()) {
          (void)owner->AppendTextTo(*title, text);
        }
        return Value::Undefined();
      });
  if (title_get.IsObject() && title_set.IsObject()) {
    title_get.object->Set(kOwnerSlot, OwnerValue(owner));
    title_set.object->Set(kOwnerSlot, OwnerValue(owner));
    target.object->DefineAccessor("title", title_get.object, title_set.object);
  }
}

}  // namespace microbrowser::bindings

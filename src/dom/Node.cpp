#include "dom/Node.h"

#include <algorithm>
#include <array>

#include "util/PerformanceCounters.h"

namespace microbrowser::dom {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

constexpr std::array<std::string_view, 14> kVoidElements = {
    "area", "base", "br",    "col",   "embed",  "hr",    "img",
    "input", "link", "meta", "param", "source", "track", "wbr"};

// Escapes text data. Not a sanitizer — it exists so the serializer round-trips
// for tests, and nothing security-relevant depends on it.
void AppendEscaped(std::string_view text, bool in_attribute, std::string& out) {
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        if (in_attribute) {
          out.push_back(c);
        } else {
          out += "&lt;";
        }
        break;
      case '>':
        if (in_attribute) {
          out.push_back(c);
        } else {
          out += "&gt;";
        }
        break;
      case '"':
        if (in_attribute) {
          out += "&quot;";
        } else {
          out.push_back(c);
        }
        break;
      default:
        out.push_back(c);
    }
  }
}

}  // namespace

bool IsVoidElement(std::string_view tag_name) {
  return std::find(kVoidElements.begin(), kVoidElements.end(), tag_name) != kVoidElements.end();
}

void Node::SetNodeDocument(Document* document) {
  if (node_document_ == document) {
    // The common case, and the reason adoption is not a cost on every
    // insertion: a subtree built by `document.createElement` and appended into
    // the same document stops here rather than being walked.
    return;
  }
  node_document_ = document;
  for (const std::unique_ptr<Node>& child : children_) {
    child->SetNodeDocument(document);
  }
  // A shadow root and a template's contents are *not* children -- deliberately,
  // so that nothing which walks the document reaches them -- and a node inside
  // either still belongs to the host's document. A cast here rather than a
  // virtual, because one more vtable entry on every node in the tree buys one
  // branch on the two elements in a page that have either.
  if (kind_ != Kind::Element) {
    return;
  }
  const Element& element = static_cast<const Element&>(*this);
  if (DocumentFragment* content = element.Content()) {
    content->SetNodeDocument(document);
  }
  if (DocumentFragment* shadow = element.ShadowRoot()) {
    shadow->SetNodeDocument(document);
  }
}

Document* Node::ConnectedDocument() const {
  const Node* node = this;
  while (true) {
    if (node->parent_ != nullptr) {
      node = node->parent_;
      continue;
    }
    // A shadow root has no parent -- deliberately, ADR 0019 §2 -- but a node
    // inside one still belongs to the document its host is in, and mutating one
    // still invalidates what the document derived from the tree. Crossing here is
    // what makes `root.innerHTML = …` bump the mutation version and therefore
    // relayout; without it a component could rewrite itself and the screen would
    // never change.
    if (node->kind_ == Kind::DocumentFragment) {
      if (const Element* host = static_cast<const DocumentFragment*>(node)->Host()) {
        node = host;
        continue;
      }
    }
    break;
  }
  // A node script built and has not inserted yet has no document, and nothing
  // derived from the tree describes it. That is the common case during a parse,
  // where attributes are set before the element is inserted, and it is why this
  // is cheap there.
  if (node->kind_ != Kind::Document) {
    return nullptr;
  }
  return const_cast<Document*>(static_cast<const Document*>(node));
}

void Node::NoteMutation() {
  if (Document* document = ConnectedDocument(); document != nullptr) {
    document->NoteTreeMutation();
  }
}

void Node::NoteStructureChange() {
  if (Document* document = ConnectedDocument(); document != nullptr) {
    document->NoteStructureMutation();
  }
}

void Node::ReleaseFocusWithin(const Node& removed) {
  Document* document = ConnectedDocument();
  if (document == nullptr) {
    return;
  }
  const Element* focused = document->Focus().element;
  if (focused == nullptr) {
    return;
  }
  for (const Node* at = focused; at != nullptr; at = at->parent_) {
    if (at == &removed) {
      document->SetFocus(nullptr, false);
      return;
    }
  }
}

Node& Node::Append(std::unique_ptr<Node> child) {
  child->parent_ = this;
  // The DOM's "adopt": a node inserted into a tree takes that tree's node
  // document, and so does everything under it. A no-op when they already agree,
  // which is every insertion the parser makes after the first.
  child->SetNodeDocument(node_document_);
  children_.push_back(std::move(child));
  AddPerformanceCounter(PerfCounterId::DomNodesCreated);
  NoteStructureChange();
  return *children_.back();
}

Node& Node::InsertBefore(std::unique_ptr<Node> child, const Node* reference) {
  // A null reference means "append", and asking `find_if` about it means
  // scanning every existing child to be told so. That is the whole cost of
  // parsing a document: the tree builder inserts *every* element through here
  // with a null reference, so building n siblings was O(n^2) -- 60,000 of them
  // took 15 seconds, and 15,000 took one. Both are now under 30ms.
  //
  // A parse whose cost is quadratic in a number an attacker chooses is a
  // denial of service with a 400KB payload, which is why this is a guard
  // rather than a tuning.
  if (reference == nullptr) {
    return Append(std::move(child));
  }
  const auto found = std::find_if(
      children_.begin(), children_.end(),
      [reference](const std::unique_ptr<Node>& candidate) { return candidate.get() == reference; });
  if (found == children_.end()) {
    return Append(std::move(child));
  }
  child->parent_ = this;
  child->SetNodeDocument(node_document_);
  AddPerformanceCounter(PerfCounterId::DomNodesCreated);
  Node& inserted = **children_.insert(found, std::move(child));
  NoteStructureChange();
  return inserted;
}

bool Node::Remove(Node* child) {
  const auto found = std::find_if(
      children_.begin(), children_.end(),
      [child](const std::unique_ptr<Node>& candidate) { return candidate.get() == child; });
  if (found == children_.end()) {
    return false;
  }
  ReleaseFocusWithin(**found);
  children_.erase(found);
  NoteStructureChange();
  return true;
}

std::unique_ptr<Node> Node::Detach(Node* child) {
  const auto found = std::find_if(
      children_.begin(), children_.end(),
      [child](const std::unique_ptr<Node>& candidate) { return candidate.get() == child; });
  if (found == children_.end()) {
    return nullptr;
  }
  ReleaseFocusWithin(**found);
  std::unique_ptr<Node> owned = std::move(*found);
  children_.erase(found);
  // The parent link goes with the ownership. A node that still claimed a
  // parent it is no longer a child of is the shape every "it disappeared but
  // is still in the list" bug takes.
  owned->parent_ = nullptr;
  NoteStructureChange();
  return owned;
}

std::string Node::TextContent() const {
  std::string out;
  // *This* node when it is text, and not only its descendants. A Text node has no
  // children, so the descendant walk answered "" for one -- which is wrong twice
  // over: the DOM says `textContent` on a Text node is its data, and a caller
  // asking a node it did not have to type-check got a silent empty string.
  if (IsText()) {
    return static_cast<const Text&>(*this).Data();
  }
  ForEachDescendant([&out](const Node& node) {
    if (node.IsText()) {
      out += static_cast<const Text&>(node).Data();
    }
  });
  return out;
}

Element* Node::ClosestAncestor(std::string_view tag_name) const {
  for (Node* at = parent_; at != nullptr; at = at->parent_) {
    if (at->IsElement() && static_cast<Element*>(at)->TagName() == tag_name) {
      return static_cast<Element*>(at);
    }
  }
  return nullptr;
}

std::string Node::SerializeChildren() const {
  std::string out;
  for (const std::unique_ptr<Node>& child : children_) {
    out += child->Serialize();
  }
  return out;
}

std::string Node::Serialize() const {
  return SerializeChildren();
}

const std::string* Element::GetAttribute(std::string_view name) const {
  for (const Attribute& attribute : attributes_) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

void Element::SetAttribute(std::string name, std::string value) {
  for (Attribute& attribute : attributes_) {
    if (attribute.name == name) {
      attribute.value = std::move(value);
      ++attr_version_;
      NoteMutation();
      return;
    }
  }
  // No namespace and no prefix: that is what "set an attribute" by qualified
  // name creates, and what every attribute the HTML parser produces is.
  attributes_.push_back(Attribute{std::move(name), std::move(value), NamespaceRef(), 0});
  ++attr_version_;
  NoteMutation();
}

const Attribute* Element::GetAttributeNS(const NamespaceRef& name_space,
                                         std::string_view local_name) const {
  for (const Attribute& attribute : attributes_) {
    if (attribute.name_space == name_space && attribute.LocalName() == local_name) {
      return &attribute;
    }
  }
  return nullptr;
}

void Element::SetAttributeNS(NamespaceRef name_space, std::string qualified_name,
                             std::uint32_t prefix_length, std::string value) {
  if (prefix_length >= qualified_name.size()) {
    prefix_length = 0;
  }
  const std::string_view local =
      std::string_view(qualified_name)
          .substr(prefix_length == 0 ? 0 : prefix_length + 1);
  for (Attribute& attribute : attributes_) {
    if (attribute.name_space == name_space && attribute.LocalName() == local) {
      // The existing attribute keeps its place in the list and takes the new
      // prefix: "set an attribute value" changes the qualified name of the one
      // already there rather than appending a second attribute that the `…NS`
      // getters could never tell apart.
      attribute.name = std::move(qualified_name);
      attribute.prefix_length = prefix_length;
      attribute.value = std::move(value);
      ++attr_version_;
      NoteMutation();
      return;
    }
  }
  attributes_.push_back(Attribute{std::move(qualified_name), std::move(value),
                                  std::move(name_space), prefix_length});
  ++attr_version_;
  NoteMutation();
}

bool Element::RemoveAttributeNS(const NamespaceRef& name_space,
                                std::string_view local_name) {
  const auto found =
      std::find_if(attributes_.begin(), attributes_.end(),
                   [&name_space, local_name](const Attribute& attribute) {
                     return attribute.name_space == name_space &&
                            attribute.LocalName() == local_name;
                   });
  if (found == attributes_.end()) {
    return false;
  }
  attributes_.erase(found);
  ++attr_version_;
  NoteMutation();
  return true;
}

bool Element::RemoveAttribute(std::string_view name) {
  const auto found = std::find_if(attributes_.begin(), attributes_.end(),
                                  [name](const Attribute& attribute) {
                                    return attribute.name == name;
                                  });
  if (found == attributes_.end()) {
    return false;
  }
  attributes_.erase(found);
  ++attr_version_;
  NoteMutation();
  return true;
}

bool Element::SetState(ElementState state, bool on) {
  // Only the stored states are storable. A caller asking to write `Focus` has
  // the wrong model of where focus lives, and half-recording it here is how the
  // second copy gets created -- so it is dropped, loudly enough to be found by
  // the assertion in the test that names each state and its writer.
  const auto storable = static_cast<std::uint16_t>(state & kStoredElementStates);
  if (storable == 0) {
    return false;
  }
  const auto before = static_cast<std::uint16_t>(state_);
  const auto after =
      static_cast<std::uint16_t>(on ? (before | storable) : (before & ~storable));
  if (after == before) {
    return false;
  }
  state_ = static_cast<ElementState>(after);
  // Deliberately *not* a tree mutation. Nothing about the document changed: the
  // box tree still describes it, and bumping the version here would make every
  // mouse move look like a DOM edit to layout's clean flag -- which is the
  // opposite of what ADR 0016 is for.
  return true;
}

Element::Element(std::string tag_name)
    : Element(NamespaceRef::kHtml, std::move(tag_name), 0) {}

Element::Element(NamespaceRef name_space, std::string qualified_name,
                 std::uint32_t prefix_length)
    : Node(Kind::Element),
      tag_name_(std::move(qualified_name)),
      prefix_length_(prefix_length < tag_name_.size() ? prefix_length : 0),
      namespace_(std::move(name_space)) {
  // Every `<template>` has its contents fragment from the moment it exists,
  // including one script made with `createElement`: a template whose `content`
  // appeared only when the parser filled it would be a different object
  // depending on where the element came from.
  //
  // In the HTML namespace only: `createElementNS('http://FOO', 'template')` is
  // not a template, and giving it inert contents would hide its children from
  // the tree that a page put them in.
  if (namespace_.IsHtml() && tag_name_ == "template") {
    content_ = std::make_unique<DocumentFragment>();
    content_->SetTemplateContent(true);
  }
}

Element::~Element() = default;

DocumentFragment* Element::AttachShadow(bool open) {
  if (shadow_ == nullptr) {
    shadow_ = std::make_unique<DocumentFragment>();
    shadow_->SetHost(this);
    // A shadow root's node document is its host's, and the host is usually
    // already in a tree when `attachShadow` is called -- so this cannot wait
    // for an insertion that has already happened.
    shadow_->SetNodeDocument(NodeDocument());
    shadow_open_ = open;
    NoteStructureChange();
  }
  // The existing one, not a replacement. A second `attachShadow` is an error the
  // caller reports, and handing back the first is what makes that reportable
  // rather than a silent replacement of a subtree the page holds references into.
  return shadow_.get();
}

std::string Element::Serialize() const {
  std::string out;
  out.push_back('<');
  out += tag_name_;
  for (const Attribute& attribute : attributes_) {
    out.push_back(' ');
    out += attribute.name;
    out += "=\"";
    AppendEscaped(attribute.value, true, out);
    out.push_back('"');
  }
  out.push_back('>');
  if (IsVoidElement(tag_name_)) {
    return out;
  }
  // A template serializes its *contents*, which is where its markup went. The
  // spec says the same thing, and it is what makes a round trip through
  // `innerHTML` preserve a template rather than empty it.
  out += content_ != nullptr ? content_->SerializeChildren() : SerializeChildren();
  out += "</";
  out += tag_name_;
  out.push_back('>');
  return out;
}

std::string Text::Serialize() const {
  std::string out;
  AppendEscaped(data_, false, out);
  return out;
}

std::string Comment::Serialize() const {
  return "<!--" + data_ + "-->";
}

std::string DocumentFragment::Serialize() const {
  // Its children and no markup of its own, which is what a fragment is: it
  // has no tag, so serializing one is serializing what it holds.
  std::string out;
  for (const std::unique_ptr<Node>& child : Children()) {
    out += child->Serialize();
  }
  return out;
}

std::string DocumentType::Serialize() const {
  return "<!DOCTYPE " + name_ + ">";
}

std::string ProcessingInstruction::Serialize() const {
  // The HTML serializer's form (HTML §13.3): no `?` before the closing `>`,
  // which is the one place it differs from XML's.
  return "<?" + target_ + " " + data_ + ">";
}

Element* Document::DocumentElement() const {
  for (const std::unique_ptr<Node>& child : Children()) {
    if (child->IsElement()) {
      return static_cast<Element*>(child.get());
    }
  }
  return nullptr;
}

Element* Document::FirstElementByTagName(std::string_view tag_name) const {
  Element* found = nullptr;
  ForEachDescendant([&](const Node& node) {
    if (found == nullptr && node.IsElement() &&
        static_cast<const Element&>(node).TagName() == tag_name) {
      found = const_cast<Element*>(static_cast<const Element*>(&node));
    }
  });
  return found;
}

std::vector<Element*> Document::ElementsByTagName(std::string_view tag_name) const {
  std::vector<Element*> found;
  ForEachDescendant([&](const Node& node) {
    if (node.IsElement() && static_cast<const Element&>(node).TagName() == tag_name) {
      found.push_back(const_cast<Element*>(static_cast<const Element*>(&node)));
    }
  });
  return found;
}

// "The body element of a document is the first of the html element's children
// that is either a body element or a frameset element, or null if there is no
// such element."
//
// Every clause of that does work, and a descendant search for the first `<body>`
// -- which is what this was -- gets all of them wrong:
//
//   * **A child of the html element**, not a descendant of the document. A
//     `<body>` inside some other element is not the body element, and a `<body>`
//     that *is* the document element has no html element above it, so the
//     document has no body at all.
//   * **A frameset counts.** A frameset document's body element is its
//     `<frameset>`, which is why `document.body` is where `onload` is set on
//     either kind of page.
//   * **In the HTML namespace.** `createElementNS('urn:x', 'body')` appended to
//     `<html>` is not a body, and answering with it would hand a page an element
//     with none of the members it is about to use.
Element* Document::Body() const {
  const Element* html = DocumentElement();
  if (html == nullptr || !html->Namespace().IsHtml() || html->TagName() != "html") {
    return nullptr;
  }
  for (const std::unique_ptr<Node>& child : html->Children()) {
    if (!child->IsElement()) {
      continue;
    }
    auto* element = static_cast<Element*>(child.get());
    if (element->Namespace().IsHtml() &&
        (element->TagName() == "body" || element->TagName() == "frameset")) {
      return element;
    }
  }
  return nullptr;
}

// "The head element of a document is the first head element that is a child of
// the html element." Same three clauses as Body above, and the same reason a
// descendant search is wrong: a `<head>` somewhere else is not the head, and
// `document.title = x` appending into it would put the title where no parser
// would have placed one.
Element* Document::Head() const {
  const Element* html = DocumentElement();
  if (html == nullptr || !html->Namespace().IsHtml() || html->TagName() != "html") {
    return nullptr;
  }
  for (const std::unique_ptr<Node>& child : html->Children()) {
    if (child->IsElement()) {
      auto* element = static_cast<Element*>(child.get());
      if (element->Namespace().IsHtml() && element->TagName() == "head") {
        return element;
      }
    }
  }
  return nullptr;
}

}  // namespace microbrowser::dom

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

Document* Node::OwnerDocument() const {
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
  if (Document* document = OwnerDocument(); document != nullptr) {
    document->NoteTreeMutation();
  }
}

void Node::ReleaseFocusWithin(const Node& removed) {
  Document* document = OwnerDocument();
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
  children_.push_back(std::move(child));
  AddPerformanceCounter(PerfCounterId::DomNodesCreated);
  NoteMutation();
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
  AddPerformanceCounter(PerfCounterId::DomNodesCreated);
  Node& inserted = **children_.insert(found, std::move(child));
  NoteMutation();
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
  NoteMutation();
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
  NoteMutation();
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
      NoteMutation();
      return;
    }
  }
  attributes_.push_back(Attribute{std::move(name), std::move(value)});
  NoteMutation();
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
    : Node(Kind::Element), tag_name_(std::move(tag_name)) {
  // Every `<template>` has its contents fragment from the moment it exists,
  // including one script made with `createElement`: a template whose `content`
  // appeared only when the parser filled it would be a different object
  // depending on where the element came from.
  if (tag_name_ == "template") {
    content_ = std::make_unique<DocumentFragment>();
    content_->SetTemplateContent(true);
  }
}

Element::~Element() = default;

DocumentFragment* Element::AttachShadow(bool open) {
  if (shadow_ == nullptr) {
    shadow_ = std::make_unique<DocumentFragment>();
    shadow_->SetHost(this);
    shadow_open_ = open;
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

Element* Document::Body() const {
  return FirstElementByTagName("body");
}

Element* Document::Head() const {
  return FirstElementByTagName("head");
}

}  // namespace microbrowser::dom

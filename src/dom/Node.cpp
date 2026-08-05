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

void Node::NoteMutation() {
  Node* node = this;
  while (node->parent_ != nullptr) {
    node = node->parent_;
  }
  // A node script built and has not inserted yet has no document, and nothing
  // derived from the tree describes it. That is the common case during a parse,
  // where attributes are set before the element is inserted, and it is why this
  // is cheap there.
  if (node->kind_ == Kind::Document) {
    static_cast<Document*>(node)->NoteTreeMutation();
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
  out += SerializeChildren();
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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::dom {

class Document;
class Element;

// A DOM node.
//
// A class hierarchy with a vtable, which the repo reserves for durable
// polymorphic boundaries — and this is one. Every engine that ships (Blink,
// Gecko, WebKit, Ladybird) models the DOM this way, because the tree is walked
// polymorphically by layout, paint, style, and script bindings, and a tagged
// union would push a switch into every one of them.
//
// Ownership is a strict tree: a parent owns its children, and a child holds a
// non-owning pointer up. That is what makes the lifetime question answerable —
// the alternative, reference counting in both directions, is how a DOM grows
// cycles that leak whole documents.
class Node {
 public:
  enum class Kind : std::uint8_t {
    Document,
    DocumentType,
    Element,
    Text,
    Comment,
  };

  explicit Node(Kind kind) : kind_(kind) {}
  virtual ~Node() = default;

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  Kind GetKind() const { return kind_; }
  bool IsElement() const { return kind_ == Kind::Element; }
  bool IsText() const { return kind_ == Kind::Text; }

  Node* Parent() const { return parent_; }
  const std::vector<std::unique_ptr<Node>>& Children() const { return children_; }
  Node* FirstChild() const { return children_.empty() ? nullptr : children_.front().get(); }
  Node* LastChild() const { return children_.empty() ? nullptr : children_.back().get(); }

  Node& Append(std::unique_ptr<Node> child);

  // Detaches and destroys `child`. Returns false if it is not a child of this
  // node, which is a caller bug rather than a routine outcome.
  bool Remove(Node* child);

  // Concatenated text of this subtree, which is `textContent`.
  std::string TextContent() const;

  // Depth-first walk. A callback rather than an iterator because the callers so
  // far are all "visit everything" and an iterator over a tree of unique_ptr is
  // a lot of machinery for that.
  template <typename Visitor>
  void ForEachDescendant(Visitor&& visit) const {
    for (const std::unique_ptr<Node>& child : children_) {
      visit(*child);
      child->ForEachDescendant(visit);
    }
  }

  // Nearest ancestor element with this tag name, or null.
  Element* ClosestAncestor(std::string_view tag_name) const;

  // Serializes the subtree back to HTML. Used by tests to state an expected
  // tree in one string, and by nothing else — it is not a sanitizer, and a
  // round trip through it is not a security boundary.
  std::string SerializeChildren() const;
  virtual std::string Serialize() const;

 private:
  Kind kind_;
  Node* parent_ = nullptr;
  std::vector<std::unique_ptr<Node>> children_;
};

struct Attribute {
  std::string name;
  std::string value;

  friend bool operator==(const Attribute&, const Attribute&) = default;
};

class Element : public Node {
 public:
  explicit Element(std::string tag_name)
      : Node(Kind::Element), tag_name_(std::move(tag_name)) {}

  const std::string& TagName() const { return tag_name_; }
  const std::vector<Attribute>& Attributes() const { return attributes_; }

  const std::string* GetAttribute(std::string_view name) const;
  bool HasAttribute(std::string_view name) const { return GetAttribute(name) != nullptr; }
  void SetAttribute(std::string name, std::string value);

  std::string Serialize() const override;

 private:
  std::string tag_name_;
  std::vector<Attribute> attributes_;
};

class Text : public Node {
 public:
  explicit Text(std::string data) : Node(Kind::Text), data_(std::move(data)) {}

  const std::string& Data() const { return data_; }
  void Append(std::string_view more) { data_ += more; }

  std::string Serialize() const override;

 private:
  std::string data_;
};

class Comment : public Node {
 public:
  explicit Comment(std::string data) : Node(Kind::Comment), data_(std::move(data)) {}
  const std::string& Data() const { return data_; }
  std::string Serialize() const override;

 private:
  std::string data_;
};

class DocumentType : public Node {
 public:
  explicit DocumentType(std::string name)
      : Node(Kind::DocumentType), name_(std::move(name)) {}
  const std::string& Name() const { return name_; }
  std::string Serialize() const override;

 private:
  std::string name_;
};

class Document : public Node {
 public:
  Document() : Node(Kind::Document) {}

  // Quirks mode is a rendering decision that comes from the doctype, and it has
  // to be carried on the document because layout asks about it long after the
  // doctype token is gone.
  bool InQuirksMode() const { return quirks_; }
  void SetQuirksMode(bool quirks) { quirks_ = quirks; }

  Element* DocumentElement() const;
  Element* Body() const;
  Element* Head() const;

  // First element with this tag name, in tree order.
  Element* FirstElementByTagName(std::string_view tag_name) const;
  std::vector<Element*> ElementsByTagName(std::string_view tag_name) const;

 private:
  bool quirks_ = false;
};

// Whether an element cannot have children — `br`, `img`, `meta` and the rest.
// A tree builder that let one have children would nest the whole rest of the
// document inside an `<img>`.
bool IsVoidElement(std::string_view tag_name);

}  // namespace microbrowser::dom

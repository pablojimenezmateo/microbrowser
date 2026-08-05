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
    // A parentless bag of nodes. Script builds a subtree in one of these and
    // inserts it in a single operation, which is the point: inserting the
    // fragment inserts its *children* and leaves the fragment empty, so a
    // framework that assembles a hundred rows costs one insertion rather than
    // a hundred. Nothing the HTML parser produces is one -- this exists for
    // `document.createDocumentFragment`.
    DocumentFragment,
  };

  explicit Node(Kind kind) : kind_(kind) {}
  virtual ~Node() = default;

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  Kind GetKind() const { return kind_; }
  bool IsElement() const { return kind_ == Kind::Element; }
  bool IsText() const { return kind_ == Kind::Text; }
  bool IsDocumentFragment() const { return kind_ == Kind::DocumentFragment; }

  Node* Parent() const { return parent_; }
  const std::vector<std::unique_ptr<Node>>& Children() const { return children_; }
  Node* FirstChild() const { return children_.empty() ? nullptr : children_.front().get(); }
  Node* LastChild() const { return children_.empty() ? nullptr : children_.back().get(); }

  Node& Append(std::unique_ptr<Node> child);

  // Inserts before `reference`, appending when it is null or not a child here.
  // The HTML parser's foster parenting needs it: content that appears inside a
  // table but does not belong in one is inserted *before* the table, so a null
  // or stale reference must degrade to an append rather than fail.
  Node& InsertBefore(std::unique_ptr<Node> child, const Node* reference);

  // Detaches and destroys `child`. Returns false if it is not a child of this
  // node, which is a caller bug rather than a routine outcome.
  //
  // **Nothing calls this, and the first caller has an obligation.** The DOM
  // binding layer hands script a JavaScript object holding a raw `Node*`, and
  // that is safe only because no node is freed before its document. Whoever
  // gives this a caller owns fixing that in the same commit --
  // docs/adr/0008-dom-bindings.md says how. `Detach` below is the version that
  // does not have the problem, because it hands the node over rather than
  // destroying it.
  bool Remove(Node* child);

  // Detaches `child` and returns ownership of it. Null when it is not a child
  // here.
  //
  // The difference from `Remove` is the whole point: a detached node is still
  // alive, so a wrapper script is holding does not dangle. Whoever takes it
  // owns it, and the DOM binding layer keeps removed nodes for the life of the
  // document for exactly that reason.
  std::unique_ptr<Node> Detach(Node* child);

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

 protected:
  // Records that this node changed, on the document that owns it.
  //
  // Here rather than at the call sites above it because *missing one* is the
  // failure mode: anything derived from the tree -- the box tree first, the
  // style invalidation index next -- goes stale silently, and script reads a
  // rectangle that describes the page as it was. Every mutation in this module
  // is one of five primitives, and every mutation anywhere else goes through
  // one of them, so marking them is the only marking that cannot be forgotten.
  //
  // Finding the document is a walk to the root rather than a stored pointer.
  // A pointer would be faster and is what a mature DOM keeps; it also has to be
  // maintained across every insertion and removal of every *subtree*, which is
  // a second invariant to get wrong for a walk whose depth is bounded by
  // ADR 0009's parse depth. Revisit with a measurement, not a guess.
  void NoteMutation();

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
  bool RemoveAttribute(std::string_view name);

  std::string Serialize() const override;

 private:
  std::string tag_name_;
  std::vector<Attribute> attributes_;
};

class Text : public Node {
 public:
  explicit Text(std::string data) : Node(Kind::Text), data_(std::move(data)) {}

  const std::string& Data() const { return data_; }
  void Append(std::string_view more) {
    data_ += more;
    NoteMutation();
  }

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

// A subtree with no parent, inserted as a unit.
//
// Its own class rather than a bare Node so that a wrapper can name it and a
// page can write `instanceof DocumentFragment`. It carries nothing of its own:
// what makes it a fragment is what *insertion* does with it, which is in
// Node::Append.
class DocumentFragment : public Node {
 public:
  DocumentFragment() : Node(Kind::DocumentFragment) {}
  std::string Serialize() const override;
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

  // How many times anything in this tree has changed.
  //
  // A version rather than a dirty bit, because the readers are not one: layout
  // caches a box tree against it, and the style invalidation index of ADR 0016
  // will cache against it too. A bit that each of them cleared would mean the
  // first reader to look hid the change from the second.
  //
  // It answers exactly one question -- "is what I derived from this tree still
  // describing it?" -- and it deliberately says nothing about *what* changed.
  // Anything finer belongs to the invalidation index rather than here.
  std::uint64_t MutationVersion() const { return mutation_version_; }
  // Named apart from Node::NoteMutation, which is the walk that reaches this.
  // Two members of one hierarchy with the same name and different jobs is how
  // a call ends up meaning the other one.
  void NoteTreeMutation() { ++mutation_version_; }

  // First element with this tag name, in tree order.
  Element* FirstElementByTagName(std::string_view tag_name) const;
  std::vector<Element*> ElementsByTagName(std::string_view tag_name) const;

 private:
  bool quirks_ = false;
  std::uint64_t mutation_version_ = 0;
};

// Whether an element cannot have children — `br`, `img`, `meta` and the rest.
// A tree builder that let one have children would nest the whole rest of the
// document inside an `<img>`.
bool IsVoidElement(std::string_view tag_name);

}  // namespace microbrowser::dom

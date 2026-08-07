#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::dom {

class Document;
class DocumentFragment;
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

  // The document at the root of this tree, or null when there is none -- which
  // is a node script built and has not inserted, and is the common case during
  // a parse. A walk rather than a stored pointer, for the reason NoteMutation
  // says: a pointer is a second invariant to maintain across every subtree
  // move, for a depth ADR 0009 already bounds.
  Document* OwnerDocument() const;

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

  // Clears the document's focus if it is on `removed` or inside it.
  //
  // Called before a subtree leaves the tree, and here rather than at the
  // callers for the same reason NoteMutation is: *missing one* is the failure
  // mode. The document's focus is a raw `Element*`, and script removing the
  // element it is on would otherwise leave the next key routed at a node the
  // tree no longer contains -- which is a use-after-free the moment the binding
  // layer stops holding removed nodes alive.
  //
  // It walks *up* from the focused element rather than down over the subtree,
  // so removing a thousand nodes costs the depth of one rather than a thousand.
  void ReleaseFocusWithin(const Node& removed);

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

// The dynamic state a selector can match on: the states whose truth changes
// without the tree changing.
//
// One enum naming all of them, and deliberately not one enum per storage
// location, because two vocabularies for one set is how a rule ends up keyed
// under a name nothing ever sets. Where each answer *lives* is the second
// question and it has three answers:
//
//   * `Hover`, `Active` and `Target` are stored here, as bits on the element,
//     set by the engine and read by the matcher. That is ADR 0016 §2's
//     decision, and it is what keeps `src/css` a pure function of (element,
//     selector) that does not know a mouse exists.
//   * `Checked`, `Disabled`, `Required` and `PlaceholderShown` are stored here
//     too, and they are refreshed from the document by the engine before every
//     cascade. They are HTML semantics -- "actually disabled" includes an
//     ancestor `fieldset`, and `src/html` is the one module that defines that
//     -- so a bit is what carries the answer to a module that may not see
//     `src/html` at all.
//   * `Focus`, `FocusVisible` and `FocusWithin` are **not stored**. Focus is
//     one element on one document (ADR 0017 §4) and a bit per element would be
//     the second copy that disagrees -- which is the bug the focus model was
//     built to remove. They are derived at match time from Document::Focus(),
//     and they are named here anyway because the invalidation index has to file
//     a `:focus-within` rule under something.
enum class ElementState : std::uint16_t {
  None = 0,
  Hover = 1u << 0,
  Active = 1u << 1,
  Target = 1u << 2,
  Checked = 1u << 3,
  Disabled = 1u << 4,
  Required = 1u << 5,
  PlaceholderShown = 1u << 6,
  Focus = 1u << 7,
  FocusVisible = 1u << 8,
  FocusWithin = 1u << 9,
};

constexpr ElementState operator|(ElementState a, ElementState b) {
  return static_cast<ElementState>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
constexpr ElementState operator&(ElementState a, ElementState b) {
  return static_cast<ElementState>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
constexpr ElementState& operator|=(ElementState& a, ElementState b) { return a = a | b; }
constexpr bool Any(ElementState state) { return state != ElementState::None; }

// The states an element stores. The rest are derived, and writing one would be
// creating the second copy the enum's comment refuses.
inline constexpr ElementState kStoredElementStates =
    ElementState::Hover | ElementState::Active | ElementState::Target | ElementState::Checked |
    ElementState::Disabled | ElementState::Required | ElementState::PlaceholderShown;

class Element : public Node {
 public:
  explicit Element(std::string tag_name);
  ~Element() override;

  const std::string& TagName() const { return tag_name_; }

  // The template contents of a `<template>`, and null on every other element.
  //
  // A template's children are deliberately *not* its children. The parser puts
  // everything inside one into this separate fragment, so that nothing which
  // walks the tree -- the cascade, layout, the script collector,
  // `querySelectorAll`, the image loader -- can reach it. That is the whole
  // point of the element: a `<template>` whose contents were ordinary children
  // would render its own markup and fetch its own images, which is exactly what
  // this parser did before it had one.
  //
  // Owned by the element rather than by a parent, because a fragment has no
  // parent -- and so a node inside template contents has no owner document,
  // which is why mutating one costs the live document nothing.
  DocumentFragment* Content() const { return content_.get(); }

  // --- shadow DOM, ADR 0019 -------------------------------------------------

  // This element's shadow root, or null. A `DocumentFragment` for the reason
  // template contents are one: it has children and no parent.
  //
  // Held whether the mode is open or closed -- `mode: "closed"` is not a
  // security boundary and ADR 0019 refuses to pretend otherwise, since any
  // script that could call `attachShadow` could have kept the reference. What
  // closed changes is one thing: `element.shadowRoot` answers null. The bit is
  // here rather than in the binding layer because the *tree* is what has a
  // shadow root, and layout and the cascade have to walk it either way.
  DocumentFragment* ShadowRoot() const { return shadow_.get(); }
  bool ShadowIsOpen() const { return shadow_open_; }
  // Attaches one, or returns the existing one -- a second `attachShadow` on the
  // same element is an error the caller reports, and returning the first is what
  // makes that reportable rather than a silent replacement of a subtree a page is
  // holding references into.
  DocumentFragment* AttachShadow(bool open);

  const std::vector<Attribute>& Attributes() const { return attributes_; }

  const std::string* GetAttribute(std::string_view name) const;
  bool HasAttribute(std::string_view name) const { return GetAttribute(name) != nullptr; }
  void SetAttribute(std::string name, std::string value);
  bool RemoveAttribute(std::string_view name);

  // The dynamic state on this element. See ElementState: a bit here is a fact
  // the tree does not otherwise record, and the three focus states are
  // deliberately absent from it.
  bool HasState(ElementState state) const { return Any(state_ & state); }
  // True when the stored set changed, which is what tells a caller whether a
  // restyle is owed at all. Setting a state that is not stored is a caller bug
  // and is ignored rather than half-recorded.
  bool SetState(ElementState state, bool on);
  ElementState State() const { return state_; }

  std::string Serialize() const override;

 private:
  std::string tag_name_;
  std::vector<Attribute> attributes_;
  ElementState state_ = ElementState::None;
  // Allocated only for `<template>`. A pointer on every element rather than a
  // subclass, because the parser and the bindings both create elements by tag
  // name and neither has anywhere to put a second type.
  std::unique_ptr<DocumentFragment> content_;
  // Allocated only for a host, for the reason `content_` is: the parser and the
  // bindings both make elements by tag name and neither has anywhere to put a
  // second type.
  std::unique_ptr<DocumentFragment> shadow_;
  bool shadow_open_ = true;
};

class Text : public Node {
 public:
  explicit Text(std::string data) : Node(Kind::Text), data_(std::move(data)) {}

  const std::string& Data() const { return data_; }
  void SetData(std::string data) {
    data_ = std::move(data);
    NoteMutation();
  }
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
  void SetData(std::string data) {
    data_ = std::move(data);
    NoteMutation();
  }
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

  // The element this is the shadow root of, or null for every other fragment.
  //
  // Deliberately *not* the fragment's `parent_`: a shadow root with a parent
  // would be reachable by every ordinary tree walk -- the cascade, the script
  // collector, `querySelectorAll` -- and being unreachable that way is the whole
  // point of it. This is the one link back, and it is what event retargeting
  // walks and what tells a slot which children it may be filled from.
  Element* Host() const { return host_; }
  void SetHost(Element* host) { host_ = host; }

  // True for a `<template>`'s `.content` only. Custom elements inside one stay
  // inert until stamped (HTML). ShadyDOM roots and page-made fragments are
  // *not* this — they share "no Host()" with template content and must still
  // upgrade, or Polymer stamps into a host-less root and never constructs.
  bool IsTemplateContent() const { return template_content_; }
  void SetTemplateContent(bool value) { template_content_ = value; }

 private:
  Element* host_ = nullptr;
  bool template_content_ = false;
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

  // Whether the user has ever interacted with this document.
  //
  // ADR 0017 defines user activation and ADR 0028 §1 reads it: `play()` on an unmuted element
  // without it is `NotAllowedError`, which is what makes autoplay refusable at all. It lives
  // here, on the document, because that is its scope -- an interaction with one document does
  // not license another, which is the entire point of the flag.
  //
  // **Sticky rather than transient**, and that is a deliberate simplification with a named
  // consequence: the specification expires a *transient* activation after a few seconds so that
  // a click cannot license a popup a minute later. Nothing here opens a window or a dialog, so
  // the only consumer is media autoplay -- where sticky is what users expect ("I pressed play
  // once, stop asking"). When the first transient consumer arrives, this becomes a timestamp
  // and the comparison moves to the caller; it is written here so that change is a decision
  // rather than a surprise.
  //
  // Only the engine sets it, and only from a trusted event. A page that could set it would be a
  // page that could autoplay, which is the whole thing being prevented.
  bool HasUserActivation() const { return user_activation_; }
  void NoteUserActivation() { user_activation_ = true; }

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

  // Which element has focus, and whether it got it from the keyboard.
  //
  // One member rather than two, for the reason Page's LayoutState is one: they
  // are two facts about the same thing, and a `visible` bool loose on the
  // document would say nothing about which focus it describes.
  //
  // Focus is a *document* property (ADR 0017 §4) and this is the only copy of
  // it. The engine decides what may hold it and moves it; the binding layer
  // reports it as `document.activeElement` and moves it for `focus()`. A second
  // copy on either side is the pair that disagrees about where a key goes.
  //
  // `visible` is the `:focus-visible` heuristic: set when the keyboard moved
  // focus, cleared when a pointer did. A focus ring on every click is the
  // reason authors write `outline: none`, which is worse for the user than
  // either behaviour.
  struct FocusState {
    Element* element = nullptr;
    bool visible = false;
  };
  const FocusState& Focus() const { return focus_; }
  // Null clears it, which is what `blur()` and a click on nothing focusable do.
  void SetFocus(Element* element, bool visible) { focus_ = FocusState{element, visible}; }

  // First element with this tag name, in tree order.
  Element* FirstElementByTagName(std::string_view tag_name) const;
  std::vector<Element*> ElementsByTagName(std::string_view tag_name) const;

 private:
  bool quirks_ = false;
  bool user_activation_ = false;
  std::uint64_t mutation_version_ = 0;
  FocusState focus_;
};

// Whether an element cannot have children — `br`, `img`, `meta` and the rest.
// A tree builder that let one have children would nest the whole rest of the
// document inside an `<img>`.
bool IsVoidElement(std::string_view tag_name);

}  // namespace microbrowser::dom

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Namespaces.h"

namespace microbrowser::dom {

class Document;
class DocumentFragment;
class Element;

class DocumentFragment;
class Node;

// Parsed CSS text held by reference. A constructable stylesheet is shared by
// every root that adopts it; the cascade reads this rather than cloning text
// per root. ADR 0019 §4.
using SharedConstructableSheet = std::shared_ptr<std::string>;

// Which shadow roots the HTML fragment serializer descends into, and what a
// page's `getHTML` asked for.
//
// A shadow tree is *not* markup: it does not round-trip through `innerHTML`, and
// serializing one is opting a subtree the page kept separate into a string the
// page is about to hand somewhere else. So both fields default to "no shadow
// roots at all", and `innerHTML` -- which has no way to say otherwise -- gets
// exactly that.
struct SerializeOptions {
  // Include every root whose `serializable` was set when it was attached.
  bool serializable_shadow_roots = false;
  // Include these roots whatever their `serializable` says. Named explicitly by
  // the caller, so a page that holds a closed root can serialize the one it
  // owns without opting in every root on the page. Not owned; must outlive the
  // call.
  const std::vector<const DocumentFragment*>* shadow_roots = nullptr;
};

// The HTML fragment serialization algorithm, with the shadow-root extension.
//
// A free function and the single implementation: `Node::Serialize()` is this
// with default options. Two serializers -- one that knew about shadow roots and
// one that did not -- would be two answers to "what is this subtree", and the
// pair disagreeing is what makes a round trip lose a tree.
std::string SerializeNode(const Node& node, const SerializeOptions& options);
std::string SerializeNodeChildren(const Node& node, const SerializeOptions& options);

// HTML's "serialize an HTML fragment" -- what `innerHTML` and `getHTML` return,
// which is *not* the same as serializing `node`'s children.
//
// Two things it does that the plain child walk does not. A `<template>`
// serializes its contents rather than its children, because that is where its
// markup went. And **a shadow host emits its own root first**: the element being
// asked is a host as much as any element below it, and leaving its root out was
// worth 2,176 subtests of shadow-dom/declarative/gethtml.html -- the string a
// page compares against contains the template it just declared.
//
// A shadow *root* asked directly answers with its children and no wrapping
// template: it is already inside the tree the template would introduce.
std::string SerializeFragment(const Node& node, const SerializeOptions& options);

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
    // `<?target data?>`. Not something the HTML parser produces -- it turns one
    // into a comment (HTML §13.2.5, "bogus comment") -- so this exists for
    // `document.createProcessingInstruction` and for the XML parser that does
    // not exist yet. It is here rather than approximated by Comment because a
    // page asks `nodeName` and gets the *target* back, which a comment has no
    // room for.
    ProcessingInstruction,
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
  // a parse. A walk, over a depth ADR 0009 already bounds.
  //
  // **This is not `ownerDocument`, and the difference is the reason both
  // exist.** This one answers "is this node in a rendered tree, and which one",
  // which is what invalidation and the cascade ask -- and it must answer *null*
  // for a detached subtree, or every `createElement` while script assembles a
  // fragment would bump the live document's mutation version and force a
  // relayout that describes nothing. `NodeDocument` below is what a page calls
  // `ownerDocument`, and it answers that same detached node's document. Reading
  // one for the other is the bug this pair was split to make impossible.
  Document* ConnectedDocument() const;

  // The DOM's *node document* -- what `ownerDocument` answers.
  //
  // Stored, and the two-field split above is the whole reason it has to be:
  // the DOM assigns a node document when the node is *created* and it survives
  // detachment, so no walk over the tree can derive it. `createElement` on an
  // inert document followed by `remove()` must still answer that document, and
  // a walk answers null.
  //
  // Null only for a node created outside any document, which after this change
  // is nothing the binding layer makes -- the parser's nodes get theirs when
  // they are inserted.
  Document* NodeDocument() const { return node_document_; }

  // Sets the node document of this node and its whole subtree, which is the
  // DOM's "adopt". Cheap in the case that matters: an insertion whose subtree
  // is already in the right document stops at the first node.
  void SetNodeDocument(Document* document);

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
  // Finding the document is a walk to the root -- ConnectedDocument, and
  // deliberately not the stored `node_document_` beside it. The stored one is
  // right about a *detached* node, which is exactly the case that must not mark
  // anything: script building a subtree it has not inserted has changed nothing
  // the document derived. See the comment on ConnectedDocument.
  void NoteMutation();

  // Insert/remove/reorder — bumps Document::StructureVersion as well as the
  // mutation counter. Attribute writers keep NoteMutation alone so a style
  // cache can reuse cascade answers for untouched elements (TD-0021).
  void NoteStructureChange();

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
  // See NodeDocument. Borrowed: a document outlives every node whose node
  // document it is, because the binding layer holds every node script made
  // for the life of the page and a document owns its own tree.
  Document* node_document_ = nullptr;
  std::vector<std::unique_ptr<Node>> children_;
};

struct Attribute {
  // The *qualified* name: `prefix:local` when there is a prefix, `local`
  // otherwise. This is what `getAttribute` matches on and what serializes.
  std::string name;
  std::string value;
  // The namespace, and how many bytes of `name` are the prefix -- 0 for no
  // prefix, which is almost every attribute on almost every page.
  //
  // The length is stored rather than derived from the first colon, because the
  // two are different facts: an HTML parser that meets `<a xml:lang=x>` in an
  // HTML document produces an attribute whose *whole qualified name* is
  // `xml:lang` with no prefix and no namespace, and `attr.localName` must say
  // so. Deriving would call the `xml` part a prefix and answer `lang`.
  NamespaceRef name_space;
  std::uint32_t prefix_length = 0;

  std::string_view LocalName() const {
    return std::string_view(name).substr(prefix_length == 0 ? 0 : prefix_length + 1);
  }
  std::string_view Prefix() const {
    return std::string_view(name).substr(0, prefix_length);
  }

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

// What a shadow root remembers about how it was attached.
//
// A bitmask on the root rather than fields on the host, because every one of
// these is a fact about the *root* -- `attachShadow` takes them, `getHTML`
// reports them, and a host with no root has no opinion on any of them. Folding
// `TemplateContent` in keeps `DocumentFragment` at three members: the two kinds
// of parentless fragment already needed telling apart, and one mask answers
// both questions.
enum class ShadowFlags : std::uint8_t {
  None = 0,
  // `mode: "open"`. Not a security boundary -- see Element::ShadowRoot -- and
  // the one thing it changes is whether `element.shadowRoot` answers.
  Open = 1u << 0,
  DelegatesFocus = 1u << 1,
  // Whether cloning the host clones this root too.
  Clonable = 1u << 2,
  // Whether `getHTML({serializableShadowRoots: true})` includes it. Serializing
  // a root the page did not mark is how a shadow tree leaks into markup the
  // page then hands somewhere else, so the default is off.
  Serializable = 1u << 3,
  // Attached by the parser from `<template shadowrootmode>` rather than by
  // script. The DOM's "attach a shadow root" reads this: a *declarative* root
  // is the one case where a second `attachShadow` succeeds, emptying it and
  // clearing this bit rather than throwing.
  Declarative = 1u << 4,
  // A `<template>`'s contents rather than a shadow root at all. Custom elements
  // inside one stay inert until stamped. ShadyDOM roots and page-made fragments
  // are *not* this -- they share "no Host()" with template content and must
  // still upgrade, or Polymer stamps into a host-less root and never constructs.
  TemplateContent = 1u << 5,
  // `slotAssignment: "manual"`, where a slot is filled by `assign()` rather than
  // by matching names. The bit is the non-default direction so that a root
  // attached with no opinion is "named", which is what the DOM's default is.
  ManualSlotAssignment = 1u << 6,
};

constexpr ShadowFlags operator|(ShadowFlags a, ShadowFlags b) {
  return static_cast<ShadowFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr ShadowFlags operator&(ShadowFlags a, ShadowFlags b) {
  return static_cast<ShadowFlags>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr ShadowFlags operator~(ShadowFlags a) {
  return static_cast<ShadowFlags>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)));
}
constexpr ShadowFlags& operator|=(ShadowFlags& a, ShadowFlags b) { return a = a | b; }
constexpr ShadowFlags& operator&=(ShadowFlags& a, ShadowFlags b) { return a = a & b; }
constexpr bool Any(ShadowFlags flags) { return flags != ShadowFlags::None; }

// What `attachShadow` did, which the caller has to know because two of the three
// outcomes are not "here is a new root".
enum class ShadowAttachStatus : std::uint8_t {
  // A root that did not exist now does.
  Created,
  // The host already had a *declarative* root, so this returned that one with
  // its children removed -- which is what the DOM says and what lets a page
  // reach into a closed declarative root by calling `attachShadow` again.
  Reused,
  // Refused: not a shadow host element, or a root already exists whose mode
  // does not match. `NotSupportedError` at every caller above this one.
  Refused,
};

struct ShadowAttachResult {
  DocumentFragment* root = nullptr;
  ShadowAttachStatus status = ShadowAttachStatus::Refused;
};

// Whether "attach a shadow root" would accept this element (DOM §"attach a
// shadow root" step 2): an HTML element whose local name is on the safelist, or
// any valid custom element name.
//
// In `dom` rather than at either caller because both the parser and the binding
// layer ask it, and a safelist with two copies is a safelist with two answers.
bool CanHostShadowRoot(const NamespaceRef& name_space, std::string_view local_name);

class Element : public Node {
 public:
  // An HTML element with no prefix, which is what the parser and
  // `createElement` both produce.
  explicit Element(std::string tag_name);
  // A namespaced element. `prefix_length` is how many bytes of the qualified
  // name are the prefix, 0 for none -- see Attribute for why it is stored
  // rather than found by looking for a colon.
  Element(NamespaceRef name_space, std::string qualified_name, std::uint32_t prefix_length);
  ~Element() override;

  // The qualified name, as created: `svg` for an HTML `<svg>`, `x:b` for
  // `createElementNS(ns, 'x:b')`. Lower-cased for anything the HTML parser or
  // `createElement` made, and case-preserving for `createElementNS`.
  //
  // Every match in the engine -- the cascade, the box tree, hit testing --
  // is against this. `tagName` in a page is this upper-cased, which is the
  // binding layer's job rather than the tree's.
  const std::string& TagName() const { return tag_name_; }
  std::string_view LocalName() const {
    return std::string_view(tag_name_).substr(prefix_length_ == 0 ? 0 : prefix_length_ + 1);
  }
  std::string_view Prefix() const {
    return std::string_view(tag_name_).substr(0, prefix_length_);
  }
  const NamespaceRef& Namespace() const { return namespace_; }

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
  bool ShadowIsOpen() const;
  // The DOM's "attach a shadow root", whole: the safelist, the mode check, and
  // the one case where attaching twice is not an error.
  //
  // Three outcomes rather than a pointer and a bool, because the callers do
  // three different things with them -- script throws `NotSupportedError` on
  // Refused, and the parser *inserts the template it was going to discard*,
  // which is how `<progress><template shadowrootmode=open>` leaves the template
  // in the tree instead of throwing at a page that only wrote markup.
  //
  // **`Reused` leaves the root's children in place and the caller owes their
  // removal.** The DOM says to empty it, and this cannot: emptying means
  // destroying nodes, script may hold a wrapper for any of them, and the module
  // note at the top of MODULE.deps is the reason nothing in `dom` frees a node.
  // `DomBindings::ClearChildren` is the one that can, and the one caller that
  // can reach this case -- `attachShadow` -- calls it.
  ShadowAttachResult AttachShadow(ShadowFlags flags);

  const std::vector<Attribute>& Attributes() const { return attributes_; }

  // By *qualified* name, which is what `getAttribute` matches on: two
  // attributes in different namespaces can share a local name, and the first
  // in tree order with this qualified name is the answer.
  const std::string* GetAttribute(std::string_view name) const;
  bool HasAttribute(std::string_view name) const { return GetAttribute(name) != nullptr; }
  void SetAttribute(std::string name, std::string value);
  bool RemoveAttribute(std::string_view name);

  // By namespace and *local* name, which is what the `…NS` half matches on.
  // Setting keeps an existing attribute's position and replaces its value and
  // its prefix, because that is what the DOM says and because the order
  // `getAttributeNames` reports is observable.
  const Attribute* GetAttributeNS(const NamespaceRef& name_space,
                                  std::string_view local_name) const;
  void SetAttributeNS(NamespaceRef name_space, std::string qualified_name,
                      std::uint32_t prefix_length, std::string value);
  bool RemoveAttributeNS(const NamespaceRef& name_space, std::string_view local_name);

  // Bumped on SetAttribute / RemoveAttribute only. Paired with Document::
  // StructureVersion for the style cache of TD-0021: an attribute write on one
  // element must not force every other element's cascade to re-run.
  std::uint32_t AttrVersion() const { return attr_version_; }

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
  // Allocated only for `<template>`. A pointer on every element rather than a
  // subclass, because the parser and the bindings both create elements by tag
  // name and neither has anywhere to put a second type.
  std::unique_ptr<DocumentFragment> content_;
  // Allocated only for a host, for the reason `content_` is: the parser and the
  // bindings both make elements by tag name and neither has anywhere to put a
  // second type.
  std::unique_ptr<DocumentFragment> shadow_;
  // The four small fields are together at the end deliberately: they pack into
  // the padding the two pointers above already leave, so an element that
  // remembers its namespace and its prefix is the same size as one that did
  // not.
  std::uint32_t prefix_length_ = 0;
  std::uint32_t attr_version_ = 0;
  NamespaceRef namespace_ = NamespaceRef::kHtml;
  ElementState state_ = ElementState::None;
};

class Text : public Node {
 public:
  explicit Text(std::string data, bool cdata = false)
      : Node(Kind::Text), data_(std::move(data)), cdata_(cdata) {}

  // A CDATASection *is* a Text in the DOM -- it derives from it, and every
  // algorithm that reads character data, splits at an offset or measures a
  // node's length treats the two identically. The only things that differ are
  // `nodeType` (4) and `nodeName` (`#cdata-section`), so this is a flag rather
  // than a `Kind`: a seventh Kind would have to be added to a dozen exhaustive
  // switches that all wanted to answer "text" anyway, and the one that got
  // missed would be a silently wrong tree-order or length answer.
  //
  // Only an XML document can hold one; `createCDATASection` is the sole way to
  // make one here, and it refuses in an HTML document.
  bool IsCData() const { return cdata_; }

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
  bool cdata_ = false;
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
  DocumentType(std::string name, std::string public_id = {}, std::string system_id = {})
      : Node(Kind::DocumentType),
        name_(std::move(name)),
        public_id_(std::move(public_id)),
        system_id_(std::move(system_id)) {}
  const std::string& Name() const { return name_; }
  // Kept rather than dropped, and they are not decoration: `<!DOCTYPE html
  // PUBLIC "-//W3C//DTD HTML 4.01//EN">` is what puts a page in quirks or
  // limited-quirks mode, so the strings the tokenizer already produced are the
  // input to a rendering decision this browser does not yet make -- and
  // `createDocumentType` round-trips them either way.
  const std::string& PublicId() const { return public_id_; }
  const std::string& SystemId() const { return system_id_; }
  std::string Serialize() const override;

 private:
  std::string name_;
  std::string public_id_;
  std::string system_id_;
};

// `<?xml-stylesheet href="x"?>` -- a target and the rest of the data.
//
// A CharacterData in the DOM, which is what makes `data`, `substringData` and
// the rest of that interface apply to it, and what the binding layer files it
// under.
class ProcessingInstruction : public Node {
 public:
  ProcessingInstruction(std::string target, std::string data)
      : Node(Kind::ProcessingInstruction),
        target_(std::move(target)),
        data_(std::move(data)) {}
  const std::string& Target() const { return target_; }
  const std::string& Data() const { return data_; }
  void SetData(std::string data) {
    data_ = std::move(data);
    NoteMutation();
  }
  std::string Serialize() const override;

 private:
  std::string target_;
  std::string data_;
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
  bool IsTemplateContent() const { return Any(flags_ & ShadowFlags::TemplateContent); }
  void SetTemplateContent(bool value) { SetShadowFlag(ShadowFlags::TemplateContent, value); }

  // How this root was attached. Meaningless on a fragment that is not one, which
  // is why every reader below goes through Host() or through the binding layer's
  // ShadowRoot interface rather than asking a bare fragment.
  ShadowFlags Flags() const { return flags_; }
  void SetFlags(ShadowFlags flags) { flags_ = flags; }
  void SetShadowFlag(ShadowFlags flag, bool on) {
    flags_ = on ? (flags_ | flag) : (flags_ & ~flag);
  }
  bool IsOpen() const { return Any(flags_ & ShadowFlags::Open); }
  bool DelegatesFocus() const { return Any(flags_ & ShadowFlags::DelegatesFocus); }
  bool IsClonable() const { return Any(flags_ & ShadowFlags::Clonable); }
  bool IsSerializable() const { return Any(flags_ & ShadowFlags::Serializable); }
  bool IsDeclarative() const { return Any(flags_ & ShadowFlags::Declarative); }
  bool HasManualSlotAssignment() const {
    return Any(flags_ & ShadowFlags::ManualSlotAssignment);
  }

  // Constructable stylesheets adopted by this root. Empty on every fragment
  // that is not a shadow root or the document.
  const std::vector<SharedConstructableSheet>& AdoptedStyleSheets() const {
    return adopted_style_sheets_;
  }
  void SetAdoptedStyleSheets(std::vector<SharedConstructableSheet> sheets) {
    adopted_style_sheets_ = std::move(sheets);
  }

 private:
  Element* host_ = nullptr;
  ShadowFlags flags_ = ShadowFlags::None;
  std::vector<SharedConstructableSheet> adopted_style_sheets_;
};

class Document : public Node {
 public:
  // Its own node document, which is what the DOM says and what makes every
  // node inserted into it inherit one.
  Document() : Node(Kind::Document) { SetNodeDocument(this); }

  // Quirks mode is a rendering decision that comes from the doctype, and it has
  // to be carried on the document because layout asks about it long after the
  // doctype token is gone.
  bool InQuirksMode() const { return quirks_; }
  void SetQuirksMode(bool quirks) { quirks_ = quirks; }

  // Whether this is an HTML document or an XML one -- the DOM's document
  // "type", and true for every document until `DOMParser` made one that is not.
  //
  // It lives here rather than on the binding layer's wrapper because two things
  // in the *tree* branch on it and neither can see a wrapper:
  // `createElement` folds its argument to lower case and puts the element in the
  // HTML namespace only in an HTML document, and `tagName` upper-cases only
  // there. An XML document whose elements were lower-cased on the way in and
  // upper-cased on the way out round-trips to a different name.
  bool IsHtmlDocument() const { return html_; }
  void SetHtmlDocument(bool html) { html_ = html; }

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
  // Tree shape only (insert/remove/reorder). Attribute writes bump
  // MutationVersion without this, so a style cache can keep answers for
  // unchanged subtrees across Polymer property stamps (TD-0021).
  std::uint64_t StructureVersion() const { return structure_version_; }
  // Named apart from Node::NoteMutation, which is the walk that reaches this.
  // Two members of one hierarchy with the same name and different jobs is how
  // a call ends up meaning the other one.
  void NoteTreeMutation() { ++mutation_version_; }
  void NoteStructureMutation() {
    ++mutation_version_;
    ++structure_version_;
  }

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

  // `document.adoptedStyleSheets` — document-wide author rules, held by
  // reference like a shadow root's adopted sheets. ADR 0019 §4.
  const std::vector<SharedConstructableSheet>& AdoptedStyleSheets() const {
    return adopted_style_sheets_;
  }
  void SetAdoptedStyleSheets(std::vector<SharedConstructableSheet> sheets) {
    adopted_style_sheets_ = std::move(sheets);
  }

 private:
  bool quirks_ = false;
  bool html_ = true;
  bool user_activation_ = false;
  std::uint64_t mutation_version_ = 0;
  std::uint64_t structure_version_ = 0;
  FocusState focus_;
  std::vector<SharedConstructableSheet> adopted_style_sheets_;
};

// Whether an element cannot have children — `br`, `img`, `meta` and the rest.
// A tree builder that let one have children would nest the whole rest of the
// document inside an `<img>`.
bool IsVoidElement(std::string_view tag_name);

}  // namespace microbrowser::dom

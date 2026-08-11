#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/Geometry.h"
#include "dom/Node.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Value.h"
#include "util/StringUtil.h"

// Shared by the binding translation units, and private to the module: a
// binding is an implementation detail of the seam, not part of its interface,
// so this header is deliberately absent from MODULE.deps' `public:` list.

namespace microbrowser::bindings {

// Where a wrapper keeps the node it stands for, and where a native keeps the
// bindings instance it belongs to. `#` names, the same convention private
// class fields and the engine's other internal slots already use.
inline constexpr const char* kNodeSlot = "#node";
inline constexpr const char* kOwnerSlot = "#bindings";
inline constexpr const char* kBlobBodySlot = "#blobBody";
inline constexpr const char* kBlobTypeSlot = "#blobType";
inline constexpr const char* kBlobMarkerSlot = "#isBlob";
// Where the document wrapper keeps its `readyState`. A hidden property rather
// than a C++ member, because the collector can see a property and cannot see a
// `js::Value` in a field -- the same rule the wrapper cache follows.
inline constexpr const char* kReadyStateSlot = "#readyState";
// The document a `DOMImplementation` belongs to. One implementation object per
// document, because `doc.implementation.createDocumentType(...)` must answer a
// node whose `ownerDocument` is `doc` -- and a single shared object has no way
// to know which document was asked. Deliberately not kNodeSlot: an object that
// answered NodeOf would be reachable by every Node method a page cared to
// `.call()` on it.
inline constexpr const char* kImplementationDocumentSlot = "#implDocument";
// The MIME type a document was parsed as, on the document's wrapper for the
// reason `readyState` is there: it is a fact about how the object was *made*
// rather than about the tree, and `dom::Document` is shared with an engine that
// has no notion of one. `DOMParser` writes it; `document.contentType` reads it.
inline constexpr const char* kContentTypeSlot = "#contentType";
// The URL a document reports when it is not the page's own -- `about:blank` for
// every document script builds. Beside kContentTypeSlot because the two are set
// together by everything that makes a document: `DOMParser`, and
// `implementation.createDocument`.
inline constexpr const char* kDocumentUrlSlot = "#documentUrl";
// Where a document wrapper keeps the one DOMImplementation it hands out, so
// that `document.implementation === document.implementation`.
inline constexpr const char* kImplementationSlot = "#implementation";
inline constexpr const char* kCSSStyleSheetMarkerSlot = "#isCSSStyleSheet";
// Where an element's wrapper keeps its one `DOMTokenList`. The list is live,
// so this is identity rather than a cache: `el.classList === el.classList` is
// something pages assert, and a fresh object per read answers false.
inline constexpr const char* kClassListSlot = "#classList";
// A heap-allocated `std::shared_ptr<std::string>` holding the parsed text.
// Shared by every root that adopts the sheet; `replaceSync` mutates it in place.
inline constexpr const char* kCSSSheetStorageSlot = "#cssSheetStorage";

// A C++ pointer, as a value script can hold but not usefully forge. It travels
// as a double, which holds a 53-bit integer exactly -- more than any address on
// a machine with canonical-form pointers.
inline js::Value PointerValue(const void* pointer) {
  return js::Value::Number(static_cast<double>(reinterpret_cast<std::uintptr_t>(pointer)));
}

// An argument, or undefined when the caller passed fewer.
//
// A local copy rather than `js::Argument`, which lives in a header `src/js`
// keeps to itself. That is the module boundary working: a binding is a
// consumer of the engine's *public* surface, and reaching past it for a
// three-line helper would be the first crack in the line this module's
// manifest calls a security boundary.
inline js::Value Argument(const std::vector<js::Value>& arguments, std::size_t index) {
  return index < arguments.size() ? arguments[index] : js::Value::Undefined();
}

// Convert a binding argument the way script operators do: `@@toPrimitive` /
// `toString` / `valueOf`. `js::ToString` invents "[object Object]" for ordinary
// objects and cannot see `Location`/`URL.prototype.toString` — which is how
// `new URL(location)` became `https://…/[object%20Object]` and youtube's
// consent `continue=` URL broke.
inline bool CoerceToString(js::NativeCall& call, const js::Value& value, std::string& out) {
  const js::Result converted = call.interpreter.ToStringOf(value, out);
  if (converted.IsAbrupt()) {
    (void)call.ThrowValue(converted.value);
    return false;
  }
  return true;
}

// The same conversion, to a Web IDL **USVString** rather than a DOMString: a JavaScript string is
// a sequence of UTF-16 code units and may hold an unpaired surrogate, and a USVString may not, so
// every lone one becomes U+FFFD.
//
// It is a separate function rather than a flag because the choice belongs to the *interface*: the
// URL Standard takes USVStrings everywhere, because a URL becomes bytes on a wire and a lone
// surrogate has no encoding. `setAttribute` takes a DOMString and must keep whatever it was given.
inline bool CoerceToUsvString(js::NativeCall& call, const js::Value& value, std::string& out) {
  if (!CoerceToString(call, value, out)) {
    return false;
  }
  out = util::Utf8DecodeLossy(out);
  return true;
}

// `DOMException`: the type a *web API* throws, as against the `Error` types the
// language throws. Defined in DomExceptions.cpp, where the WebIDL error-names
// table lives.
//
// `ThrowDom` is the only way a binding should raise one, and it is a free
// function rather than a method on anything because the call sites are natives
// with no receiver in common. `name` is a WebIDL error name -- "NotFoundError",
// "InvalidStateError" -- and the numeric `code` a page reads is derived from it
// rather than passed, so the two cannot disagree.
void InstallDomException(js::Interpreter& interpreter);
js::Value MakeDomException(js::Interpreter& interpreter, std::string_view name,
                           std::string message);
js::Value ThrowDom(js::NativeCall& call, std::string_view name, std::string message);

class DomBindings;

// What a `Proxy` wraps, following a chain of them, or the object itself.
//
// A live binding object -- `el.style`, `el.dataset`, `el.classList` -- is a
// Proxy over a plain target that carries the element. When such an object is
// the *receiver* of a method read off a shared prototype, `this` is the proxy
// and the element is on the target behind it, so every "which node is this?"
// question has to look through. Bounded, because a page can nest proxies.
inline js::Object* BehindProxies(js::Object* object) {
  for (int depth = 0; object != nullptr && depth < 16; ++depth) {
    if (object->GetKind() != js::Object::Kind::Proxy) {
      return object;
    }
    const js::Value* behind = object->GetOwn("#target");
    object = behind != nullptr && behind->IsObject() ? behind->object : nullptr;
  }
  return object;
}

// The node behind a wrapper, or null for anything that is not one.
//
// Every binding starts here rather than trusting its receiver, because a page
// can call one on anything: `Element.prototype.appendChild.call(7, x)` is legal
// JavaScript and must be a TypeError rather than a jump through a bad pointer.
inline dom::Node* NodeOf(const js::Value& value) {
  if (!value.IsObject()) {
    return nullptr;
  }
  const js::Object* behind = BehindProxies(value.object);
  const js::Value* slot = behind == nullptr ? nullptr : behind->GetOwn(kNodeSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(slot->number));
}

// ---------------------------------------------------------------------------
// Tree order, and the boundary points written against it.
//
// These four were the private top of Ranges.cpp until `compareDocumentPosition`
// and the content-mutation half needed them too. They are here rather than
// copied because **tree order is the thing three files must agree about**: a
// second implementation of "which of these comes first" does not fail loudly,
// it disagrees with the first one about some case nobody wrote a test for, and
// then `range.toString()` and `a.compareDocumentPosition(b)` describe different
// documents.
// ---------------------------------------------------------------------------

// How many positions there are inside `node`: its character count for a text
// node or a comment, and its child count for anything else. The specification
// calls this the node's length, and it is what bounds an offset.
inline std::size_t NodeLength(const dom::Node& node) {
  switch (node.GetKind()) {
    case dom::Node::Kind::Text:
      return static_cast<const dom::Text&>(node).Data().size();
    case dom::Node::Kind::Comment:
      return static_cast<const dom::Comment&>(node).Data().size();
    case dom::Node::Kind::ProcessingInstruction:
      return static_cast<const dom::ProcessingInstruction&>(node).Data().size();
    default:
      return node.Children().size();
  }
}

// Where `node` sits among its parent's children.
inline std::size_t IndexIn(const dom::Node& node) {
  const dom::Node* parent = node.Parent();
  if (parent == nullptr) {
    return 0;
  }
  const std::vector<std::unique_ptr<dom::Node>>& children = parent->Children();
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (children[i].get() == &node) {
      return i;
    }
  }
  return 0;
}

// The chain from `node` up to its root, root first. Bounded because a tree
// built by script is a tree built by script.
inline std::vector<const dom::Node*> AncestorsOf(const dom::Node& node) {
  std::vector<const dom::Node*> chain;
  constexpr std::size_t kMaxDepth = 100'000;
  for (const dom::Node* at = &node; at != nullptr && chain.size() < kMaxDepth;
       at = at->Parent()) {
    chain.push_back(at);
  }
  // Root first, which is what makes the common-prefix walk below a loop with
  // one index rather than two.
  std::reverse(chain.begin(), chain.end());
  return chain;
}

// **The one ordering function.** -1, 0 or 1 for (a, a_offset) against
// (b, b_offset) in tree order, and 2 for two points in different trees, which
// the specification makes a WrongDocumentError.
//
// Everything written against boundary points is written against this:
// `collapsed` is "equal", `compareBoundaryPoints` is this directly, and
// `comparePoint` is this twice.
inline int ComparePoints(const dom::Node& a, std::size_t a_offset, const dom::Node& b,
                         std::size_t b_offset) {
  if (&a == &b) {
    if (a_offset == b_offset) {
      return 0;
    }
    return a_offset < b_offset ? -1 : 1;
  }
  const std::vector<const dom::Node*> a_chain = AncestorsOf(a);
  const std::vector<const dom::Node*> b_chain = AncestorsOf(b);
  if (a_chain.empty() || b_chain.empty() || a_chain.front() != b_chain.front()) {
    return 2;  // different trees
  }
  // Walk down the common prefix. The first place they differ decides, and the
  // comparison there is between two *children of the same parent*, which is an
  // index comparison.
  std::size_t depth = 0;
  while (depth < a_chain.size() && depth < b_chain.size() &&
         a_chain[depth] == b_chain[depth]) {
    ++depth;
  }
  if (depth >= a_chain.size()) {
    // `a` is an ancestor of `b`: the point in `a` is before the point in `b`
    // exactly when a_offset is at or before the child that leads down to `b`.
    const std::size_t child = IndexIn(*b_chain[depth]);
    return a_offset <= child ? -1 : 1;
  }
  if (depth >= b_chain.size()) {
    const std::size_t child = IndexIn(*a_chain[depth]);
    return b_offset <= child ? 1 : -1;
  }
  const std::size_t a_index = IndexIn(*a_chain[depth]);
  const std::size_t b_index = IndexIn(*b_chain[depth]);
  return a_index < b_index ? -1 : 1;
}

// Is `candidate` `root`, or somewhere under it? Inclusive, which is the
// specification's and the surprising half: a node contains itself, and a
// polyfill that walks up asking `root.contains(node)` depends on it
// terminating.
//
// Here rather than in a translation unit because three files had their own copy
// of it -- ElementQueries.cpp for `contains`, TreeWalkers.cpp for the walk
// limit, and RangeContents.cpp for "is the start an inclusive ancestor of the
// end", which is the question every content-mutation step opens with.
inline bool IsInclusiveDescendant(const dom::Node* candidate, const dom::Node* root) {
  for (const dom::Node* at = candidate; at != nullptr; at = at->Parent()) {
    if (at == root) {
      return true;
    }
  }
  return false;
}

// The node a boundary point is rooted at: the topmost thing you reach walking
// up. Two points are comparable exactly when these are the same node.
inline const dom::Node* RootOf(const dom::Node& node) {
  const dom::Node* at = &node;
  for (int depth = 0; at->Parent() != nullptr && depth < 100'000; ++depth) {
    at = at->Parent();
  }
  return at;
}

// A property key as an array index, or `kNotAnIndex`.
//
// The canonical form only: "0", "1", … and nothing with a leading zero, a
// sign or a decimal point. That strictness is the specification's -- `list["01"]`
// is a named property and not element 1 -- and it is what keeps an indexed
// getter from shadowing a name a page put on the object.
inline constexpr std::size_t kNotAnIndex = static_cast<std::size_t>(-1);
inline std::size_t ArrayIndexOf(std::string_view text) {
  if (text.empty() || text.size() > 10) {
    return kNotAnIndex;
  }
  if (text.size() > 1 && text[0] == '0') {
    return kNotAnIndex;
  }
  std::size_t index = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return kNotAnIndex;
    }
    index = index * 10 + static_cast<std::size_t>(c - '0');
  }
  return index > 4294967294ULL ? kNotAnIndex : index;
}

// The key a Proxy trap was handed. It arrives as a value -- a string or a
// symbol -- and every trap in this module has to turn it back into the key it
// came from without letting a symbol's description collide with a string.
inline js::PropertyKey KeyOfTrapArgument(const js::Value& value) {
  if (value.IsSymbol()) {
    return js::PropertyKey::Symbol(value.object);
  }
  return js::PropertyKey(js::ToString(value));
}

// The bindings instance a native belongs to. Carried on the function object
// rather than captured, because a capture is invisible to the collector and a
// raw pointer in one is a lifetime nobody is tracking.
inline DomBindings* OwnerOf(const js::NativeCall& call) {
  const js::Value* slot = call.callee == nullptr ? nullptr : call.callee->GetOwn(kOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<DomBindings*>(static_cast<std::uintptr_t>(slot->number));
}

// A rectangle as a page reads one: the eight members of a `DOMRect`, where
// `x`/`y`/`width`/`height` and `top`/`right`/`bottom`/`left` are the same four
// numbers under two names.
//
// One implementation rather than one per caller, which is not tidiness: a page
// that gets one set of names and not the other silently computes zero, and
// `getBoundingClientRect`, an intersection record and a resize record all have
// to answer with the same object shape. Defined in GeometryBindings.cpp.
js::Value MakeDomRect(js::Interpreter& interpreter, const GeometryRect& rect);

// The one ASCII lower-caser, in util. This name stays because it is used at
// forty call sites in this module and `util::` on each of them buys nothing.
inline std::string LowerCase(std::string_view text) { return util::AsciiLowerCase(text); }

// The name `setAttribute`, `getAttribute`, `hasAttribute`, `removeAttribute`
// and `toggleAttribute` actually look for.
//
// The DOM lower-cases the argument when "the element is in the HTML namespace
// and its node document is an HTML document" -- and both halves matter: every
// document here is an HTML one, but `createElementNS('http://FOO', 'a')` is
// not an HTML element, and `setAttribute('X', …)` on it must create an
// attribute called `X`. Lower-casing unconditionally is what made
// `getAttribute('X')` on a foreign element answer about a different attribute.
inline std::string AttributeNameFor(const dom::Element& element, std::string_view name) {
  return element.Namespace().IsHtml() ? util::AsciiLowerCase(name) : std::string(name);
}

// The `(namespace, localName)` pair `getElementsByTagNameNS` filters on, read
// off a call once rather than per element.
//
// `"*"` is a wildcard in either position, and it is not the same as null:
// `getElementsByTagNameNS("*", "a")` finds an `a` in any namespace, and
// `getElementsByTagNameNS(null, "a")` finds only one in none. Collapsing the
// two is the mistake this class exists to make impossible.
class NamespaceQuery {
 public:
  NamespaceQuery(const js::Value& namespace_argument, const js::Value& local_name_argument)
      : any_namespace_(namespace_argument.IsString() && *namespace_argument.string == "*"),
        any_local_name_(local_name_argument.IsString() &&
                        *local_name_argument.string == "*"),
        local_name_(js::ToString(local_name_argument)) {
    if (!any_namespace_ && !namespace_argument.IsNull() &&
        !namespace_argument.IsUndefined()) {
      name_space_ = dom::NamespaceRef(js::ToString(namespace_argument));
    }
  }

  bool Matches(const dom::Element& element) const {
    return (any_namespace_ || element.Namespace() == name_space_) &&
           (any_local_name_ || element.LocalName() == local_name_);
  }

 private:
  bool any_namespace_ = false;
  bool any_local_name_ = false;
  dom::NamespaceRef name_space_;
  std::string local_name_;
};

// Whether `element` answers to `getElementsByTagName(qualified)`.
//
// The rule reads oddly and is worth stating once: in an HTML document an HTML
// element matches case-insensitively and everything else matches exactly, so
// one document can hold an `<a>` that `getElementsByTagName('A')` finds and an
// `<A>` in a foreign namespace that it does not. `lowered` is the caller's
// lower-cased copy, hoisted because the walk asks this per element.
inline bool MatchesTagName(const dom::Element& element, std::string_view qualified,
                           std::string_view lowered) {
  if (qualified == "*") {
    return true;
  }
  return element.Namespace().IsHtml() ? element.TagName() == lowered
                                      : element.TagName() == qualified;
}

// What a node calls itself: `nodeName`, and for an element `tagName` too.
//
// **They are the same string, and this exists because they were not.**
// `nodeName` upper-cased the parser's tag name and `tagName` handed back the
// stored lower-case one, with a comment saying the two "deliberately differ".
// The DOM says the opposite -- Element.tagName *is* its qualified name, which
// for an HTML element in an HTML document is upper case -- and every browser
// agrees, so `if (el.tagName === 'SCRIPT')` is written everywhere and was
// silently false here. One function so a third caller cannot invent a third
// answer.
//
// Upper case unconditionally, which is right for this browser rather than a
// simplification: the rule is really "HTML elements in HTML documents", and
// there is one tree here with no XML in it. SVG is rendered from its own
// decoder and never becomes elements.
std::string NodeNameOf(const dom::Node& node);

// A deep or shallow copy of `node`, for `cloneNode` and `document.importNode`.
//
// One implementation rather than two: Polymer stamps a template with
// `document.importNode(template.content, true)`, and until that existed the
// call threw and every custom element's template stayed empty -- which is why
// youtube.com painted a white page with two upgraded hosts and no shadow
// trees. `cloneNode` and `importNode` must describe the same tree.
std::unique_ptr<dom::Node> CloneDomNode(const dom::Node& node, bool deep);

// The three node kinds the DOM files under CharacterData -- Text, Comment and
// ProcessingInstruction -- and the data of one.
//
// Here rather than repeated at each reader because the failure mode is silent:
// a kind missing from one copy of the condition is a node whose `data` reads
// and will not write, or whose `nodeValue` is null when the DOM says it is a
// string.
bool IsCharacterDataNode(const dom::Node& node);

// WebIDL's `length` for a native method: the number of arguments before the
// first optional or variadic one.
//
// Native functions here have no `length` at all by default, and that is
// observable in a way that bites: `pre-insertion-validation-hierarchy.js`
// branches on `parent[method].length > 1` to decide whether to pass a second
// argument, so a missing `length` sent every one of its checks down the
// one-argument path and made six tests fail with the *arity* TypeError instead
// of the HierarchyRequestError they were testing for. `ChildNode-remove.js`
// asserts `node.remove.length === 0` outright.
//
// Set where the number is part of the contract rather than everywhere, and the
// general gap -- every native binding should carry its IDL arity -- is written
// down here because this is where the next person will look.
inline void SetFunctionLength(const js::Value& function, double length) {
  if (function.IsObject()) {
    function.object->Set("length", js::Value::Number(length));
  }
}

// The node after `node` among its parent's children, or null. Null too for a
// node with no parent, which is what makes it safe to use as an insertion
// reference without a second check.
inline dom::Node* NextSiblingOf(const dom::Node& node) {
  const dom::Node* parent = node.Parent();
  if (parent == nullptr) {
    return nullptr;
  }
  const std::vector<std::unique_ptr<dom::Node>>& children = parent->Children();
  for (std::size_t i = 0; i + 1 < children.size(); ++i) {
    if (children[i].get() == &node) {
      return children[i + 1].get();
    }
  }
  return nullptr;
}

// The node before `node`, on the same terms. The pair is what a MutationRecord
// reports as `previousSibling`/`nextSibling`, which is how an observer places
// a change without walking the tree the change already moved.
inline dom::Node* PreviousSiblingOf(const dom::Node& node) {
  const dom::Node* parent = node.Parent();
  if (parent == nullptr) {
    return nullptr;
  }
  const std::vector<std::unique_ptr<dom::Node>>& children = parent->Children();
  for (std::size_t i = 1; i < children.size(); ++i) {
    if (children[i].get() == &node) {
      return children[i - 1].get();
    }
  }
  return nullptr;
}
// DOM S4.2.3, "ensure pre-insertion validity": the WebIDL error name this
// insertion must be refused with, or null when it is allowed.
//
// One function for all three of `appendChild`, `insertBefore` and
// `replaceChild`, because the specification has one and three copies is three
// chances to disagree. Before this the checks were absent: appending a document
// to an element, or anything at all to a text node, quietly built a tree no
// other browser would produce -- and the *reason* a page cares is that the DOM
// answers these with an exception it can catch, not with a corrupt tree it
// cannot see.
// `replacing` is null for an insertion and the outgoing child for a
// `replaceChild`. The DOM states the two algorithms separately and they differ
// in exactly that: every "does the parent already have one" question below
// must not count the node that is on its way out.
// `replacing_all` is `replaceChildren`, which empties the parent *before* it
// inserts and so must not be refused by what is on its way out: a document
// already holding a doctype and an element still accepts
// `doc.replaceChildren(el)`. whatwg/dom#1045 is the issue, and
// ParentNode-replaceChildren.html states all three cases.
inline const char* PreInsertionError(const dom::Node& parent, const dom::Node& node,
                                     const dom::Node* reference,
                                     const dom::Node* replacing = nullptr,
                                     bool replacing_all = false) {
  // 1. Only these three can have children.
  switch (parent.GetKind()) {
    case dom::Node::Kind::Document:
    case dom::Node::Kind::DocumentFragment:
    case dom::Node::Kind::Element:
      break;
    case dom::Node::Kind::DocumentType:
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
      return "HierarchyRequestError";
  }
  // 2. A node cannot be inserted into itself or into its own descendant. The
  // walk is up from the parent, which is bounded by the tree's depth -- and it
  // is the check that stops a page turning its document into a cycle.
  for (const dom::Node* walk = &parent; walk != nullptr; walk = walk->Parent()) {
    if (walk == &node) {
      return "HierarchyRequestError";
    }
  }
  // 3. The reference node has to be a child of this parent. `null` means
  // "append", which is always in range.
  if (reference != nullptr && reference->Parent() != &parent) {
    return "NotFoundError";
  }
  // 4. And only these four kinds can be inserted at all.
  switch (node.GetKind()) {
    case dom::Node::Kind::DocumentFragment:
    case dom::Node::Kind::DocumentType:
    case dom::Node::Kind::Element:
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
      break;
    case dom::Node::Kind::Document:
      return "HierarchyRequestError";
  }
  // 5. Text does not belong directly in a document, and a doctype belongs
  // nowhere else.
  const bool parent_is_document = parent.GetKind() == dom::Node::Kind::Document;
  if (node.IsText() && parent_is_document) {
    return "HierarchyRequestError";
  }
  if (node.GetKind() == dom::Node::Kind::DocumentType && !parent_is_document) {
    return "HierarchyRequestError";
  }
  if (!parent_is_document) {
    return nullptr;
  }
  // 6. **A document has at most one element child and at most one doctype, and
  // the doctype comes first.** These were left out until 2026-08-11 on the
  // argument that "a page that reaches them is doing something no page does",
  // and the suite priced that at 41 subtests across Node-insertBefore.html and
  // Node-replaceChild.html. They are also the constraints that keep
  // `document.documentElement` a question with one answer.
  //
  // `replacing` is the child being replaced, which is excluded from every count
  // below: `replaceChild(newHtml, oldHtml)` is legal precisely because the
  // element that is in the way is the one going out.
  const auto has_child = [&parent, replacing, replacing_all](dom::Node::Kind kind,
                                                            const dom::Node* except) {
    if (replacing_all) {
      return false;
    }
    for (const std::unique_ptr<dom::Node>& child : parent.Children()) {
      if (child.get() != except && child.get() != replacing && child->GetKind() == kind) {
        return true;
      }
    }
    return false;
  };
  // "A doctype is following `child`" / "an element is preceding `child`".
  const auto sibling_of_kind = [&parent, reference](dom::Node::Kind kind, bool after) {
    bool seen_reference = false;
    for (const std::unique_ptr<dom::Node>& child : parent.Children()) {
      if (child.get() == reference) {
        seen_reference = true;
        continue;
      }
      if (child->GetKind() == kind && seen_reference == after) {
        return true;
      }
    }
    return false;
  };
  // "child is a doctype, or a doctype is following child" -- the first half of
  // which is an *insertion* rule only: when `child` is being replaced it is on
  // its way out and cannot be in the way of anything.
  const auto doctype_is_in_the_way = [&sibling_of_kind, reference, replacing]() {
    if (reference == nullptr) {
      return false;
    }
    if (replacing == nullptr && reference->GetKind() == dom::Node::Kind::DocumentType) {
      return true;
    }
    return sibling_of_kind(dom::Node::Kind::DocumentType, true);
  };
  switch (node.GetKind()) {
    case dom::Node::Kind::DocumentFragment: {
      std::size_t elements = 0;
      for (const std::unique_ptr<dom::Node>& child : node.Children()) {
        if (child->IsElement()) {
          ++elements;
        } else if (child->IsText()) {
          return "HierarchyRequestError";
        }
      }
      if (elements > 1) {
        return "HierarchyRequestError";
      }
      if (elements == 1 && (has_child(dom::Node::Kind::Element, nullptr) ||
                            doctype_is_in_the_way())) {
        return "HierarchyRequestError";
      }
      return nullptr;
    }
    case dom::Node::Kind::Element:
      if (has_child(dom::Node::Kind::Element, nullptr) || doctype_is_in_the_way()) {
        return "HierarchyRequestError";
      }
      return nullptr;
    case dom::Node::Kind::DocumentType:
      if (has_child(dom::Node::Kind::DocumentType, nullptr)) {
        return "HierarchyRequestError";
      }
      if (reference == nullptr ? has_child(dom::Node::Kind::Element, nullptr)
                               : sibling_of_kind(dom::Node::Kind::Element, false)) {
        return "HierarchyRequestError";
      }
      return nullptr;
    case dom::Node::Kind::Text:
    case dom::Node::Kind::Comment:
    case dom::Node::Kind::ProcessingInstruction:
    case dom::Node::Kind::Document:
      return nullptr;
  }
  return nullptr;
}

// The children of `parent`, as raw pointers, for the moment before they are
// removed. A mutation record's `removedNodes` is exactly this, taken while the
// answer is still true.
inline std::vector<dom::Node*> ChildrenOf(const dom::Node& parent) {
  std::vector<dom::Node*> children;
  children.reserve(parent.Children().size());
  for (const std::unique_ptr<dom::Node>& child : parent.Children()) {
    children.push_back(child.get());
  }
  return children;
}

// What inserting `node` will actually put into a parent: a DocumentFragment
// contributes its children and everything else contributes itself. The DOM
// calls this "nodes" and every mutation record's `addedNodes` is it, which is
// why it has to be taken *before* the insertion empties the fragment.
inline std::vector<dom::Node*> InsertedNodesOf(dom::Node& node) {
  if (node.IsDocumentFragment()) {
    return ChildrenOf(node);
  }
  return {&node};
}
std::string CharacterDataOf(const dom::Node* node);

// Polymer / Lit binding tokens left in attribute values until effects replace
// them. Kept visible to getAttribute/clone so annotation parsing can see them;
// attributeChangedCallback must not treat them as serial values (TD-0017).
// Template contents must stay un-upgraded so these tokens survive until
// `_parseTemplate` (see InsertFragmentChildren).
inline bool IsTemplateBindingToken(std::string_view value) {
  if (value.size() < 4) {
    return false;
  }
  if (value[0] == '[' && value[1] == '[' && value[value.size() - 2] == ']' &&
      value[value.size() - 1] == ']') {
    return true;
  }
  if (value[0] == '{' && value[1] == '{' && value[value.size() - 2] == '}' &&
      value[value.size() - 1] == '}') {
    return true;
  }
  return false;
}

}  // namespace microbrowser::bindings

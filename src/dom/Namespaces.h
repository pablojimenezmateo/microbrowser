#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace microbrowser::dom {

// The XML namespace an element or an attribute is in, held as a small handle
// rather than as a string.
//
// Every element and every attribute has one, and a hundred thousand of them on
// a page would be a hundred thousand copies of the same seven URIs. The handle
// is two bytes and fits in padding an element already has; the comparison
// `getElementsByTagNameNS` does per element is an integer compare rather than a
// string one.
//
// The six URIs a real page uses are compile-time constants and cost no storage
// at all. Anything else -- a page calling `createElementNS('http://FOO', 'a')'
// -- is interned into a process-wide table, and **the entries are reference
// counted**, which is the whole reason this is a class rather than an enum: an
// append-only table is unbounded growth driven by page script, and
// `for (i = 0; i < 1e7; i++) document.createElementNS('u' + i, 'a')` would
// leak it forever with nothing left alive to point at it.
//
// Main thread only, like the DOM it describes. A worker never reaches one
// (ADR 0022 -- worker work runs on this thread), so the table needs no lock.
class NamespaceRef {
 public:
  // The fixed URIs. Ids below kFirstInterned are constants and never counted.
  enum Known : std::uint32_t {
    kNone = 0,  // no namespace -- the DOM's null, and what most attributes are
    kHtml = 1,
    kSvg = 2,
    kMathMl = 3,
    kXLink = 4,
    kXml = 5,
    kXmlns = 6,
    kFirstInterned = 7,
  };

  constexpr NamespaceRef() = default;
  // A fixed namespace. Deliberately implicit: `NamespaceRef::kHtml` reads as a
  // value of this type at every call site, which is what it is.
  constexpr NamespaceRef(Known known) : id_(known) {}  // NOLINT(runtime/explicit)

  // An arbitrary URI. The empty string is `kNone`, which is step 1 of every
  // namespace-taking DOM method: an empty namespace *is* no namespace.
  explicit NamespaceRef(std::string_view uri);

  NamespaceRef(const NamespaceRef& other);
  NamespaceRef(NamespaceRef&& other) noexcept : id_(other.id_) { other.id_ = kNone; }
  NamespaceRef& operator=(const NamespaceRef& other);
  NamespaceRef& operator=(NamespaceRef&& other) noexcept;
  ~NamespaceRef();

  bool IsNone() const { return id_ == kNone; }
  bool IsHtml() const { return id_ == kHtml; }
  std::uint32_t Id() const { return id_; }

  // The URI, or the empty string for `kNone`. A view into storage that lives as
  // long as this reference does.
  std::string_view Uri() const;

  friend bool operator==(const NamespaceRef& a, const NamespaceRef& b) {
    return a.id_ == b.id_;
  }

 private:
  // Drops this reference and becomes `kNone`. The destructor and the two
  // assignments are the same operation followed by different work.
  void Release();

  // Four bytes rather than two so that exhaustion is not a case: a table that
  // could run out would need a wrong answer to give when it did, and "this
  // namespace is now null" is the one answer that must never be invented.
  std::uint32_t id_ = kNone;
};

// Whether (namespace, local name) names a script element: one whose `src` the
// loader fetches and whose text the interpreter runs.
//
// **Not a `tagName` comparison**, because the two disagree the moment a
// document is not HTML: an SVG document writes its external scripts as
// `<h:script src="…"/>` with `h` bound to the XHTML namespace, so the qualified
// name is `h:script` and `tagName == "script"` misses every one of them. Both
// namespaces answer true -- SVG has a `script` element of its own (SVG 2 §5.7)
// and it executes for the same reason HTML's does.
//
// Here rather than at its four callers for the reason `CanHostShadowRoot` is in
// `dom`: the loader, the tree mutation path and two binding collections all ask,
// and four copies of the question are four chances to answer it differently.
bool IsScriptElement(const NamespaceRef& name_space, std::string_view local_name);

// How many URIs are interned right now. For the test that says the table is
// reference counted rather than append-only; nothing else should care.
std::size_t InternedNamespaceCount();

}  // namespace microbrowser::dom

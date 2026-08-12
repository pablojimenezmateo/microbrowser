#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "js/Interpreter.h"

// Which window is which, around one document. ADR 0027 §1, ADR 0042 §5.
//
// **A public header of this module, unlike FrameBindings.h next to it**, and the
// split between the two is the seam. `FrameBindings.h` installs accessors and
// needs `DomBindings`, so it stays private. What is here is the *state those
// accessors read* -- which `<iframe>` holds which child context, and which
// context holds this one -- and only `src/engine` can fill it in, because
// deciding same-origin needs `src/url` and this module's own code paths may not
// reach it (ADR 0008, ADR 0027 §2).
//
// So the shape is the same inversion `Geometry.h` and `Network.h` already use:
// the binding layer declares what it needs to know, and the engine answers.

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::bindings {

// The global object of each browsing context around this one.
//
// **A table on the side rather than a field on the element**, because neither
// module that would otherwise hold it can. `dom::Element` already carries the
// child's *document* (`SetNestedDocument`) and cannot carry its global --
// `src/dom` may not see `src/js`. And `DomBindings` is at 989 of its 990
// permitted lines, which is the architecture lint saying the next thing added
// to that class should be a separate class.
//
// A cross-origin child is simply never entered here, so there is nothing for an
// accessor to guard: absence *is* the check, which is the same structural form
// ADR 0027 §2 gave `Element::SetNestedDocument`.
//
// **Every pointer here is a realm's global**, and a realm is never destroyed
// while its interpreter lives -- `Realm::Roots()` keeps the global reachable for
// the collector, and the realm list only ever grows. So these are raw pointers
// that cannot dangle, for the same reason a `RealmId` cannot.
class FrameGlobals {
 public:
  // The global of the context nested in `element`. Null when that frame has no
  // document, has run no script, or is cross-origin -- three states a page must
  // not be able to tell apart, because "is there a document there" is itself
  // information about another origin.
  js::Object* Nested(const dom::Element* element) const;
  void SetNested(const dom::Element* element, js::Object* global);

  // In tree order, which is what `window[0]` and `window.length` are indexed by.
  // Only frames that have a global appear, so a page with one scripted and one
  // cross-origin child sees `length === 1`. That is a deviation from HTML, which
  // counts every child context, and it is the one worth taking here: the
  // alternative is a hole in the list that answers `undefined` and still tells
  // the page how many frames it was not allowed to see.
  std::size_t Count() const { return nested_.size(); }
  js::Object* At(std::size_t index) const;

  // The context this document is nested in, and the root of its tree. Both null
  // for a top-level document, which is what makes `parent === window` and
  // `top === window` the default rather than a case.
  js::Object* Embedder() const { return embedder_; }
  js::Object* Top() const { return top_; }
  void SetEmbedder(js::Object* embedder, js::Object* top) {
    embedder_ = embedder;
    top_ = top;
  }

  // Dropped with the document whose frames these were. The keys are elements in
  // that document, so keeping them across a navigation is a use-after-free
  // waiting for the next page to allocate an element at the same address -- the
  // same rule `Page::Load` follows for every per-element map it holds.
  void Clear();

 private:
  std::vector<std::pair<const dom::Element*, js::Object*>> nested_;
  js::Object* embedder_ = nullptr;
  js::Object* top_ = nullptr;
};

// `contentWindow` as the child's actual global, and `contentDocument` as that
// window's own `document` -- so the two are the *same object*, which is what a
// page checks and what the plain-object stub could not offer.
//
// Installed after `DomBindings::Install`, from `engine::PageScript`, and
// reaching the `HTMLIFrameElement` prototype through the interpreter rather than
// through `DomBindings`. That is not a shortcut: it is what keeps this out of a
// class the lint has already declared full, and it works because the interface
// table is published on the global under a name no page can type.
void InstallFrameWindows(js::Interpreter& interpreter, const FrameGlobals& globals);

// `parent`, `top`, `window[i]` and `window.length`, rewritten from `globals`.
//
// Separate from the installer, and called again whenever the tree moves, because
// a child realm exists only *after* its document arrives -- so at install time
// the answer to `parent` is not yet known, and by first paint it may have
// changed. Plain properties rather than accessors, because a page reads
// `window.top` on paths it takes per frame and one load beats one call.
void PublishFrameWindows(js::Interpreter& interpreter, const FrameGlobals& globals);

}  // namespace microbrowser::bindings

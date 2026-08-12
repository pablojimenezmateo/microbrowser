#pragma once

// `document.body` and `document.title`: the two document accessors that are
// algorithms rather than lookups.
//
// A type of its own for the reason `Reflector` is one -- `DomBindings.h` is at
// its line cap, and that cap is the module asking whether the class has taken
// on another job. It holds nothing but a reference to the binding layer, so it
// is constructed at the point of use and the accessors it installs capture the
// `DomBindings*`.
//
// Both of these *write*, and both write somewhere other than where they read:
// `document.title = x` creates a `<title>` inside `<head>` when there is not
// one, and `document.body = e` replaces an element that may be a `<frameset>`
// and throws when the new value is neither. As one-line accessors beside
// `documentElement` -- which is what they were -- the getters answered with the
// first matching tag anywhere in the tree and the setters did not exist, so
// `document.title = 'x'` succeeded, read back the old title, and left the tab
// named whatever the markup said.

#include "js/Interpreter.h"

namespace microbrowser::bindings {

class DomBindings;

class DocumentTree {
 public:
  explicit DocumentTree(DomBindings& bindings) : bindings_(bindings) {}

  // `body` and `title` onto `Document.prototype`.
  void Install(const js::Value& target);

 private:
  DomBindings& bindings_;
};

}  // namespace microbrowser::bindings

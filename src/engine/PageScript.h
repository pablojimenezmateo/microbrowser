#pragma once

#include <memory>
#include <string>
#include <vector>

#include "bindings/DomBindings.h"
#include "dom/Node.h"
#include "js/Interpreter.h"

namespace microbrowser::engine {

// The script half of a page: one interpreter, one binding layer, one document.
//
// Its own type rather than two more members on Page, because Page was already
// the class most at risk of becoming "the browser" and the fan-out lint said
// so the moment script arrived. Page coordinates; this owns the coordination
// of one thing.
//
// Rebuilt per load. A fresh global scope per document is the same rule the
// fresh style resolver follows and for a stronger reason: leaving the previous
// page's globals in place would let one document's script see another's state,
// which is a same-origin violation rather than a stale stylesheet.
class PageScript {
 public:
  // Runs the document's inline scripts, in document order.
  //
  // After parsing rather than during it, which is a real difference from the
  // specification: a script that runs while the parser is still working sees a
  // half-built tree, and `document.write` depends on exactly that. Nothing
  // here has `document.write`, and running after is the version that is easy
  // to be sure about -- so it is the one that ships, and this is where the
  // difference is written down.
  //
  // `<script src>` is skipped. An external script means a fetch, and a fetch
  // means a privacy verdict and a same-origin decision; that is its own
  // commit, not a line slipped into this one.
  void Run(dom::Document& document);

  // Anything the page wrote with `console.log`, in order. Collected rather
  // than printed: a page must not be able to write to the terminal the browser
  // was started from.
  const std::vector<std::string>& ConsoleOutput() const;

 private:
  std::unique_ptr<js::Interpreter> interpreter_;
  std::unique_ptr<bindings::DomBindings> bindings_;
};

}  // namespace microbrowser::engine

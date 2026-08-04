#pragma once

#include <memory>
#include <optional>
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
  // Finds the document's scripts and records them in document order, inline
  // text filled in and external ones left as a URL for the caller to fetch.
  //
  // Two steps rather than one because ordering is the whole problem: a page's
  // scripts must run in the order they appear whether each is inline or
  // external, so nothing can run until every external one has arrived. The
  // same shape the stylesheets already use.
  void Collect(dom::Document& document);
  // The external scripts, in the order they were found. The caller fetches
  // them, because what a URL turns into is the loader's problem -- and because
  // a fetch needs a privacy verdict, which this layer has no business
  // producing.
  const std::vector<std::string>& PendingUrls() const { return pending_urls_; }
  // Supplies the source for `PendingUrls()[index]`.
  void AddFetched(std::size_t index, std::string source);
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
  // Runs everything Collect found, in document order. Idempotent: calling it
  // twice runs nothing the second time, so a caller that fetches subresources
  // and a caller that does not can both end with it.
  void Run(dom::Document& document, const std::string& url);

  // Anything the page wrote with `console.log`, in order. Collected rather
  // than printed: a page must not be able to write to the terminal the browser
  // was started from.
  // Runs the click handlers registered on `target` and its ancestors. True
  // when one called `preventDefault`.
  bool DispatchClick(dom::Element& target);

  const std::vector<std::string>& ConsoleOutput() const;

 private:
  std::unique_ptr<js::Interpreter> interpreter_;
  std::unique_ptr<bindings::DomBindings> bindings_;
  // One slot per script in document order. Empty until an external one is
  // fetched, which is what makes a script that fails to load a script that is
  // skipped rather than one that shifts every later script's turn.
  std::vector<std::optional<std::string>> slots_;
  std::vector<std::string> pending_urls_;
  std::vector<std::size_t> pending_slots_;
  bool ran_ = false;
};

}  // namespace microbrowser::engine

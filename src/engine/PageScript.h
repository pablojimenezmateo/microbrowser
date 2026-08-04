#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bindings/DomBindings.h"
#include "bindings/Timers.h"
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
  // When a script runs, relative to the rest of the load.
  //
  // ADR 0011 decided these are three points in a document's lifecycle rather
  // than three attributes to ignore, and this enum is where that decision
  // lives. What is *not* implemented is running a blocking script during the
  // parse: everything here still runs after it, which is the deviation
  // recorded on `Run` below.
  enum class Timing : std::uint8_t {
    // Classic, no `defer` and no `async`. Document order, first.
    Blocking,
    // `defer`, and a module -- which is deferred by definition. Document order,
    // after every blocking script.
    Deferred,
    // `async`. Whenever it arrives, and its order is nobody's promise. It does
    // not hold the page up: a page whose analytics tag is slow paints without
    // waiting for it, which is the entire reason the attribute exists.
    Async,
  };

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
  // Whether `PendingUrls()[index]` is one the page said it would not wait for.
  // The engine asks so it knows which outstanding scripts hold the first paint
  // and which do not.
  bool IsAsync(std::size_t index) const;
  // Supplies the source for `PendingUrls()[index]`.
  void AddFetched(std::size_t index, std::string source);

  // Runs the document's scripts: every blocking one in document order, then
  // every deferred one, then whatever `async` scripts have arrived so far.
  //
  // After parsing rather than during it, which is a real difference from the
  // specification: a script that runs while the parser is still working sees a
  // half-built tree, and `document.write` depends on exactly that. Nothing
  // here has `document.write` -- ADR 0011 decided it stays unimplemented,
  // because supporting it properly means re-entering the tokenizer mid-parse
  // for a feature the web is actively removing -- and running after is the
  // version that is easy to be sure about.
  //
  // Idempotent: calling it twice runs nothing the second time, so a caller
  // that fetches subresources and a caller that does not can both end with it.
  //
  // `now_ms` is the epoch a timer's delay is measured from. Passed in for the
  // reason the loader takes a time: two decisions inside one turn must not
  // disagree about what time it is.
  void Run(dom::Document& document, const std::string& url, std::int64_t now_ms);

  // Runs any `async` script whose source arrived after `Run`. True when one
  // did, which is the caller's signal that the document may have changed.
  // Nothing before `Run`: an async script is still a script on this document,
  // and running one before the page had an interpreter would be a second way
  // to build one.
  bool RunReadyAsync();

  // Milliseconds until the soonest timer, or nothing when none is pending --
  // which is the answer that lets the loop block rather than wake.
  std::optional<std::uint32_t> NextTimerDelay(std::int64_t now_ms) const;
  // Runs every timer due now. True when any ran, which is the caller's signal
  // that the document may have changed.
  bool RunDueTimers(std::int64_t now_ms);

  // Runs the click handlers registered on `target` and its ancestors. True
  // when one called `preventDefault`.
  bool DispatchClick(dom::Element& target);
  // Whether this page ran any script at all. A page that did cannot be assumed
  // not to have changed the tree from a handler, and a page that did not
  // cannot have handlers to run -- which is what keeps a click on a static
  // document from costing a relayout.
  bool HasListeners() const { return interpreter_ != nullptr; }

  // Anything the page wrote with `console.log`, in order. Collected rather
  // than printed: a page must not be able to write to the terminal the browser
  // was started from.
  const std::vector<std::string>& ConsoleOutput() const;
  // Every script that ended on a throw, in the order they ran, each named by
  // the script it came from.
  //
  // Kept rather than dropped because `Run` deliberately continues past a
  // throw -- which is what a browser does, and which without this makes a page
  // that fails nine scripts in a row indistinguishable from one that ran none.
  // A blank render then has no signal at all behind it, and finding out why
  // means adding this line by hand. It is the same reasoning as ConsoleOutput:
  // collected, never printed, because a page must not be able to write to the
  // terminal the browser was started from.
  const std::vector<std::string>& ScriptErrors() const { return errors_; }

 private:
  // One script in document order.
  //
  // A struct rather than three parallel vectors, which is what the timings
  // would otherwise have made this: the source, when it runs, and whether it
  // is a module are three facts about one thing.
  struct Slot {
    // Empty until an external script is fetched, and emptied again once it has
    // run. Those two states want the same treatment -- do not run it -- which
    // is why one `optional` says both.
    std::optional<std::string> source;
    Timing timing = Timing::Blocking;
    // A module is linked and evaluated rather than run: different scoping, a
    // different top level, and `import` means something. It cannot reach the
    // network yet -- the host half of the module loader is what ADR 0011
    // unblocks rather than what it builds -- so an `import` fails with the
    // engine saying there is no resolver, which is a legible answer and not a
    // parse error.
    bool module = false;
  };

  // Builds the interpreter and the binding layer, once. Kept apart from `Run`
  // because an `async` script that lands after the main pass still needs them.
  void EnsureInterpreter(dom::Document& document, const std::string& url,
                         std::int64_t now_ms);
  // Runs every arrived script with this timing, in document order, and empties
  // its slot. True when any ran.
  bool RunTiming(Timing timing);
  // How `ScriptErrors()` names the script in slot `slot`: its URL when it came
  // from one, its position when it was inline.
  std::string SourceName(std::size_t slot) const;

  std::unique_ptr<js::Interpreter> interpreter_;
  std::unique_ptr<bindings::DomBindings> bindings_;
  std::vector<Slot> slots_;
  std::vector<std::string> pending_urls_;
  std::vector<std::size_t> pending_slots_;
  bool ran_ = false;
  bindings::TimerQueue timers_;
  std::vector<std::string> errors_;
};

}  // namespace microbrowser::engine

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "js/Interpreter.h"
#include "url/Url.h"

namespace microbrowser::engine {

// The host half of the module loader: what a specifier means, and where its
// source comes from.
//
// ADR 0011 left this as the one design question it did not answer, and the answer
// is a **split**, because a static import and a dynamic one are different
// problems:
//
//   * **A static graph is fetched before anything in it runs.** The engine's
//     resolver is synchronous -- `Interpreter::SetModuleResolver` -- and going to
//     the network inside it would block the one loop this browser has. So the
//     host parses each module for the specifiers it names (`ModuleImportSpecifiers`),
//     fetches those, and repeats until the graph is closed. Only then does
//     evaluation start, and by then the resolver only has to look a URL up in a
//     table.
//   * **A dynamic `import()` answers later.** A page reaches one at a moment
//     nobody could predict, so the promise is handed back pending and settled on
//     a later turn -- `Interpreter::SetDynamicImportStarter`. This is what
//     reddit's bundle needs: its entry module is a `data:` URL that
//     `import()`s three more from a CDN.
//
// This class is the table and the URL arithmetic. *Fetching* is the engine's,
// because a fetch needs a privacy verdict and a connection pool; this holds what
// came back and answers the resolver from it.
class ModuleLoader {
 public:
  // Forgets every source. A module graph belongs to the document that asked for
  // it: a source kept across a navigation would let one document's code be
  // evaluated in another's scope, which is the same rule PageScript's fresh
  // global follows and for the same reason.
  void Clear();
  // What a bare or relative specifier resolves against when the referrer is not
  // itself a usable base. **Separate from Clear on purpose**: a module script's
  // source arrives before the interpreter exists, so the graph is populated
  // before the resolver is installed -- and an install that cleared would throw
  // away exactly the sources it was about to be asked for.
  void SetDocumentUrl(std::string_view document_url);

  // What `specifier` means, written in `referrer`. Nothing when it does not
  // resolve at all -- which the engine turns into the TypeError the language
  // throws at the import.
  //
  // `data:` resolves to itself: it *is* its own source, which is why the loader
  // can answer for one without a fetch. reddit's entry point is one.
  std::optional<std::string> Resolve(std::string_view specifier,
                                     std::string_view referrer) const;

  // Records a module's source under its resolved URL.
  void Add(std::string url, std::string source);
  // Whether this URL's source is already here.
  bool Has(std::string_view url) const;
  // The source, or nothing. What the synchronous resolver answers from.
  const std::string* Source(std::string_view url) const;

  // Every URL in `url`'s static graph that is not here yet, resolved. Empty means
  // the graph is closed and evaluation may start.
  //
  // Walks what is present rather than the whole graph, so calling it after each
  // arrival converges: each round names one more layer.
  std::vector<std::string> MissingFrom(std::string_view url) const;

  // A `data:` module, decoded and recorded without a fetch. True when `url` was
  // one -- which is also the answer to "does this need the network at all".
  bool AddDataUrl(std::string_view url);

  std::size_t Size() const { return sources_.size(); }

 private:
  // Where a bare specifier and a relative one resolve against when the referrer
  // is not itself a URL -- a `data:` module's imports resolve against the
  // *document*, because a `data:` URL is not a base anything can be relative to.
  std::optional<url::Url> document_;
  std::map<std::string, std::string, std::less<>> sources_;
};

}  // namespace microbrowser::engine

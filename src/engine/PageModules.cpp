// The module graph, from the page's side.
//
// Ledger session 50's second half, and ADR 0011's one unanswered design question:
// `Interpreter::SetModuleResolver` is synchronous, so what happens when a
// specifier names something that has to be fetched?
//
// The answer is a split, and the whole file is that split:
//
//   * **A static graph is closed before evaluation.** The resolver cannot go to
//     the network -- it would block the one loop this browser has, which ADR 0011
//     exists to prevent -- so each module is parsed for the specifiers it names,
//     those are fetched, and the round repeats until nothing is missing. Only
//     then is anything evaluated, and by then the resolver is a table lookup.
//   * **A dynamic `import()` is answered later.** A page reaches one at a moment
//     nobody could predict, so the promise is handed back *pending* and settled
//     on a later turn of the loop.
//
// Fetching is deliberately not here: a fetch needs a privacy verdict and a
// connection pool, and both live in the engine. This decides *what* to fetch and
// what to do when it arrives, which is the half that needs to know what a module
// graph is.

#include <algorithm>
#include <string>
#include <utility>

#include "engine/PageScript.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

void PageScript::InstallModuleHost(const std::string& document_url) {
  // The document URL and *not* a clear: a module script's source reaches the
  // graph through AddFetched, which happens before the interpreter exists, and an
  // install that cleared would throw away the sources it is about to be asked
  // for. Clearing belongs to Detach, which is where a document ends.
  modules_.SetDocumentUrl(document_url);
  if (interpreter_ == nullptr) {
    return;
  }

  // The synchronous half. It answers only from what has already been fetched,
  // which is the contract that lets it be synchronous at all: a miss is a
  // TypeError at the import rather than a stall.
  interpreter_->SetModuleResolver([this](std::string_view specifier, std::string_view referrer,
                                        std::string& resolved, std::string& source) {
    const std::optional<std::string> url = modules_.Resolve(specifier, referrer);
    if (!url.has_value()) {
      return false;
    }
    // A `data:` module is its own source, so it can be answered without ever
    // having been fetched. reddit's entry point is one.
    modules_.AddDataUrl(*url);
    const std::string* text = modules_.Source(*url);
    if (text == nullptr) {
      return false;
    }
    resolved = *url;
    source = *text;
    return true;
  });

  // The asynchronous half.
  interpreter_->SetDynamicImportStarter(
      [this](std::string_view specifier, std::string_view referrer, js::Object* promise) {
        const std::optional<std::string> url = modules_.Resolve(specifier, referrer);
        if (!url.has_value() || promise == nullptr) {
          AddPerformanceCounter(PerfCounterId::JsDynamicImportsRefused);
          return false;
        }
        AddPerformanceCounter(PerfCounterId::JsDynamicImports);
        modules_.AddDataUrl(*url);
        pending_imports_.push_back(PendingImport{*url, std::string(referrer), promise});
        // The specifier is stored *resolved*, so settling later does not depend on
        // the referrer still meaning what it did.
        if (!modules_.Has(*url)) {
          Want(*url);
        }
        RefreshModuleFetches();
        return true;
      });
}

void PageScript::SetModuleDocumentUrl(const std::string& url) {
  modules_.SetDocumentUrl(url);
}

void PageScript::Want(const std::string& url) {
  if (!requested_modules_.insert(url).second) {
    return;
  }
  module_fetches_.push_back(url);
}

void PageScript::RefreshModuleFetches() {
  // Recomputed from the whole graph rather than from the module that just
  // arrived: a graph comes in a layer at a time and each arrival can name more,
  // so asking what is missing after every arrival is what makes this converge
  // without tracking a frontier.
  for (const std::string& url : modules_.MissingFrom({})) {
    Want(url);
  }
}

std::vector<std::string> PageScript::TakeModuleFetches() {
  std::vector<std::string> taken;
  taken.swap(module_fetches_);
  AddPerformanceCounter(PerfCounterId::JsModuleFetches, taken.size());
  return taken;
}

void PageScript::AddModuleSource(std::string url, std::string source) {
  modules_.Add(std::move(url), std::move(source));
  RefreshModuleFetches();
}

bool PageScript::HasPendingModules() const {
  return !pending_imports_.empty() || !module_fetches_.empty();
}

bool PageScript::AdvanceModules() {
  if (interpreter_ == nullptr || pending_imports_.empty()) {
    return false;
  }
  RefreshModuleFetches();
  if (!module_fetches_.empty() || !requested_modules_.empty()) {
    // Something is still outstanding. A graph that is not closed must not start
    // evaluating: the resolver would miss and the import would fail with a
    // TypeError naming a module that is simply still in flight.
    const bool waiting =
        std::any_of(requested_modules_.begin(), requested_modules_.end(),
                    [this](const std::string& url) { return !modules_.Has(url); });
    if (waiting) {
      return false;
    }
  }

  // Taken before any of them settles: settling runs a page's `then`, which can
  // call `import()` again, and a walk over a list that is being appended to is a
  // walk that does not end.
  std::vector<PendingImport> due;
  due.swap(pending_imports_);
  bool ran = false;
  for (const PendingImport& import : due) {
    interpreter_->SettleDynamicImport(import.promise, import.specifier, import.referrer);
    AddPerformanceCounter(PerfCounterId::JsDynamicImportsSettled);
    ran = true;
  }
  if (ran) {
    interpreter_->DrainMicrotasks();
  }
  return ran;
}

}  // namespace microbrowser::engine

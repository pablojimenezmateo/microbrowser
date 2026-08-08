// The engine half of the module graph: the fetching, and nothing else.
//
// Its own translation unit for the reason EngineFetch.cpp is -- Engine.cpp is at
// its module cap -- and the seam is the same one PageModules.cpp names from the
// other side: a fetch needs a privacy verdict and a connection pool, and deciding
// *what* to fetch needs to know what a module graph is. This is the first half;
// engine/PageModules.cpp is the second.
//
// A module fetch behaves like a late image rather than like a subresource: it
// happens after the navigation that carried the document, because `import()` is
// reached whenever the page reaches it. So it follows the same two rules -- a
// navigation clears it, and one in flight keeps the loop turning.

#include <optional>
#include <string>
#include <utility>

#include "engine/Clock.h"
#include "engine/Engine.h"
#include "url/Url.h"

namespace microbrowser::engine {

bool Engine::AdvanceModules() {
  // Started first, so a graph that needs another layer is already in flight by
  // the time this returns and the loop does not have to come back round for it.
  if (load_.base.has_value() || !page_.Url().empty()) {
    const std::optional<url::Url> base = page_.BaseUrl();
    for (const std::string& url : page_.TakeModuleFetches()) {
      if (!base.has_value()) {
        continue;
      }
      const Loader::RequestId id = loader_.StartSubresource(
          url, *base, privacy::ResourceType::Script, NowSeconds(), {});
      module_fetches_[id] = url;
      if (load_.active) {
        // While a navigation is in flight, a missing module holds the scripts
        // back: evaluating a module whose import has not arrived would ask the
        // resolver for something it cannot fetch.
        ++load_.modules_outstanding;
      }
    }
  }
  return page_.AdvanceModules();
}

bool Engine::OnModuleFetch(Loader::Completion completion) {
  const auto found = module_fetches_.find(completion.id);
  if (found == module_fetches_.end()) {
    return false;
  }
  const std::string url = found->second;
  module_fetches_.erase(found);
  if (load_.active && load_.modules_outstanding > 0) {
    --load_.modules_outstanding;
  }
  // Recorded even when it failed: an empty source is a module that fails to
  // parse, and a failure reported once at evaluation beats a graph that never
  // closes and a promise nobody settles.
  page_.AddModuleSource(url, completion.result.ok ? std::move(completion.result.body)
                                                 : std::string());
  // A closed graph lets a deferred `<script type=module>` or a pending dynamic
  // `import()` run; without this, post-load module arrivals only settle imports
  // and never reach `RunPendingScripts` (Gate B concat chain).
  if (load_.scripts_ran || post_load_.document_interactive) {
    if (ProcessDynamicScripts()) {
      page_.InvalidateLayout();
      LayoutAndPaint();
    }
  }
  return true;
}

}  // namespace microbrowser::engine

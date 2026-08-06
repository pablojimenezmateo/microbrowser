#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/Loader.h"
#include "url/Url.h"

namespace microbrowser::engine {

// What one outstanding request is for. A completion arrives carrying only an id
// and some bytes, so somebody has to have written down what was asked.
enum class ResourceKind {
  StyleSheet,
  Script,
  Image,
};

struct PendingResource {
  ResourceKind kind = ResourceKind::StyleSheet;
  // The slot this fills: the index into the page's pending list for a sheet or
  // a script. Slots rather than appends, because author-origin cascade order
  // and script order are document order, and arrival order is not.
  std::size_t index = 0;
  // The `src` exactly as the document wrote it, for an image.
  std::string src;
};

// A navigation from the moment it is asked for to the moment it is on screen.
//
// This is the state ADR 0011 said the engine would gain, and it is a value of
// its own rather than eight fields on Engine for the reason Engine's budget
// exists: Engine routes, and the things it routes to own themselves. Every
// field here is present because **arrival order is not document order** -- the
// counters say what is still owed and the map says which slot each answer
// fills.
//
// Cleared as one value by the next navigation, which is what makes a stale
// response undeliverable rather than merely ignored.
struct PendingLoad {
  bool active = false;
  std::string url;
  // Steady milliseconds when this navigation started, so a `PerformanceObserver`
  // watching `navigation` and `resource` can be told a duration rather than a
  // timestamp. Zero on a load that never started, which is not a valid time and
  // is why the entries are only produced when the document arrived.
  std::int64_t started_ms = 0;
  // The same moment on the wall clock, and held separately rather than derived
  // because the two clocks have unrelated epochs. Only `performance.timing`
  // reads it: every field of that interface is a Unix timestamp, and the offsets
  // above are what everything else is measured in.
  std::int64_t started_wall_ms = 0;
  // When the document's scripts finished, which is what a page reads as
  // `domContentLoadedEventStart`. reddit's own perf module reports its metric
  // only when that number is non-zero, so an entry that answered zero would be
  // one that silently does nothing.
  std::int64_t dom_content_loaded_ms = 0;
  bool bypass_cache = false;
  Loader::RequestId document = 0;
  bool document_arrived = false;
  // The base every subresource href is resolved against: where the document
  // actually came from, after redirects.
  std::optional<url::Url> base;
  std::map<Loader::RequestId, PendingResource> resources;
  std::size_t sheets_outstanding = 0;
  std::size_t scripts_outstanding = 0;
  // Modules the graph is still missing. Held apart from `scripts_outstanding`
  // because a module script's *source* arriving is not the same event as its
  // imports having arrived, and only the second lets evaluation start.
  std::size_t modules_outstanding = 0;
  // Counted apart from the rest, because the whole meaning of `async` is that
  // the page does not wait for it. One that is still in flight when everything
  // else has landed does not hold the first paint; it runs when it arrives and
  // the page is laid out again.
  std::size_t async_scripts_outstanding = 0;
  std::size_t images_outstanding = 0;
  std::size_t total_resources = 0;
  std::size_t finished_resources = 0;
  bool scripts_ran = false;
  // The first frame has gone out. The load stays alive past it only for the
  // `async` scripts that have not landed yet.
  bool painted = false;
  // Image bytes, held until the scripts have run. Decoding earlier would ask
  // the document how large it wants an image drawn before the script that sets
  // that has run, and the answer would then depend on which arrived first --
  // which is precisely the nondeterminism this whole design has to not have.
  std::vector<std::pair<std::string, std::string>> image_bytes;

  // Nothing render-blocking is still owed. Stylesheets and scripts both are:
  // a script may ask about a style, so it must not run before the sheets that
  // set it have landed.
  // `modules_outstanding` is the module *graph*, not the module scripts: a
  // `<script type=module>` whose source has arrived may still name an import
  // nobody has fetched, and evaluating it then would ask a resolver that cannot
  // go to the network. See engine/PageModules.cpp.
  bool MayRunScripts() const {
    return active && document_arrived && !scripts_ran && sheets_outstanding == 0 &&
           scripts_outstanding == 0 && modules_outstanding == 0;
  }

  // Everything that holds the first frame back has resolved.
  bool MayPaint() const { return active && !painted && scripts_ran && images_outstanding == 0; }

  // And nothing at all is left, including the scripts the page said it would
  // not wait for.
  bool IsFinished() const { return painted && async_scripts_outstanding == 0; }

  // How far along, for the progress the UI shows. Never reaches 1.0: that is
  // reserved for the frame actually going out, and a progress bar that
  // completes before the page appears is worse than one that lags.
  float Progress() const {
    if (total_resources == 0) {
      return 0.0f;
    }
    const float fraction =
        static_cast<float>(finished_resources) / static_cast<float>(total_resources);
    return fraction < 0.99f ? fraction : 0.99f;
  }
};

}  // namespace microbrowser::engine

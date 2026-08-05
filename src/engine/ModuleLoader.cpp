#include "engine/ModuleLoader.h"

#include <utility>

#include "engine/Loader.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How many modules one document's graph may hold. A page controls both the count
// and the fan-out of its own imports, so the graph is bounded for the reason the
// history list and the violation log are.
constexpr std::size_t kMaxModules = 256;

bool IsDataUrl(std::string_view url) {
  return util::StartsWithAsciiCaseInsensitive(url, "data:");
}

}  // namespace

void ModuleLoader::Clear() { sources_.clear(); }

void ModuleLoader::SetDocumentUrl(std::string_view document_url) {
  document_ = url::Url::Parse(document_url);
}

std::optional<std::string> ModuleLoader::Resolve(std::string_view specifier,
                                                std::string_view referrer) const {
  if (specifier.empty()) {
    return std::nullopt;
  }
  if (IsDataUrl(specifier)) {
    // A `data:` module is its own source, so it resolves to itself. Nothing else
    // could be a base for it either -- there is no directory to be relative to.
    return std::string(specifier);
  }
  // A specifier is a URL, or is *relative* -- `/`, `./` or `../`. Anything else
  // is a bare specifier and names nothing here: there is no import map and no
  // node_modules, and resolving `react` against the document would fetch
  // `/react`, which is not what the page asked for and is a request to a URL it
  // never named.
  const bool relative = specifier.front() == '/' ||
                        util::StartsWithAsciiCaseInsensitive(specifier, "./") ||
                        util::StartsWithAsciiCaseInsensitive(specifier, "../");
  if (!relative) {
    if (std::optional<url::Url> absolute = url::Url::Parse(specifier)) {
      return absolute->Serialize();
    }
    return std::nullopt;
  }
  // The referrer first, because that is what the specification resolves against.
  // A `data:` referrer is not a base -- `new URL("./x", "data:…")` has no
  // meaning -- so those fall through to the document, which is what a browser
  // does with a relative import inside a `data:` module.
  if (!referrer.empty() && !IsDataUrl(referrer)) {
    if (const std::optional<url::Url> base = url::Url::Parse(referrer)) {
      if (std::optional<url::Url> resolved = url::Url::Parse(specifier, *base)) {
        return resolved->Serialize();
      }
    }
  }
  if (document_.has_value()) {
    if (std::optional<url::Url> resolved = url::Url::Parse(specifier, *document_)) {
      return resolved->Serialize();
    }
  }
  return std::nullopt;
}

void ModuleLoader::Add(std::string url, std::string source) {
  if (sources_.size() >= kMaxModules && sources_.find(url) == sources_.end()) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::JsModulesLoaded);
  sources_.insert_or_assign(std::move(url), std::move(source));
}

bool ModuleLoader::Has(std::string_view url) const { return sources_.find(url) != sources_.end(); }

const std::string* ModuleLoader::Source(std::string_view url) const {
  const auto found = sources_.find(url);
  return found == sources_.end() ? nullptr : &found->second;
}

std::vector<std::string> ModuleLoader::MissingFrom(std::string_view url) const {
  std::vector<std::string> missing;
  // Every module present, not only `url`: a graph arrives a layer at a time and
  // each arrival can name more. Walking what is here after every arrival is what
  // makes the loop converge without the caller tracking a frontier.
  (void)url;
  for (const auto& [module_url, source] : sources_) {
    for (const std::string& specifier : js::ModuleImportSpecifiers(source)) {
      const std::optional<std::string> resolved = Resolve(specifier, module_url);
      if (!resolved.has_value() || Has(*resolved)) {
        continue;
      }
      // A `data:` import needs no fetch, and reporting it as missing would ask
      // the caller to fetch a URL that is already its own answer.
      if (IsDataUrl(*resolved)) {
        continue;
      }
      bool already = false;
      for (const std::string& queued : missing) {
        already = already || queued == *resolved;
      }
      if (!already) {
        missing.push_back(*resolved);
      }
    }
  }
  return missing;
}

bool ModuleLoader::AddDataUrl(std::string_view url) {
  if (!IsDataUrl(url)) {
    return false;
  }
  if (Has(url)) {
    return true;
  }
  const DataUrl decoded = DecodeDataUrl(url);
  // Recorded even when it did not decode: the empty source is what the module
  // will fail to parse, and a failure reported once at evaluation is better than
  // the loader silently asking for it again on every round.
  Add(std::string(url), decoded.ok ? decoded.body : std::string());
  return true;
}

}  // namespace microbrowser::engine

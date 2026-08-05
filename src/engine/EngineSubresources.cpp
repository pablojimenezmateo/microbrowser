// How a subresource is fetched, and whether the bytes that came back are the
// ones the document said they would be.
//
// Its own translation unit for the reason EngineFetch.cpp is: Engine.cpp is at
// its module cap, and a file over its cap means a missing seam rather than a
// bigger file. The seam here is a real one -- everything in this file is one
// question asked twice, about a stylesheet and about a script, and the whole
// point of it being one function each is that "refuse to apply" and "refuse to
// execute" cannot come to mean two different things.
//
// ADR 0020 §4 is the argument. Subresource Integrity is the only mechanism in
// this browser that protects the user against the site's *own* CDN, which is a
// threat the site chose to defend against and that we would otherwise silently
// discard.

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "csp/SubresourceIntegrity.h"
#include "engine/Clock.h"
#include "engine/Engine.h"
#include "url/Origin.h"
#include "url/Url.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

std::optional<net::FetchOptions> Engine::OptionsForSubresource(
    const SubresourceRequest& request) const {
  net::FetchOptions options;
  options.bypass_cache = load_.bypass_cache;
  const url::Url& document = *load_.base;

  if (!request.cross_origin.has_value()) {
    const std::optional<url::Url> target = url::Url::Parse(request.url, document);
    const bool cross_origin =
        target.has_value() &&
        !url::Origin::FromUrl(*target).IsSameOrigin(url::Origin::FromUrl(document));
    if (cross_origin && csp::HasIntegrityMetadata(request.integrity)) {
      // ADR 0020 §4: `integrity` on a cross-origin resource requires
      // `crossorigin`. Refused rather than fetched and left unchecked, because
      // an integrity check over bytes the page may not read is an oracle for
      // them -- one guess per reload. Plex sets both, which is why it is the
      // site this rule was measured against.
      return std::nullopt;
    }
    return options;
  }
  // `crossorigin` present: this is a CORS request, and `use-credentials` is the
  // only value that sends any. `crossorigin=""` is `anonymous`, which is why the
  // attribute is an optional rather than a string.
  options.cors.mode = net::RequestMode::Cors;
  options.cors.credentials =
      util::EqualsAsciiCaseInsensitive(*request.cross_origin, "use-credentials")
          ? net::CredentialsMode::Include
          : net::CredentialsMode::Omit;
  options.cors.origin = url::Origin::FromUrl(document);
  return options;
}

// Whether the bytes are the ones the document said they would be.
//
// True when the element named no usable hash, which is what an absent
// `integrity` means and also what an unreadable one means: an attribute this
// browser cannot parse must not be stricter than no attribute at all, or a typo
// takes a site offline.
bool Engine::IntegrityHolds(const std::vector<SubresourceRequest>& requests, std::size_t index,
                            std::string_view body) {
  if (index >= requests.size() || requests[index].integrity.empty()) {
    return true;
  }
  AddPerformanceCounter(PerfCounterId::SriChecks);
  switch (csp::CheckIntegrity(requests[index].integrity, body)) {
    case csp::IntegrityResult::Match:
      return true;
    case csp::IntegrityResult::NoMetadata:
      AddPerformanceCounter(PerfCounterId::SriUnparseable);
      return true;
    case csp::IntegrityResult::Mismatch:
      break;
  }
  AddPerformanceCounter(PerfCounterId::SriMismatches);
  return false;
}

void Engine::StartSubresources() {
  const url::Url& document = *load_.base;

  // All at once. The order they are *started* in is document order and stays
  // deterministic; the order they arrive in is the network's business and this
  // engine must not have an opinion about it, which is what the slot-filling
  // below is for.
  const std::vector<SubresourceRequest>& sheets = page_.PendingStyleSheets();
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    const std::optional<net::FetchOptions> options = OptionsForSubresource(sheets[i]);
    if (!options.has_value()) {
      continue;
    }
    const Loader::RequestId id = loader_.StartSubresource(
        sheets[i].url, document, privacy::ResourceType::Stylesheet, NowSeconds(), *options);
    load_.resources[id] = PendingResource{ResourceKind::StyleSheet, i, {}};
    ++load_.sheets_outstanding;
  }

  const std::vector<SubresourceRequest>& scripts = page_.PendingScripts();
  for (std::size_t i = 0; i < scripts.size(); ++i) {
    const std::optional<net::FetchOptions> options = OptionsForSubresource(scripts[i]);
    if (!options.has_value()) {
      continue;
    }
    const Loader::RequestId id = loader_.StartSubresource(
        scripts[i].url, document, privacy::ResourceType::Script, NowSeconds(), *options);
    load_.resources[id] = PendingResource{ResourceKind::Script, i, {}};
    ++(page_.PendingScriptIsAsync(i) ? load_.async_scripts_outstanding
                                     : load_.scripts_outstanding);
  }

  StartImageRequests();

  load_.total_resources = load_.resources.size();
}

}  // namespace microbrowser::engine

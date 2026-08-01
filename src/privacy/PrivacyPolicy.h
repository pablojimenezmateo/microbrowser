#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "privacy/BlockingEngine.h"
#include "privacy/Request.h"
#include "privacy/Verdict.h"

namespace microbrowser::privacy {

// The single gate every request passes before `net` sees it.
//
// One entry point rather than a set of helpers a caller assembles, because the
// order matters and getting it wrong is silent. Blocking runs before the
// upgrade, so a blocked request never causes a DNS lookup for its https form;
// sanitization runs after, so a tracking parameter cannot survive by being
// added during the upgrade; and the referrer is computed last, from the final
// URL rather than from the requested one.
class PrivacyPolicy {
 public:
  struct Settings {
    // HTTPS-only. Downgrading is an explicit per-site act, not a silent
    // fallback — `guidelines/privacy.md`.
    bool https_only = true;
    // Cross-origin referrers are trimmed to their origin, always. Not
    // configurable downward: this is the referrer policy, not a default that a
    // page's `Referrer-Policy` header can loosen.
    bool trim_cross_origin_referrer = true;
    bool block_tracking_parameters = true;
  };

  PrivacyPolicy() = default;
  explicit PrivacyPolicy(Settings settings) : settings_(settings) {}

  BlockingEngine& Engine() { return engine_; }
  const BlockingEngine& Engine() const { return engine_; }

  const Settings& GetSettings() const { return settings_; }

  // Hosts the user has explicitly allowed to be reached over plain HTTP. A set
  // rather than a global switch, because "the user turned off HTTPS-only" and
  // "the user accepted the risk for this one host" are different decisions and
  // only the second one should be easy.
  void AllowInsecureHost(std::string host);
  bool IsInsecureHostAllowed(std::string_view host) const;

  // The decision. `referrer_document` is the URL of the page making the
  // request, which is what the referrer is derived from.
  Verdict Decide(const Request& request, const url::Url* referrer_document = nullptr) const;

 private:
  Settings settings_;
  BlockingEngine engine_;
  std::vector<std::string> insecure_hosts_;
};

// The tracking parameters stripped from every URL, independent of any filter
// list. Exposed so a test can assert the list is non-empty and so the sanitizer
// has exactly one source of truth.
//
// Implemented here rather than as a second thing that rewrites URLs: filter
// lists express these as `$removeparam` rules and both paths converge on the
// same code, which is what stops the two from drifting.
std::vector<std::string_view> DefaultTrackingParameters();

// Removes the named parameters from a URL's query. Returns true when anything
// was removed.
bool StripQueryParameters(url::Url& url, const std::vector<std::string_view>& parameters);

}  // namespace microbrowser::privacy

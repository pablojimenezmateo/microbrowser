#include "privacy/PrivacyPolicy.h"

#include <algorithm>

#include "util/PerformanceCounters.h"

namespace microbrowser::privacy {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Splits a query string into `name=value` pieces, preserving order.
std::vector<std::string_view> SplitQuery(std::string_view query) {
  std::vector<std::string_view> pieces;
  std::size_t start = 0;
  while (start <= query.size()) {
    const std::size_t amp = query.find('&', start);
    const std::string_view piece =
        amp == std::string_view::npos ? query.substr(start) : query.substr(start, amp - start);
    if (!piece.empty()) {
      pieces.push_back(piece);
    }
    if (amp == std::string_view::npos) {
      break;
    }
    start = amp + 1;
  }
  return pieces;
}

std::string_view NameOf(std::string_view parameter) {
  const std::size_t equals = parameter.find('=');
  return equals == std::string_view::npos ? parameter : parameter.substr(0, equals);
}

}  // namespace

bool Request::IsThirdParty() const {
  const url::Site request_site = url::Site::FromUrl(url);
  if (request_site.IsOpaque() || top_level_site.IsOpaque()) {
    // An opaque site is not the same site as anything, including itself, so a
    // request from or to one is third-party. Treating "unknown" as first-party
    // would be the permissive answer, and this is a security decision.
    return true;
  }
  return !(request_site == top_level_site);
}

std::vector<std::string_view> DefaultTrackingParameters() {
  // A maintained list, kept short and specific. Every entry is a parameter that
  // carries identity and never affects what the server returns — stripping one
  // that did would break the page, which is why this is a list rather than a
  // pattern.
  return {
      "utm_source", "utm_medium",  "utm_campaign", "utm_term",   "utm_content",  "utm_id",
      "utm_name",   "utm_cid",     "utm_reader",   "fbclid",     "gclid",        "dclid",
      "gbraid",     "wbraid",      "msclkid",      "twclid",     "igshid",       "mc_eid",
      "mc_cid",     "yclid",       "_openstat",    "vero_id",    "vero_conv",    "oly_anon_id",
      "oly_enc_id", "hsCtaTracking", "__s",        "wickedid",   "s_cid",        "ml_subscriber",
      "ml_subscriber_hash",
  };
}

bool StripQueryParameters(url::Url& url, const std::vector<std::string_view>& parameters) {
  if (!url.HasQuery() || parameters.empty()) {
    return false;
  }
  const std::string query = url.Query();
  const std::vector<std::string_view> pieces = SplitQuery(query);

  std::string kept;
  bool removed = false;
  for (const std::string_view piece : pieces) {
    const std::string_view name = NameOf(piece);
    const bool strip = std::any_of(parameters.begin(), parameters.end(),
                                   [name](std::string_view p) { return p == name; });
    if (strip) {
      removed = true;
      continue;
    }
    if (!kept.empty()) {
      kept.push_back('&');
    }
    kept += piece;
  }
  if (!removed) {
    return false;
  }
  // A URL that had `?a=1` and loses it keeps no `?`. Leaving an empty query
  // behind would make the sanitized URL a different cache key from the clean
  // one a user typed.
  if (kept.empty()) {
    url.SetQuery(std::nullopt);
  } else {
    url.SetQuery(std::move(kept));
  }
  return true;
}

void PrivacyPolicy::AllowInsecureHost(std::string host) {
  if (!IsInsecureHostAllowed(host)) {
    insecure_hosts_.push_back(std::move(host));
  }
}

bool PrivacyPolicy::IsInsecureHostAllowed(std::string_view host) const {
  return std::any_of(insecure_hosts_.begin(), insecure_hosts_.end(),
                     [host](const std::string& allowed) { return allowed == host; });
}

Verdict PrivacyPolicy::Decide(const Request& request, const url::Url* referrer_document) const {
  AddPerformanceCounter(PerfCounterId::PrivacyDecisions);

  if (!request.url.IsValid()) {
    return Verdict::Blocked("not a URL");
  }

  // Blocking first, so a blocked request never causes so much as a DNS lookup
  // for the form it would have been upgraded to.
  const MatchResult match = engine_.Match(request);
  if (match.blocked) {
    return Verdict::Blocked("filter rule");
  }

  url::Url final_url = request.url;
  bool upgraded = false;

  if (final_url.Scheme() == "http") {
    const bool exempt = final_url.GetHost().IsPotentiallyPrivate() ||
                        IsInsecureHostAllowed(final_url.HostSerialized());
    if (settings_.https_only && !exempt) {
      // Upgraded by reparsing rather than by editing the scheme in place: the
      // default port and every other scheme-dependent piece of the URL has to
      // be recomputed, and doing that by hand is how `http://x:80` becomes
      // `https://x:80`.
      std::string upgraded_text = final_url.Serialize();
      upgraded_text.insert(4, "s");
      const auto reparsed = url::Url::Parse(upgraded_text);
      if (!reparsed.has_value()) {
        return Verdict::BlockedInsecure("cannot be reached securely");
      }
      final_url = *reparsed;
      upgraded = true;
      AddPerformanceCounter(PerfCounterId::PrivacyHttpsUpgrades);
    }
  }

  bool sanitized = false;
  if (settings_.block_tracking_parameters) {
    std::vector<std::string_view> to_remove = DefaultTrackingParameters();
    // A `$removeparam` rule contributes to the same operation rather than to a
    // second one, which is what keeps the two from drifting apart.
    to_remove.insert(to_remove.end(), match.removed_parameters.begin(),
                     match.removed_parameters.end());
    sanitized = StripQueryParameters(final_url, to_remove);
    if (sanitized) {
      AddPerformanceCounter(PerfCounterId::PrivacyUrlsSanitized);
    }
  }

  url::PartitionKey partition =
      url::PartitionKey::ForEmbedded(request.container, request.top_level_site, final_url);

  Verdict verdict = Verdict::Allowed(std::move(final_url), std::move(partition));
  if (upgraded) {
    verdict.MarkUpgraded();
  }
  if (sanitized) {
    verdict.MarkSanitized();
  }

  if (referrer_document != nullptr && referrer_document->IsValid()) {
    const url::Origin from = url::Origin::FromUrl(*referrer_document);
    const url::Origin to = url::Origin::FromUrl(verdict.FinalUrl());
    if (from.IsOpaque()) {
      verdict.SetReferrer(std::string());
    } else if (!from.IsPotentiallyTrustworthy() || to.IsPotentiallyTrustworthy()) {
      if (settings_.trim_cross_origin_referrer && !from.IsSameOrigin(to)) {
        // Origin only, with a trailing slash — the full path of the page a user
        // came from is not the destination's business.
        verdict.SetReferrer(from.Serialize() + "/");
      } else {
        verdict.SetReferrer(referrer_document->Serialize(true));
      }
    } else {
      // https to http: no referrer at all, so a downgrade cannot leak where the
      // user was.
      verdict.SetReferrer(std::string());
    }
  }

  return verdict;
}

}  // namespace microbrowser::privacy

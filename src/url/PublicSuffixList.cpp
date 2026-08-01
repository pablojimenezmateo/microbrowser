#include "url/PublicSuffixList.h"

#include <algorithm>
#include <array>
#include <vector>

namespace microbrowser::url {

namespace {

// A curated subset of the Public Suffix List. The rule syntax is the real one:
//
//   "com"            an ordinary rule
//   "*.ck"           a wildcard: any single label under .ck is a suffix
//   "!www.ck"        an exception: www.ck is *not* a suffix, .ck is
//
// Deliberately small and deliberately including a wildcard and an exception,
// because those two are where an implementation is wrong if it is wrong at all
// — a list of plain rules can be matched by a suffix comparison that silently
// mishandles both.
constexpr std::array<std::string_view, 61> kRules = {
    // Generic
    "com", "org", "net", "edu", "gov", "mil", "int", "info", "biz", "name", "pro", "dev", "app",
    "io", "sh", "xyz", "online", "site", "tech", "cloud",
    // Country
    "uk", "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk",
    "de", "fr", "es", "it", "nl", "pl", "se", "no", "fi", "dk", "ie", "pt", "ch", "at", "be",
    "jp", "co.jp", "or.jp", "ne.jp", "ac.jp",
    "cn", "com.cn", "au", "com.au", "net.au", "org.au", "edu.au", "gov.au",
    "br", "com.br", "us",
    // A wildcard registry and its exception, both real entries.
    "*.ck", "!www.ck",
};

std::vector<std::string_view> SplitLabels(std::string_view host) {
  std::vector<std::string_view> labels;
  std::size_t start = 0;
  while (start <= host.size()) {
    const std::size_t dot = host.find('.', start);
    if (dot == std::string_view::npos) {
      labels.push_back(host.substr(start));
      break;
    }
    labels.push_back(host.substr(start, dot - start));
    start = dot + 1;
  }
  return labels;
}

struct Match {
  std::size_t labels = 0;
  bool found = false;
  bool exception = false;
};

// Longest match wins, and an exception rule beats every ordinary rule. That
// ordering is the algorithm; matching "the longest rule that is a string
// suffix" gets `www.ck` wrong and is the usual mistake.
Match FindRule(const std::vector<std::string_view>& labels) {
  Match best;
  for (const std::string_view rule : kRules) {
    const bool is_exception = rule.front() == '!';
    const std::string_view body = is_exception ? rule.substr(1) : rule;
    const std::vector<std::string_view> rule_labels = SplitLabels(body);
    if (rule_labels.size() > labels.size()) {
      continue;
    }

    bool matches = true;
    for (std::size_t i = 0; i < rule_labels.size(); ++i) {
      const std::string_view rule_label = rule_labels[rule_labels.size() - 1 - i];
      const std::string_view host_label = labels[labels.size() - 1 - i];
      if (rule_label == "*") {
        continue;  // matches exactly one label, whatever it is
      }
      if (rule_label != host_label) {
        matches = false;
        break;
      }
    }
    if (!matches) {
      continue;
    }

    // An exception rule always wins; otherwise the longer rule does.
    if (is_exception) {
      if (!best.exception || rule_labels.size() > best.labels) {
        best = Match{rule_labels.size(), true, true};
      }
    } else if (!best.exception && (!best.found || rule_labels.size() > best.labels)) {
      best = Match{rule_labels.size(), true, false};
    }
  }
  return best;
}

bool LooksLikeIpAddress(std::string_view host) {
  if (host.empty()) {
    return false;
  }
  if (host.front() == '[') {
    return true;  // IPv6, already bracketed by the host parser
  }
  // The host parser has canonicalized IPv4 to dotted decimal, so a host whose
  // last label is entirely digits is an address rather than a name.
  const std::size_t dot = host.rfind('.');
  const std::string_view last = dot == std::string_view::npos ? host : host.substr(dot + 1);
  return !last.empty() &&
         std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

std::size_t PublicSuffixLabelCount(std::string_view host) {
  if (host.empty() || LooksLikeIpAddress(host)) {
    return 0;
  }
  const std::vector<std::string_view> labels = SplitLabels(host);
  const Match match = FindRule(labels);
  if (!match.found) {
    // "If no rules match, the prevailing rule is `*`." That line of the
    // algorithm is what makes an unlisted TLD behave sensibly: `widget.example`
    // is one registrable domain rather than nothing at all. Returning zero here
    // instead would make every host under an unlisted TLD its own site *and*
    // no site, depending on which caller asked.
    return 1;
  }
  // An exception rule removes its own leftmost label from the suffix, which is
  // what makes `www.ck` a registrable domain rather than a registry.
  return match.exception ? match.labels - 1 : match.labels;
}

bool IsPublicSuffix(std::string_view host) {
  const std::size_t suffix = PublicSuffixLabelCount(host);
  return suffix != 0 && suffix == SplitLabels(host).size();
}

std::string RegistrableDomain(std::string_view host) {
  if (host.empty() || LooksLikeIpAddress(host)) {
    return {};
  }
  const std::vector<std::string_view> labels = SplitLabels(host);
  const std::size_t suffix = PublicSuffixLabelCount(host);
  if (suffix == 0 || labels.size() <= suffix) {
    // Either no rule matched, or the host *is* the suffix. Both mean nothing is
    // registrable: returning the host would make every `.com` domain one site.
    return {};
  }

  std::string out;
  for (std::size_t i = labels.size() - suffix - 1; i < labels.size(); ++i) {
    if (!out.empty()) {
      out.push_back('.');
    }
    out += labels[i];
  }
  return out;
}

std::size_t PublicSuffixListSize() {
  return kRules.size();
}

}  // namespace microbrowser::url

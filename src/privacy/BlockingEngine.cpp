#include "privacy/BlockingEngine.h"

#include <algorithm>

#include "url/PublicSuffixList.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::privacy {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// A separator in Adblock syntax: anything that is not a letter, digit, `_`,
// `-`, `.` or `%`, plus the end of the URL.
bool IsSeparator(char c) {
  const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9');
  return !(alphanumeric || c == '_' || c == '-' || c == '.' || c == '%');
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool IsAlphanumeric(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

std::string NormalizeDomainOption(std::string_view entry) {
  std::string out;
  const bool negated = !entry.empty() && entry.front() == '~';
  if (negated) {
    entry.remove_prefix(1);
    out.push_back('~');
  }
  if (!entry.empty() && entry.front() == '.') {
    entry.remove_prefix(1);
  }
  out += Lowered(url::HostWithoutTrailingRootDot(entry));
  return out;
}

std::string_view DomainOptionWithoutNegation(std::string_view entry) {
  if (!entry.empty() && entry.front() == '~') {
    entry.remove_prefix(1);
  }
  return entry;
}

// Longest *alphanumeric* run in a pattern, standing in for "most selective
// token". uBO computes real selectivity from list statistics; the longest run
// approximates it and needs no second pass over the list.
//
// It has to be alphanumeric, and by exactly the same rule the URL tokenizer
// uses. A pattern indexed under `/banner.gif` can never be found by a URL
// tokenized into `banner` and `gif`: the two sides must agree on what a token
// is, or the index silently returns nothing and every rule in it stops
// matching.
std::string_view MostSelectiveToken(std::string_view pattern) {
  std::string_view best;
  std::size_t start = std::string_view::npos;
  for (std::size_t i = 0; i <= pattern.size(); ++i) {
    if (i < pattern.size() && IsAlphanumeric(pattern[i])) {
      if (start == std::string_view::npos) {
        start = i;
      }
      continue;
    }
    if (start != std::string_view::npos) {
      const std::string_view run = pattern.substr(start, i - start);
      if (run.size() > best.size()) {
        best = run;
      }
      start = std::string_view::npos;
    }
  }
  return best.size() >= 3 ? best : std::string_view();
}

// Tokenizes a URL into the alphanumeric runs a pattern's token could be. Writes
// into the caller's buffer so the match path allocates nothing.
void Tokenize(std::string_view url, std::vector<std::string_view>& out) {
  out.clear();
  std::size_t start = std::string_view::npos;
  for (std::size_t i = 0; i <= url.size(); ++i) {
    const bool alphanumeric =
        i < url.size() && ((url[i] >= 'a' && url[i] <= 'z') || (url[i] >= 'A' && url[i] <= 'Z') ||
                           (url[i] >= '0' && url[i] <= '9'));
    if (alphanumeric) {
      if (start == std::string_view::npos) {
        start = i;
      }
      continue;
    }
    if (start != std::string_view::npos) {
      if (i - start >= 3) {
        out.push_back(url.substr(start, i - start));
      }
      start = std::string_view::npos;
    }
  }
}

}  // namespace

bool PatternMatches(std::string_view pattern, std::string_view text, bool start_anchored,
                    bool end_anchored) {
  // Split the pattern on `*`; each piece must appear in order. `^` inside a
  // piece matches any separator or the end of the text.
  const auto piece_matches_at = [&](std::string_view piece, std::size_t at) {
    if (at + piece.size() > text.size()) {
      // A trailing `^` is allowed to match the end of the text, so a piece can
      // be one longer than what remains.
      if (piece.empty() || piece.back() != '^' || at + piece.size() != text.size() + 1) {
        return false;
      }
    }
    for (std::size_t i = 0; i < piece.size(); ++i) {
      const std::size_t index = at + i;
      if (piece[i] == '^') {
        if (index == text.size()) {
          continue;  // end of text counts as a separator
        }
        if (index > text.size() || !IsSeparator(text[index])) {
          return false;
        }
        continue;
      }
      if (index >= text.size() || text[index] != piece[i]) {
        return false;
      }
    }
    return true;
  };

  std::vector<std::string_view> pieces;
  std::size_t start = 0;
  while (true) {
    const std::size_t star = pattern.find('*', start);
    if (star == std::string_view::npos) {
      pieces.push_back(pattern.substr(start));
      break;
    }
    pieces.push_back(pattern.substr(start, star - start));
    start = star + 1;
  }

  std::size_t position = 0;
  for (std::size_t index = 0; index < pieces.size(); ++index) {
    const std::string_view piece = pieces[index];
    if (piece.empty()) {
      continue;
    }
    const bool first = index == 0;
    const bool last = index + 1 == pieces.size();

    if (first && start_anchored) {
      if (!piece_matches_at(piece, 0)) {
        return false;
      }
      position = piece.size();
      continue;
    }
    if (last && end_anchored) {
      if (piece.size() > text.size()) {
        return false;
      }
      const std::size_t at = text.size() - piece.size();
      if (at < position || !piece_matches_at(piece, at)) {
        return false;
      }
      position = text.size();
      continue;
    }

    bool found = false;
    for (std::size_t at = position; at <= text.size(); ++at) {
      if (piece_matches_at(piece, at)) {
        position = at + piece.size();
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

void BlockingEngine::AddRules(std::string_view list_text) {
  std::size_t start = 0;
  while (start <= list_text.size()) {
    const std::size_t newline = list_text.find('\n', start);
    const std::string_view line =
        newline == std::string_view::npos ? list_text.substr(start) : list_text.substr(start, newline - start);
    AddRule(Trim(line));
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
}

void BlockingEngine::AddRule(std::string_view line) {
  if (line.empty() || line.front() == '!' || line.front() == '[') {
    return;  // a comment or a list header, not a rule
  }
  // Cosmetic filters arrive with the style engine in M4. Skipped and counted,
  // not guessed at.
  if (line.find("##") != std::string_view::npos || line.find("#@#") != std::string_view::npos) {
    ++skipped_;
    return;
  }

  CompiledRule rule;
  if (line.size() >= 2 && line.substr(0, 2) == "@@") {
    rule.flags |= CompiledRule::Exception;
    line.remove_prefix(2);
  }

  std::string_view options;
  const std::size_t dollar = line.rfind('$');
  if (dollar != std::string_view::npos) {
    options = line.substr(dollar + 1);
    line = line.substr(0, dollar);
  }

  // Regex rules are the one part of the engine with no index, and are not
  // implemented yet rather than approximated. An approximated regex is a rule
  // that matches the wrong requests.
  if (line.size() >= 2 && line.front() == '/' && line.back() == '/') {
    ++skipped_;
    return;
  }

  if (line.size() >= 2 && line.substr(0, 2) == "||") {
    rule.flags |= CompiledRule::HostAnchored;
    line.remove_prefix(2);
  } else if (!line.empty() && line.front() == '|') {
    rule.flags |= CompiledRule::StartAnchored;
    line.remove_prefix(1);
  }
  if (!line.empty() && line.back() == '|') {
    rule.flags |= CompiledRule::EndAnchored;
    line.remove_suffix(1);
  }
  if (line.empty()) {
    ++skipped_;
    return;
  }

  std::uint16_t types = 0;
  std::uint16_t excluded_types = 0;
  bool any_type_named = false;
  bool any_type_excluded = false;
  bool unknown_option = false;

  const auto add_resource_type = [&](std::string_view option, ResourceType type) {
    const bool negated = !option.empty() && option.front() == '~';
    if (negated) {
      excluded_types |= ResourceTypeBit(type);
      any_type_excluded = true;
    } else {
      types |= ResourceTypeBit(type);
      any_type_named = true;
    }
  };

  const auto add_domain_option = [&](std::string_view domain) {
    if (domain.empty()) {
      return;
    }
    const std::string normalized = NormalizeDomainOption(domain);
    if (DomainOptionWithoutNegation(normalized).empty()) {
      return;
    }
    if (rule.domain_count == 0) {
      rule.domain_offset = static_cast<std::uint32_t>(domains_.size());
    }
    domains_.push_back(normalized);
    ++rule.domain_count;
  };

  const auto each_option = [&](std::string_view text) {
    std::size_t option_start = 0;
    while (option_start <= text.size()) {
      const std::size_t comma = text.find(',', option_start);
      const std::string_view option = comma == std::string_view::npos
                                          ? text.substr(option_start)
                                          : text.substr(option_start, comma - option_start);
      if (!option.empty()) {
        if (option == "third-party" || option == "3p") {
          rule.flags |= CompiledRule::ThirdPartyOnly;
        } else if (option == "~third-party" || option == "1p" || option == "first-party") {
          rule.flags |= CompiledRule::FirstPartyOnly;
        } else if (option == "important") {
          rule.flags |= CompiledRule::Important;
        } else if (option == "script" || option == "~script") {
          add_resource_type(option, ResourceType::Script);
        } else if (option == "image" || option == "~image") {
          add_resource_type(option, ResourceType::Image);
        } else if (option == "stylesheet" || option == "~stylesheet" || option == "css" ||
                   option == "~css") {
          add_resource_type(option, ResourceType::Stylesheet);
        } else if (option == "xhr" || option == "~xhr" || option == "xmlhttprequest" ||
                   option == "~xmlhttprequest") {
          add_resource_type(option, ResourceType::Xhr);
        } else if (option == "font" || option == "~font") {
          add_resource_type(option, ResourceType::Font);
        } else if (option == "media" || option == "~media") {
          add_resource_type(option, ResourceType::Media);
        } else if (option == "subdocument" || option == "~subdocument" || option == "frame" ||
                   option == "~frame") {
          add_resource_type(option, ResourceType::Subdocument);
        } else if (option == "websocket" || option == "~websocket") {
          add_resource_type(option, ResourceType::WebSocket);
        } else if (option == "ping" || option == "~ping" || option == "beacon" ||
                   option == "~beacon") {
          add_resource_type(option, ResourceType::Ping);
        } else if (option == "document" || option == "~document") {
          add_resource_type(option, ResourceType::Document);
        } else if (option.size() > 7 && option.substr(0, 7) == "domain=") {
          const std::string_view list = option.substr(7);
          std::size_t piece_start = 0;
          while (piece_start <= list.size()) {
            const std::size_t bar = list.find('|', piece_start);
            const std::string_view domain = bar == std::string_view::npos
                                                ? list.substr(piece_start)
                                                : list.substr(piece_start, bar - piece_start);
            add_domain_option(domain);
            if (bar == std::string_view::npos) {
              break;
            }
            piece_start = bar + 1;
          }
        } else if (option.size() > 12 && option.substr(0, 12) == "removeparam=") {
          rule.flags |= CompiledRule::RemoveParam;
          const std::string_view parameter = option.substr(12);
          rule.parameter_offset = static_cast<std::uint32_t>(arena_.size());
          rule.parameter_length = static_cast<std::uint16_t>(parameter.size());
          arena_ += parameter;
        } else {
          unknown_option = true;
        }
      }
      if (comma == std::string_view::npos) {
        break;
      }
      option_start = comma + 1;
    }
  };
  each_option(options);

  if (unknown_option) {
    // The rule might mean something narrower than we understand, and applying
    // it anyway would block requests it was not written for.
    ++skipped_;
    return;
  }
  if (any_type_named) {
    rule.resource_types = types;
  } else if (any_type_excluded) {
    rule.resource_types = kAllResourceTypes;
  }
  if (any_type_excluded) {
    rule.resource_types &= static_cast<std::uint16_t>(~excluded_types);
  }

  rule.pattern_offset = static_cast<std::uint32_t>(arena_.size());
  rule.pattern_length = static_cast<std::uint32_t>(line.size());
  arena_ += line;

  const auto index = static_cast<std::uint32_t>(rules_.size());
  rules_.push_back(rule);
  Index& target = rule.Has(CompiledRule::Exception) ? allow_ : block_;

  const std::string_view pattern(arena_.data() + rule.pattern_offset, rule.pattern_length);
  if (rule.Has(CompiledRule::HostAnchored)) {
    // The host part of `||host/path` is up to the first separator.
    std::size_t host_end = 0;
    while (host_end < pattern.size() && pattern[host_end] != '/' && pattern[host_end] != '^' &&
           pattern[host_end] != '*') {
      ++host_end;
    }
    if (host_end > 0) {
      target.by_host[std::string(pattern.substr(0, host_end))].push_back(index);
      return;
    }
  }
  const std::string_view token = MostSelectiveToken(pattern);
  if (!token.empty()) {
    std::string lowered = Lowered(token);
    target.by_token[lowered].push_back(index);
  } else {
    target.unindexed.push_back(index);
  }
}

bool BlockingEngine::MatchesRule(const CompiledRule& rule, const Request& request,
                                 std::string_view url, std::string_view host) const {
  if ((rule.resource_types & ResourceTypeBit(request.type)) == 0) {
    return false;
  }
  const bool third_party = request.IsThirdParty();
  if (rule.Has(CompiledRule::ThirdPartyOnly) && !third_party) {
    return false;
  }
  if (rule.Has(CompiledRule::FirstPartyOnly) && third_party) {
    return false;
  }

  if (rule.domain_count > 0) {
    // `$domain=a.com|~b.com`: an entry without `~` means "only on these", one
    // with means "except on these". Both forms in one list is legal.
    bool has_positive = false;
    bool positive_matched = false;
    bool negative_matched = false;
    const std::string& top = request.top_level_site.RegistrableDomain();
    for (std::uint16_t i = 0; i < rule.domain_count; ++i) {
      std::string_view entry = domains_[rule.domain_offset + i];
      const bool negated = !entry.empty() && entry.front() == '~';
      if (negated) {
        entry.remove_prefix(1);
      } else {
        has_positive = true;
      }
      const bool matches =
          top == entry ||
          (top.size() > entry.size() + 1 &&
           top.compare(top.size() - entry.size(), entry.size(), entry) == 0 &&
           top[top.size() - entry.size() - 1] == '.');
      if (matches) {
        (negated ? negative_matched : positive_matched) = true;
      }
    }
    if (negative_matched) {
      return false;
    }
    if (has_positive && !positive_matched) {
      return false;
    }
  }

  const std::string_view pattern(arena_.data() + rule.pattern_offset, rule.pattern_length);
  if (rule.Has(CompiledRule::HostAnchored)) {
    // `||example.com^` matches the host and every subdomain of it, and matches
    // against the URL from the host onward rather than from the scheme.
    const std::size_t host_position = url.find(host);
    if (host_position == std::string_view::npos) {
      return false;
    }
    const std::string_view from_host = url.substr(host_position);
    if (PatternMatches(pattern, from_host, true, rule.Has(CompiledRule::EndAnchored))) {
      return true;
    }
    // Try each subdomain boundary, so `||example.com^` matches
    // `a.b.example.com`.
    std::size_t label = 0;
    while ((label = host.find('.', label)) != std::string_view::npos) {
      ++label;
      const std::string_view suffix = url.substr(host_position + label);
      if (PatternMatches(pattern, suffix, true, rule.Has(CompiledRule::EndAnchored))) {
        return true;
      }
    }
    return false;
  }

  return PatternMatches(pattern, url, rule.Has(CompiledRule::StartAnchored),
                        rule.Has(CompiledRule::EndAnchored));
}

void BlockingEngine::Collect(const Index& index, std::string_view url, std::string_view host,
                             std::vector<std::uint32_t>& out) const {
  out.clear();
  // Walk the host's suffixes: `a.b.example.com` probes itself, `b.example.com`,
  // `example.com` and `com`. That is where the "and all subdomains" semantics
  // come from, rather than from a loop over every host rule.
  std::string_view remaining = host;
  while (!remaining.empty()) {
    const auto found = index.by_host.find(remaining);
    if (found != index.by_host.end()) {
      out.insert(out.end(), found->second.begin(), found->second.end());
    }
    const std::size_t dot = remaining.find('.');
    if (dot == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(dot + 1);
  }

  std::vector<std::string_view> tokens;
  Tokenize(url, tokens);
  std::string lowered;
  for (const std::string_view token : tokens) {
    lowered = Lowered(token);
    const auto found = index.by_token.find(lowered);
    if (found != index.by_token.end()) {
      out.insert(out.end(), found->second.begin(), found->second.end());
    }
  }
  out.insert(out.end(), index.unindexed.begin(), index.unindexed.end());

  AddPerformanceCounter(PerfCounterId::PrivacyRulesProbed, out.size());
}

MatchResult BlockingEngine::Match(const Request& request) const {
  MatchResult result;
  AddPerformanceCounter(PerfCounterId::PrivacyRequestsMatched);

  const std::string url = request.url.Serialize(true);
  const std::string_view host = request.url.HostSerialized();

  std::vector<std::uint32_t> candidates;
  Collect(block_, url, host, candidates);

  bool blocked = false;
  bool important = false;
  for (const std::uint32_t index : candidates) {
    const CompiledRule& rule = rules_[index];
    if (!MatchesRule(rule, request, url, host)) {
      continue;
    }
    if (rule.Has(CompiledRule::RemoveParam)) {
      // A removeparam rule rewrites rather than blocks, so it never sets the
      // blocked flag however many of them match.
      result.removed_parameters.emplace_back(arena_.data() + rule.parameter_offset,
                                             rule.parameter_length);
      continue;
    }
    blocked = true;
    result.rule = index;
    if (rule.Has(CompiledRule::Important)) {
      important = true;
      break;
    }
  }

  if (blocked && !important) {
    // Exceptions are consulted only after something matched, so the common
    // case — no match at all — never touches them.
    Collect(allow_, url, host, candidates);
    for (const std::uint32_t index : candidates) {
      if (MatchesRule(rules_[index], request, url, host)) {
        blocked = false;
        result.rule = index;
        break;
      }
    }
  }

  result.blocked = blocked;
  if (blocked) {
    AddPerformanceCounter(PerfCounterId::PrivacyRequestsBlocked);
  }
  return result;
}

}  // namespace microbrowser::privacy

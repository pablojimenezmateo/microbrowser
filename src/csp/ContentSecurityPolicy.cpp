#include "csp/ContentSecurityPolicy.h"

#include <algorithm>
#include <cstddef>

#include "util/Base64.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::csp {

namespace {

bool IsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsWhitespace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsWhitespace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::vector<std::string_view> SplitOnWhitespace(std::string_view text) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsWhitespace(text[i])) {
      ++i;
    }
    const std::size_t start = i;
    while (i < text.size() && !IsWhitespace(text[i])) {
      ++i;
    }
    if (i > start) {
      tokens.push_back(text.substr(start, i - start));
    }
  }
  return tokens;
}

std::optional<Directive> DirectiveNamed(std::string_view name) {
  // Lower-cased by the caller. The `-elem` and `-attr` variants are
  // deliberately folded into their base directive rather than ignored: a page
  // that writes `script-src-elem 'nonce-x'` and nothing else means its scripts
  // are governed, and treating that as "no policy" would be the one failure
  // mode a policy engine must not have.
  if (name == "default-src") {
    return Directive::Default;
  }
  if (name == "script-src" || name == "script-src-elem") {
    return Directive::Script;
  }
  if (name == "style-src" || name == "style-src-elem") {
    return Directive::Style;
  }
  if (name == "img-src") {
    return Directive::Img;
  }
  if (name == "connect-src") {
    return Directive::Connect;
  }
  if (name == "form-action") {
    return Directive::FormAction;
  }
  if (name == "base-uri") {
    return Directive::BaseUri;
  }
  if (name == "frame-src" || name == "child-src") {
    // `child-src` is the older spelling and the specification still makes it
    // `frame-src`'s fallback. One entry rather than two, because a page that
    // sets only `child-src` means it about frames -- workers went their own way
    // and this browser does not load one from a URL a policy could name.
    return Directive::Frame;
  }
  return std::nullopt;
}

std::size_t IndexOf(Directive directive) {
  return static_cast<std::size_t>(directive);
}

// `'sha256-…'`, `'sha384-…'`, `'sha512-…'`. The digest is decoded here rather
// than compared as text: base64 has more than one spelling of the same bytes,
// and an integrity decision that compared strings would be a decision two
// spellings disagree about.
std::optional<Source> ParseHashSource(std::string_view body) {
  util::HashAlgorithm algorithm = util::HashAlgorithm::Sha256;
  std::string_view digest;
  if (util::StartsWithAsciiCaseInsensitive(body, "sha256-")) {
    algorithm = util::HashAlgorithm::Sha256;
    digest = body.substr(7);
  } else if (util::StartsWithAsciiCaseInsensitive(body, "sha384-")) {
    algorithm = util::HashAlgorithm::Sha384;
    digest = body.substr(7);
  } else if (util::StartsWithAsciiCaseInsensitive(body, "sha512-")) {
    algorithm = util::HashAlgorithm::Sha512;
    digest = body.substr(7);
  } else {
    return std::nullopt;
  }
  const std::optional<std::string> decoded = util::Base64Decode(digest);
  if (!decoded.has_value() || decoded->size() != util::HashLength(algorithm)) {
    return std::nullopt;
  }
  Source source;
  source.kind = Source::Kind::Hash;
  source.algorithm = algorithm;
  source.value = *decoded;
  return source;
}

// A host-source, or a scheme-source, or the `*` that is neither.
//
// Written by hand rather than through url::Url::Parse, because a source
// expression is not a URL: `*.example.com` has no scheme, `https:` has nothing
// after it, and both would come back from a URL parser as something else
// entirely.
std::optional<Source> ParseHostOrScheme(std::string_view token) {
  Source source;
  if (token == "*") {
    source.kind = Source::Kind::Wildcard;
    return source;
  }

  // A scheme part is `scheme://` for a host-source and `scheme:` for a
  // scheme-source. The `//` is what tells them apart.
  const std::size_t colon = token.find(':');
  if (colon != std::string_view::npos) {
    const std::string_view maybe_scheme = token.substr(0, colon);
    const bool schemey =
        !maybe_scheme.empty() && util::IsAsciiAlpha(maybe_scheme.front()) &&
        std::all_of(maybe_scheme.begin(), maybe_scheme.end(), [](char c) {
          return util::IsAsciiAlphanumeric(c) || c == '+' || c == '-' || c == '.';
        });
    if (schemey) {
      if (colon + 1 == token.size()) {
        source.kind = Source::Kind::Scheme;
        source.scheme = util::AsciiLowerCase(maybe_scheme);
        return source;
      }
      if (token.substr(colon + 1, 2) == "//") {
        source.scheme = util::AsciiLowerCase(maybe_scheme);
        token = token.substr(colon + 3);
      }
    }
  }

  if (token.empty()) {
    return std::nullopt;
  }

  // Path first, so that a `:` inside it is not read as a port.
  if (const std::size_t slash = token.find('/'); slash != std::string_view::npos) {
    source.path = std::string(token.substr(slash));
    token = token.substr(0, slash);
  }
  if (token.empty()) {
    return std::nullopt;
  }
  // Then the port, from the end.
  if (const std::size_t port_at = token.rfind(':'); port_at != std::string_view::npos) {
    const std::string_view port = token.substr(port_at + 1);
    if (port == "*") {
      source.any_port = true;
    } else {
      const std::optional<std::size_t> parsed = util::ParseSize(port);
      if (!parsed.has_value() || *parsed > 65535) {
        return std::nullopt;
      }
      source.port = static_cast<std::uint16_t>(*parsed);
    }
    token = token.substr(0, port_at);
  }
  if (token.empty()) {
    return std::nullopt;
  }
  source.kind = Source::Kind::Host;
  source.host = util::AsciiLowerCase(token);
  return source;
}

Source ParseSource(std::string_view token) {
  Source nothing;
  if (token.size() >= 2 && token.front() == '\'' && token.back() == '\'') {
    const std::string_view body = token.substr(1, token.size() - 2);
    if (util::EqualsAsciiCaseInsensitive(body, "self")) {
      Source source;
      source.kind = Source::Kind::Self;
      return source;
    }
    if (util::EqualsAsciiCaseInsensitive(body, "unsafe-inline")) {
      Source source;
      source.kind = Source::Kind::UnsafeInline;
      return source;
    }
    if (util::EqualsAsciiCaseInsensitive(body, "unsafe-eval")) {
      Source source;
      source.kind = Source::Kind::UnsafeEval;
      return source;
    }
    if (util::StartsWithAsciiCaseInsensitive(body, "nonce-")) {
      Source source;
      source.kind = Source::Kind::Nonce;
      // The nonce is compared byte for byte and case-sensitively, which is what
      // the specification says and what makes a nonce a secret rather than a
      // label.
      source.value = std::string(body.substr(6));
      return source.value.empty() ? nothing : source;
    }
    if (std::optional<Source> hash = ParseHashSource(body); hash.has_value()) {
      return *hash;
    }
    // `'none'`, `'unsafe-hashes'`, and anything else quoted that is not a
    // keyword this parser understands. `'strict-dynamic'` is handled in
    // Policy::Parse rather than here (it is not a URL source).
    return nothing;
  }
  if (std::optional<Source> parsed = ParseHostOrScheme(token); parsed.has_value()) {
    return *parsed;
  }
  return nothing;
}

bool PortMatches(const Source& source, const url::Url& target) {
  if (source.any_port) {
    return true;
  }
  const std::optional<std::uint16_t> effective = target.EffectivePort();
  if (source.port.has_value()) {
    return effective.has_value() && *effective == *source.port;
  }
  // No port part: the URL must be on its scheme's default port. The one
  // exception the specification carves out is the upgrade -- an `http` source
  // with no port matching an `https` URL on 443 -- and it falls out of this,
  // because 443 *is* https's default.
  const std::optional<std::uint16_t> default_port = url::DefaultPortForScheme(target.Scheme());
  if (!effective.has_value()) {
    return true;
  }
  return default_port.has_value() && *effective == *default_port;
}

bool HostMatches(std::string_view pattern, std::string_view host) {
  if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
    const std::string_view suffix = pattern.substr(1);  // ".example.com"
    // A subdomain, and never the domain itself: `*.example.com` does not match
    // `example.com`, which is the rule that stops a wildcard from being a
    // shorter way to write the bare host.
    return host.size() > suffix.size() &&
           util::EqualsAsciiCaseInsensitive(host.substr(host.size() - suffix.size()), suffix);
  }
  return util::EqualsAsciiCaseInsensitive(pattern, host);
}

bool PathMatches(std::string_view pattern, const url::Url& target) {
  if (pattern.empty() || pattern == "/") {
    return true;
  }
  const std::string path = target.PathString();
  if (pattern.back() == '/') {
    return path.size() >= pattern.size() && path.compare(0, pattern.size(), pattern) == 0;
  }
  return path == pattern;
}

// Whether `from` may be upgraded to `to`: the specification lets an `http`
// source match an `https` URL and `ws` match `wss`, because a policy written
// before a site moved to TLS must not stop it moving.
bool SchemeMatches(std::string_view source_scheme, std::string_view target_scheme) {
  if (util::EqualsAsciiCaseInsensitive(source_scheme, target_scheme)) {
    return true;
  }
  if (util::EqualsAsciiCaseInsensitive(source_scheme, "http") &&
      util::EqualsAsciiCaseInsensitive(target_scheme, "https")) {
    return true;
  }
  return util::EqualsAsciiCaseInsensitive(source_scheme, "ws") &&
         util::EqualsAsciiCaseInsensitive(target_scheme, "wss");
}

bool MatchesSelf(const url::Url& target, const url::Origin& self) {
  if (self.IsOpaque()) {
    // A document with no origin -- a `data:` URL -- has nothing for `'self'` to
    // name, and the specification says such a policy matches nothing.
    return false;
  }
  if (!util::EqualsAsciiCaseInsensitive(target.HostSerialized(), self.Host())) {
    return false;
  }
  if (!SchemeMatches(self.Scheme(), target.Scheme())) {
    return false;
  }
  const std::optional<std::uint16_t> target_port = target.EffectivePort();
  if (util::EqualsAsciiCaseInsensitive(self.Scheme(), target.Scheme())) {
    return target_port == self.Port() ||
           (!self.Port().has_value() &&
            target_port == url::DefaultPortForScheme(target.Scheme()));
  }
  // The upgrade case: the port must be the upgraded scheme's default, or the
  // policy would allow an arbitrary port it never named.
  return target_port == url::DefaultPortForScheme(target.Scheme());
}

bool MatchesUrl(const Source& source, const url::Url& target, const url::Origin& self) {
  switch (source.kind) {
    case Source::Kind::Nothing:
    case Source::Kind::Nonce:
    case Source::Kind::Hash:
    case Source::Kind::UnsafeInline:
    case Source::Kind::UnsafeEval:
      // None of these say anything about a URL. `'unsafe-inline'` in
      // particular does not allow an external script, which is the mistake a
      // table-driven implementation makes.
      return false;
    case Source::Kind::Self:
      return MatchesSelf(target, self);
    case Source::Kind::Wildcard:
      // `*` is every *network* scheme, and deliberately not `data:` or
      // `blob:`. A wildcard that matched `data:` would make
      // `img-src *` allow `data:image/svg+xml,<svg onload=…>`.
      return target.Scheme() == "http" || target.Scheme() == "https" ||
             target.Scheme() == "ws" || target.Scheme() == "wss" ||
             target.Scheme() == "ftp" ||
             (!self.IsOpaque() && util::EqualsAsciiCaseInsensitive(target.Scheme(), self.Scheme()));
    case Source::Kind::Scheme:
      return SchemeMatches(source.scheme, target.Scheme());
    case Source::Kind::Host:
      break;
  }
  if (source.scheme.empty()) {
    if (self.IsOpaque() || !SchemeMatches(self.Scheme(), target.Scheme())) {
      return false;
    }
  } else if (!SchemeMatches(source.scheme, target.Scheme())) {
    return false;
  }
  return HostMatches(source.host, target.HostSerialized()) && PortMatches(source, target) &&
         PathMatches(source.path, target);
}

}  // namespace

Policy Policy::Parse(std::string_view serialized) {
  Policy policy;
  std::size_t at = 0;
  while (at <= serialized.size()) {
    const std::size_t end = serialized.find(';', at);
    const std::string_view piece =
        Trim(serialized.substr(at, end == std::string_view::npos ? std::string_view::npos
                                                                 : end - at));
    at = end == std::string_view::npos ? serialized.size() + 1 : end + 1;
    if (piece.empty()) {
      continue;
    }
    const std::vector<std::string_view> tokens = SplitOnWhitespace(piece);
    const std::optional<Directive> directive =
        DirectiveNamed(util::AsciiLowerCase(tokens.front()));
    if (!directive.has_value()) {
      // Unknown, or one this browser has no enforcement point for. Ignored --
      // failing the policy closed here would break a page over `report-to`.
      continue;
    }
    std::vector<Source>& list = policy.lists_[IndexOf(*directive)];
    if (!list.empty()) {
      // A directive given twice: the first wins and the rest is ignored, which
      // is the specification's rule and the one that stops a second, looser
      // copy from taking effect.
      continue;
    }
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string_view token = tokens[i];
      if (*directive == Directive::Script && token.size() >= 2 && token.front() == '\'' &&
          token.back() == '\'') {
        const std::string_view body = token.substr(1, token.size() - 2);
        if (util::EqualsAsciiCaseInsensitive(body, "strict-dynamic")) {
          policy.script_strict_dynamic_ = true;
          continue;
        }
      }
      list.push_back(ParseSource(token));
    }
    if (list.empty()) {
      // An empty source list allows nothing, exactly as `'none'` does.
      list.push_back(Source{});
    }
  }
  return policy;
}

bool Policy::Empty() const {
  return std::all_of(lists_.begin(), lists_.end(),
                     [](const std::vector<Source>& list) { return list.empty(); });
}

const std::vector<Source>* Policy::ListFor(Directive directive) const {
  const std::vector<Source>& own = lists_[IndexOf(directive)];
  if (!own.empty()) {
    return &own;
  }
  // `form-action` and `base-uri` do not fall back to `default-src`. That is the
  // specification's rule and the one people are most often surprised by: a page
  // with `default-src 'none'` can still submit a form anywhere.
  if (directive == Directive::FormAction || directive == Directive::BaseUri ||
      directive == Directive::Default) {
    return nullptr;
  }
  const std::vector<Source>& fallback = lists_[IndexOf(Directive::Default)];
  return fallback.empty() ? nullptr : &fallback;
}

bool Policy::AllowsUrl(Directive directive, const url::Url& target, const url::Origin& self,
                       std::string_view nonce) const {
  const std::vector<Source>* list = ListFor(directive);
  if (list == nullptr) {
    return true;
  }
  for (const Source& source : *list) {
    if (source.kind == Source::Kind::Nonce) {
      // CSP2 and later: a nonce on the element allows the resource it names,
      // whether that resource is inline or fetched.
      if (!nonce.empty() && source.value == nonce) {
        return true;
      }
      continue;
    }
    // CSP3: with `'strict-dynamic'`, host allowlists do not authorize script
    // loads. Only a nonce (above) or transitive trust in bindings does.
    if (directive == Directive::Script && script_strict_dynamic_) {
      continue;
    }
    if (MatchesUrl(source, target, self)) {
      return true;
    }
  }
  return false;
}

bool Policy::AllowsEval() const {
  // CSP3: `eval` is gated by `script-src` / `default-src`. No governing list
  // means the platform default (allowed). A governing list without
  // `'unsafe-eval'` forbids it — including `'none'`.
  const std::vector<Source>* list = ListFor(Directive::Script);
  if (list == nullptr) {
    return true;
  }
  for (const Source& source : *list) {
    if (source.kind == Source::Kind::UnsafeEval) {
      return true;
    }
  }
  return false;
}

bool Policy::AllowsInline(Directive directive, std::string_view nonce,
                          std::string_view body) const {
  const std::vector<Source>* list = ListFor(directive);
  if (list == nullptr) {
    return true;
  }
  bool unsafe_inline = false;
  bool nonce_or_hash = false;
  for (const Source& source : *list) {
    switch (source.kind) {
      case Source::Kind::Nonce:
        nonce_or_hash = true;
        if (!nonce.empty() && source.value == nonce) {
          return true;
        }
        break;
      case Source::Kind::Hash:
        nonce_or_hash = true;
        if (util::Sha2(source.algorithm, body) == source.value) {
          return true;
        }
        break;
      case Source::Kind::UnsafeInline:
        unsafe_inline = true;
        break;
      default:
        break;
    }
  }
  // `'unsafe-inline'` is ignored when the list also carries a nonce or a hash.
  // That is the whole mechanism by which a modern policy stays safe on a
  // browser that understands nonces while staying usable on one that does not.
  return unsafe_inline && !nonce_or_hash;
}

void PolicyList::AddFromHeader(std::string_view value) {
  std::size_t at = 0;
  while (at <= value.size()) {
    const std::size_t comma = value.find(',', at);
    const std::string_view piece = value.substr(
        at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
    at = comma == std::string_view::npos ? value.size() + 1 : comma + 1;
    Policy policy = Policy::Parse(piece);
    if (!policy.Empty()) {
      policies_.push_back(std::move(policy));
    }
  }
}

void PolicyList::AddFromMeta(std::string_view value) {
  // A `<meta>`-delivered policy carries no comma-separated list: the
  // specification says the whole attribute is one policy.
  Policy policy = Policy::Parse(value);
  if (!policy.Empty()) {
    policies_.push_back(std::move(policy));
  }
}

bool PolicyList::AllowsUrl(Directive directive, const url::Url& target, const url::Origin& self,
                           std::string_view nonce) const {
  return std::all_of(policies_.begin(), policies_.end(), [&](const Policy& policy) {
    return policy.AllowsUrl(directive, target, self, nonce);
  });
}

bool PolicyList::AllowsInline(Directive directive, std::string_view nonce,
                              std::string_view body) const {
  return std::all_of(policies_.begin(), policies_.end(), [&](const Policy& policy) {
    return policy.AllowsInline(directive, nonce, body);
  });
}

bool PolicyList::Governs(Directive directive) const {
  return std::any_of(policies_.begin(), policies_.end(),
                     [&](const Policy& policy) { return policy.Governs(directive); });
}

bool PolicyList::ScriptStrictDynamic() const {
  return std::any_of(policies_.begin(), policies_.end(),
                     [](const Policy& policy) { return policy.ScriptStrictDynamic(); });
}

bool PolicyList::AllowsEval() const {
  return std::all_of(policies_.begin(), policies_.end(),
                     [](const Policy& policy) { return policy.AllowsEval(); });
}

}  // namespace microbrowser::csp

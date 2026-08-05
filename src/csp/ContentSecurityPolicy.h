#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "url/Origin.h"
#include "url/Url.h"
#include "util/Sha2.h"

namespace microbrowser::csp {

// Content-Security-Policy: the page's own policy, enforced rather than logged.
//
// ADR 0020 §3. `privacy::Verdict` answers whether a request is allowed by *our*
// policy; this answers whether it is allowed by the *page's*. Two policies, one
// chokepoint, and a request needs both.
//
// Three decisions are visible in the shape of this file:
//
//   * **Nothing is reported.** `report-uri` and `report-to` are unknown
//     directives here and are dropped. A violation report is an outbound
//     request the user did not cause, describing what the user's browser did,
//     sent to a third party -- which AGENTS.md forbids by name.
//     `Content-Security-Policy-Report-Only` is therefore neither enforced nor
//     reported, which is why nothing here takes one: a report-only policy that
//     blocked would break the pages that are testing a policy before
//     committing to it.
//   * **An unknown directive fails open for the policy and closed for itself.**
//     A directive this browser does not implement is ignored, because failing
//     the whole policy closed would break a page over a directive nobody has
//     written yet. A directive it *does* implement whose source list contains
//     a token it cannot parse keeps the tokens it understood and turns the
//     other into a source that matches nothing -- stricter, never looser. That
//     is also how `'none'` and an empty source list are represented, because
//     that is exactly what they mean.
//   * **A list, not a policy.** Two `Content-Security-Policy` headers are two
//     policies and *both* must allow. Merging them would let a page loosen its
//     own policy by sending a second, weaker one.

// The directives this browser enforces, and each has an enforcement point. A
// directive with none is deliberately absent: `frame-src` and `frame-ancestors`
// are not here because there are no nested browsing contexts to apply them to,
// and a directive that parsed but decided nothing would read as enforcement.
enum class Directive : std::uint8_t {
  // The fallback. Never asked about directly.
  Default,
  Script,
  Style,
  Img,
  Connect,
  FormAction,
  BaseUri,
};

// One source expression. Public because `Policy` stores them by value and a
// vector needs a complete type; nothing outside this module has a reason to
// build one.
struct Source {
  enum class Kind : std::uint8_t {
    // `'none'`, and anything that did not parse. Matches nothing, ever.
    Nothing,
    Self,
    Wildcard,
    // `https:` -- a scheme and nothing else.
    Scheme,
    // `*.example.com:443/path`, with every part optional but the host.
    Host,
    Nonce,
    Hash,
    UnsafeInline,
  };

  Kind kind = Kind::Nothing;
  std::string scheme;
  // May begin with `*.`, which matches a subdomain and not the domain itself.
  std::string host;
  // Empty when the expression names none, which matches every path.
  std::string path;
  // The nonce, or a hash-source's digest as raw bytes.
  std::string value;
  std::optional<std::uint16_t> port;
  bool any_port = false;
  util::HashAlgorithm algorithm = util::HashAlgorithm::Sha256;
};

// One policy: the source lists of one header value or one `<meta>`.
class Policy {
 public:
  static Policy Parse(std::string_view serialized);

  // Whether a URL may be fetched for this purpose. `self` is the document's
  // origin, which `'self'` and a scheme-less host-source are both relative to.
  // `nonce` is the one on the element that named the URL -- a nonce allows an
  // external script as well as an inline one, which is CSP2's change and the
  // reason reddit's `script-src 'nonce-…'` works at all.
  bool AllowsUrl(Directive directive, const url::Url& target, const url::Origin& self,
                 std::string_view nonce = std::string_view()) const;

  // Whether an inline `<script>` or `<style>` may run. `body` is its text, for
  // the hash-sources: it is hashed only when the list contains one, so the
  // common policy costs no digest at all.
  bool AllowsInline(Directive directive, std::string_view nonce, std::string_view body) const;

  // Whether this policy says anything about `directive`, following the
  // `default-src` fallback. False means everything is allowed, which is what an
  // absent directive means and is not what an empty one means.
  bool Governs(Directive directive) const { return ListFor(directive) != nullptr; }

  // Whether the header parsed into any directive this browser enforces.
  bool Empty() const;

 private:
  const std::vector<Source>* ListFor(Directive directive) const;

  // One entry per Directive, indexed by it. An array rather than a map because
  // the set is closed, and a lookup that can miss is a lookup that can silently
  // allow. An empty entry means the directive was absent; `'none'` is a
  // one-element entry that matches nothing.
  std::array<std::vector<Source>, 7> lists_;
};

// Every policy in force on one document.
class PolicyList {
 public:
  // Appends the policies in one `Content-Security-Policy` header value. One
  // header may carry several, comma-separated -- no source expression can
  // contain a comma, which is what makes the split safe.
  void AddFromHeader(std::string_view value);
  // The same, from `<meta http-equiv="Content-Security-Policy">`. A separate
  // entry point because a reader of the engine should be able to see which of
  // the two a policy came from, and because the two differ in what they may
  // carry.
  void AddFromMeta(std::string_view value);

  bool Empty() const { return policies_.empty(); }
  std::size_t Size() const { return policies_.size(); }

  // Allowed only when *every* policy allows.
  bool AllowsUrl(Directive directive, const url::Url& target, const url::Origin& self,
                 std::string_view nonce = std::string_view()) const;
  bool AllowsInline(Directive directive, std::string_view nonce, std::string_view body) const;
  // Whether any policy governs `directive`. The engine asks this first, so a
  // document with no policy pays a bool test rather than a URL parse.
  bool Governs(Directive directive) const;

 private:
  std::vector<Policy> policies_;
};

}  // namespace microbrowser::csp

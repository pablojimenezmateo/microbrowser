#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "csp/ContentSecurityPolicy.h"
#include "url/Origin.h"
#include "url/Url.h"

namespace microbrowser::engine {

// This document's Content-Security-Policy, plus the two things every question
// about one needs: the origin `'self'` names, and the base its relative URLs
// resolve against.
//
// A type rather than three members on Page, because a CSP question is asked
// from four places -- collecting the stylesheets, collecting the scripts,
// starting a subresource, and starting a `fetch` -- and each of them would
// otherwise have to resolve the URL itself. Four resolutions of one string is
// four chances to disagree, and a disagreement here is a resource that passed
// the check under one base and is fetched under another.
//
// The base is also where `<base href>` lands, which is why `base-uri` is
// enforced *inside* SetBase rather than by its caller: a `<base>` the policy
// refuses must leave the base where it was, and a caller that checked first and
// set second could forget the first half.
class DocumentPolicy {
 public:
  // Replaces everything: the policies from the response headers, and the
  // document's own address. Called once per document, before anything asks.
  void Reset(csp::PolicyList policies, std::string_view document_url);

  // Appends a policy the document delivered itself, in a
  // `<meta http-equiv="Content-Security-Policy">`. After Reset, because a
  // `<meta>` can only ever add a policy -- every policy in the list must allow,
  // so a page cannot use one to loosen the header its server sent.
  void AddFromMeta(std::string_view value);

  // Moves the base URL to `href`, resolved against the document's address.
  // Refused -- leaving the base alone -- when `base-uri` says so, or when the
  // result does not parse. True when it moved.
  bool SetBase(std::string_view href);

  // What a relative URL in this document resolves against: `<base href>` when
  // there is one and the document's own address otherwise. Nothing when the
  // address itself does not parse, which is a `data:` or `about:` document.
  const std::optional<url::Url>& Base() const { return base_; }

  bool Governs(csp::Directive directive) const { return policies_.Governs(directive); }

  // Whether the policy permits `written_url` -- a URL exactly as the document
  // or a script wrote it -- for this purpose. True when nothing governs the
  // directive, which is the common case and costs a bool test. A URL that does
  // not resolve is refused: a fetch of it would fail anyway, and answering
  // "allowed" about a URL nobody could parse is an answer about nothing.
  bool AllowsUrl(csp::Directive directive, std::string_view written_url,
                 std::string_view nonce = std::string_view()) const;

  bool AllowsInline(csp::Directive directive, std::string_view nonce,
                    std::string_view body) const {
    return policies_.AllowsInline(directive, nonce, body);
  }

  // What was refused, in the order it was refused, for the console. Local and
  // never sent: a violation report is an outbound request the user did not
  // cause. See csp/ContentSecurityPolicy.h.
  const std::vector<std::string>& Violations() const { return violations_; }

 private:
  void Record(csp::Directive directive, std::string_view what) const;

  csp::PolicyList policies_;
  url::Origin self_;
  std::optional<url::Url> base_;
  // Mutable so that answering a question can record having refused one. The
  // alternative is every caller remembering to log, at eight call sites, which
  // is how a browser ends up enforcing silently.
  mutable std::vector<std::string> violations_;
};

}  // namespace microbrowser::engine

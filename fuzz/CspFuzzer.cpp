#include <cstddef>
#include <cstdint>
#include <string_view>

#include "csp/ContentSecurityPolicy.h"
#include "url/Origin.h"
#include "url/Url.h"

// A Content-Security-Policy header, written by whoever served the page.
//
// The bytes are a server's and the questions asked of the result are a page's,
// which makes this the same shape as the CORS fuzzer: there is no buffer here
// to overrun, so "does it crash" is the cheap half. The property that matters
// is the one an assertion can state --
//
//   **a policy that governs a directive never allows more than the same policy
//   with an extra source list appended.**
//
// Adding a second policy can only ever remove permission, because every policy
// in the list must allow. A parse bug that made a malformed token widen a
// source list -- an unterminated quote read as a host, a port that wrapped,
// a `*.` prefix that matched its own domain -- shows up as a policy that says
// yes where a stricter one said no, and that is checkable without knowing what
// the right answer was.
//
// The second property is the one `'none'` depends on: a directive whose source
// list did not parse must still *govern*. A token this browser cannot read
// becomes a source that matches nothing, so an unreadable policy is stricter
// than an absent one and never looser.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  using namespace microbrowser;

  const std::string_view text(reinterpret_cast<const char*>(data), size);

  const auto self_url = url::Url::Parse("https://page.example/dir/doc.html");
  if (!self_url.has_value()) {
    return 0;
  }
  const url::Origin self = url::Origin::FromUrl(*self_url);

  static constexpr std::string_view kTargets[] = {
      "https://page.example/script.js", "http://page.example/script.js",
      "https://cdn.page.example:8443/a/b", "https://evil.example/x",
      "data:text/javascript,0",           "blob:https://page.example/1",
  };
  static constexpr csp::Directive kDirectives[] = {
      csp::Directive::Script, csp::Directive::Style,      csp::Directive::Img,
      csp::Directive::Connect, csp::Directive::FormAction, csp::Directive::BaseUri,
  };

  csp::PolicyList one;
  one.AddFromHeader(text);

  // The same policies plus one that allows nearly everything. It has to be
  // permissive, or the property below would hold for the trivial reason that
  // the second policy refuses every question.
  csp::PolicyList two;
  two.AddFromHeader(text);
  two.AddFromHeader(
      "default-src * data: blob: 'unsafe-inline'; form-action *; base-uri *");

  for (const csp::Directive directive : kDirectives) {
    for (const std::string_view target_text : kTargets) {
      const auto target = url::Url::Parse(target_text);
      if (!target.has_value()) {
        continue;
      }
      const bool allowed_by_one = one.AllowsUrl(directive, *target, self, "nonce-value");
      const bool allowed_by_two = two.AllowsUrl(directive, *target, self, "nonce-value");
      if (allowed_by_two && !allowed_by_one) {
        __builtin_trap();  // a second policy widened the first
      }
    }
    const bool inline_one = one.AllowsInline(directive, "nonce-value", "alert(1)");
    const bool inline_two = two.AllowsInline(directive, "nonce-value", "alert(1)");
    if (inline_two && !inline_one) {
      __builtin_trap();
    }
    if (!one.Governs(directive) && !one.AllowsUrl(directive, *self_url, self)) {
      // A directive nobody governs allows everything. `Governs` is what the
      // engine tests before it does any work at all, so the two answering
      // about different tables would be a policy enforced where none exists.
      __builtin_trap();
    }
  }

  // A `<meta>`-delivered policy takes the same bytes down a different entry
  // point, and must not be able to parse into something the header form could
  // not produce.
  csp::PolicyList meta;
  meta.AddFromMeta(text);
  (void)meta.Governs(csp::Directive::Script);
  (void)meta.AllowsInline(csp::Directive::Script, "", text);
  return 0;
}

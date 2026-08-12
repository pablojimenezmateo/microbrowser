// Content-Security-Policy, the parser and the decision.
//
// ADR 0020 §3. The cases here are the ones where being wrong is a security bug
// rather than a rendering bug: a nonce that matches something it should not, a
// wildcard that reaches `data:`, `'unsafe-inline'` still applying next to a
// nonce, and `form-action` falling back to `default-src` when it must not.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "csp/ContentSecurityPolicy.h"
#include "url/Origin.h"
#include "url/Url.h"

namespace microbrowser::tests {

namespace {

using csp::Directive;
using csp::PolicyList;

url::Origin OriginOf(std::string_view text) {
  const std::optional<url::Url> url = url::Url::Parse(text);
  Expect(url.has_value(), "the test's own origin URL must parse");
  return url::Origin::FromUrl(*url);
}

url::Url UrlOf(std::string_view text) {
  const std::optional<url::Url> url = url::Url::Parse(text);
  Expect(url.has_value(), "the test's own target URL must parse");
  return *url;
}

PolicyList Policies(std::string_view header) {
  PolicyList list;
  list.AddFromHeader(header);
  return list;
}

bool AllowsScript(std::string_view header, std::string_view target,
                  std::string_view nonce = {}) {
  return Policies(header).AllowsUrl(Directive::Script, UrlOf(target),
                                    OriginOf("https://page.example/a/b"), nonce);
}

}  // namespace

void RegisterCspTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Csp/ADocumentWithNoPolicyGovernsNothing", [] {
    const PolicyList empty;
    Expect(!empty.Governs(Directive::Script), "no policy governs nothing");
    Expect(empty.AllowsUrl(Directive::Script, UrlOf("https://evil.example/x"),
                           OriginOf("https://page.example/")),
           "and allows everything");
    // A header made entirely of directives this browser does not enforce is
    // the same as no header: `report-to` must not make a page act as if it had
    // a policy.
    const PolicyList reports = Policies("report-to csp-endpoint; report-uri /csp");
    Expect(reports.Empty(), "a policy of nothing but reporting is no policy");
  });

  AddTest(tests, "Csp/SelfIsTheDocumentsOriginAndNotItsHost", [] {
    Expect(AllowsScript("script-src 'self'", "https://page.example/lib.js"),
           "same origin");
    Expect(!AllowsScript("script-src 'self'", "https://other.example/lib.js"),
           "a different host is not self");
    Expect(!AllowsScript("script-src 'self'", "https://page.example:8443/lib.js"),
           "a different port is not self");
    Expect(!AllowsScript("script-src 'self'", "https://sub.page.example/lib.js"),
           "a subdomain is not self");
  });

  AddTest(tests, "Csp/AHostSourceMatchesASubdomainOnlyThroughAWildcard", [] {
    Expect(AllowsScript("script-src cdn.example", "https://cdn.example/a.js"),
           "a bare host, scheme taken from the document");
    Expect(!AllowsScript("script-src cdn.example", "https://x.cdn.example/a.js"),
           "and not its subdomains");
    Expect(AllowsScript("script-src *.cdn.example", "https://x.cdn.example/a.js"),
           "which is what the wildcard is for");
    Expect(!AllowsScript("script-src *.cdn.example", "https://cdn.example/a.js"),
           "and the wildcard is not a shorter way to write the bare host");
    Expect(AllowsScript("script-src https://cdn.example:8443", "https://cdn.example:8443/a.js"),
           "an explicit port");
    Expect(!AllowsScript("script-src https://cdn.example:8443", "https://cdn.example/a.js"),
           "which is not the default one");
    Expect(AllowsScript("script-src https://cdn.example:*", "https://cdn.example:1234/a.js"),
           "and `:*` is any port");
  });

  AddTest(tests, "Csp/APathInASourceIsAPrefixOnlyWhenItEndsInASlash", [] {
    Expect(AllowsScript("script-src https://cdn.example/lib/", "https://cdn.example/lib/a.js"),
           "a trailing slash is a prefix");
    Expect(!AllowsScript("script-src https://cdn.example/lib/", "https://cdn.example/other/a.js"),
           "and only that prefix");
    Expect(AllowsScript("script-src https://cdn.example/lib/a.js", "https://cdn.example/lib/a.js"),
           "no trailing slash is an exact path");
    Expect(!AllowsScript("script-src https://cdn.example/lib/a.js", "https://cdn.example/lib/b.js"),
           "and only that path");
  });

  AddTest(tests, "Csp/AWildcardIsEveryNetworkSchemeAndNotData", [] {
    Expect(AllowsScript("script-src *", "https://anything.example/a.js"), "https");
    Expect(AllowsScript("script-src *", "http://anything.example/a.js"), "http");
    // The one that matters: `img-src *` allowing `data:` would make
    // `data:image/svg+xml,<svg onload=…>` a policy-approved image.
    Expect(!AllowsScript("script-src *", "data:text/javascript,alert(1)"),
           "`*` is not `data:`");
    Expect(AllowsScript("script-src data:", "data:text/javascript,alert(1)"),
           "which a page has to name if it means it");
  });

  AddTest(tests, "Csp/AnHttpSourceMatchesTheHttpsUpgradeAndNotTheReverse", [] {
    PolicyList list;
    list.AddFromHeader("script-src http://cdn.example");
    Expect(list.AllowsUrl(Directive::Script, UrlOf("https://cdn.example/a.js"),
                          OriginOf("https://page.example/")),
           "a policy written before a CDN moved to TLS must not stop it moving");
    PolicyList secure;
    secure.AddFromHeader("script-src https://cdn.example");
    Expect(!secure.AllowsUrl(Directive::Script, UrlOf("http://cdn.example/a.js"),
                             OriginOf("https://page.example/")),
           "and the downgrade is not allowed");
  });

  AddTest(tests, "Csp/OnlyUnsafeInlineCompilesAnEventHandlerAttribute", [] {
    // An `on*` content attribute is the one inline script that only
    // `'unsafe-inline'` can permit: it carries no nonce and CSP never hashes
    // it. So this is its own question rather than `AllowsInline` with two
    // empty strings, and the two differ in the case that matters.
    Expect(Policies("").AllowsInlineHandler(),
           "no policy at all: a handler runs, as it does on any page");
    Expect(Policies("img-src 'none'").AllowsInlineHandler(),
           "a policy that says nothing about script says nothing about handlers");
    Expect(!Policies("script-src 'self'").AllowsInlineHandler(),
           "a script-src without 'unsafe-inline' refuses one");
    Expect(!Policies("default-src 'self'").AllowsInlineHandler(),
           "and default-src is the fallback, as it is everywhere else");
    Expect(Policies("script-src 'unsafe-inline'").AllowsInlineHandler(),
           "'unsafe-inline' is what permits it");
    // **The one case the general form gets wrong.** A nonce cancels
    // `'unsafe-inline'` for a `<script>` element -- that is how a modern
    // policy stays safe on a browser that understands nonces while staying
    // usable on one that does not. An attribute has nowhere to put a nonce, so
    // the cancellation cannot apply to it, and every engine agrees.
    const std::string_view both = "script-src 'unsafe-inline' 'nonce-abc'";
    Expect(Policies(both).AllowsInlineHandler(),
           "a nonce beside 'unsafe-inline' still permits a handler");
    Expect(!Policies(both).AllowsInline(Directive::Script, "", "alert(1)"),
           "while the same policy refuses an inline <script> without the nonce");
    // Every policy in force has to allow, like every other question here.
    PolicyList two;
    two.AddFromHeader("script-src 'unsafe-inline'");
    two.AddFromHeader("script-src 'self'");
    Expect(!two.AllowsInlineHandler(), "two policies, and both must allow");
  });

  AddTest(tests, "Csp/ANonceAllowsInlineAndExternalAndNothingElse", [] {
    const std::string_view header = "script-src 'nonce-abc123'";
    Expect(Policies(header).AllowsInline(Directive::Script, "abc123", "alert(1)"),
           "the matching nonce runs the inline script");
    Expect(!Policies(header).AllowsInline(Directive::Script, "wrong", "alert(1)"),
           "and a different one does not");
    Expect(!Policies(header).AllowsInline(Directive::Script, "", "alert(1)"),
           "and no nonce at all does not");
    // CSP2's change, and what reddit's front page depends on: a nonce on a
    // `<script src>` allows the fetch too.
    Expect(AllowsScript(header, "https://evil.example/x.js", "abc123"),
           "a nonce allows the external script it is written on");
    Expect(!AllowsScript(header, "https://evil.example/x.js"),
           "and an injected script without one is refused");
    // A nonce is a secret, so it is compared case-sensitively.
    Expect(!Policies(header).AllowsInline(Directive::Script, "ABC123", "alert(1)"),
           "a nonce is compared byte for byte");
  });

  AddTest(tests, "Csp/UnsafeInlineIsIgnoredNextToANonce", [] {
    Expect(Policies("script-src 'unsafe-inline'")
               .AllowsInline(Directive::Script, "", "alert(1)"),
           "on its own it allows an inline script");
    // The whole mechanism by which a modern policy stays safe on a browser that
    // understands nonces and usable on one that does not.
    Expect(!Policies("script-src 'unsafe-inline' 'nonce-abc'")
                .AllowsInline(Directive::Script, "", "alert(1)"),
           "beside a nonce it means nothing");
    Expect(Policies("script-src 'unsafe-inline' 'nonce-abc'")
               .AllowsInline(Directive::Script, "abc", "alert(1)"),
           "and the nonce still works");
    Expect(!AllowsScript("script-src 'unsafe-inline'", "https://page.example/a.js"),
           "`'unsafe-inline'` says nothing about a URL");
  });

  AddTest(tests, "Csp/AHashSourceMatchesTheContentAndNotItsNeighbour", [] {
    // sha256 of "alert(1)", base64. Computed independently, by:
    //   import hashlib, base64
    //   base64.b64encode(hashlib.sha256(b'alert(1)').digest())
    const std::string_view header =
        "script-src 'sha256-bhHHL3z2vDgxUt0W3dWQOrprscmda2Y5pLsLg4GF+pI='";
    Expect(Policies(header).AllowsInline(Directive::Script, "", "alert(1)"),
           "the hashed body runs");
    Expect(!Policies(header).AllowsInline(Directive::Script, "", "alert(2)"),
           "and one byte different does not");
    // A hash-source suppresses `'unsafe-inline'` exactly as a nonce does.
    PolicyList both;
    both.AddFromHeader(
        "script-src 'unsafe-inline' 'sha256-bhHHL3z2vDgxUt0W3dWQOrprscmda2Y5pLsLg4GF+pI='");
    Expect(!both.AllowsInline(Directive::Script, "", "alert(2)"),
           "a hash suppresses 'unsafe-inline' too");
  });

  AddTest(tests, "Csp/UnsafeEvalGatesStringToCode", [] {
    Expect(Policies("img-src 'none'").AllowsEval(),
           "a policy that never mentions script leaves eval alone");
    Expect(!Policies("script-src 'self'").AllowsEval(),
           "script-src without 'unsafe-eval' forbids eval");
    Expect(Policies("script-src 'self' 'unsafe-eval'").AllowsEval(),
           "and 'unsafe-eval' restores it");
    Expect(!Policies("default-src 'none'").AllowsEval(),
           "default-src without 'unsafe-eval' forbids it too");
    Expect(Policies("default-src 'none' 'unsafe-eval'").AllowsEval(),
           "default-src can carry 'unsafe-eval'");
    Expect(!Policies("default-src 'none' 'unsafe-eval'; script-src 'self'").AllowsEval(),
           "an explicit script-src without the keyword still forbids");
    Expect(!Policies("script-src 'self' 'unsafe-eval', default-src 'none'").AllowsEval(),
           "every policy in the list must allow");
  });

  AddTest(tests, "Csp/AnUnparseableTokenIsStricterAndNeverLooser", [] {
    Expect(Policies("script-src 'strict-dynamic' 'nonce-abc'")
               .AllowsInline(Directive::Script, "abc", "x"),
           "the tokens we understood still work");
    Expect(Policies("script-src 'strict-dynamic' 'nonce-abc'").ScriptStrictDynamic(),
           "strict-dynamic is recognized on script-src");
    Expect(!AllowsScript("script-src 'strict-dynamic' 'nonce-abc'",
                         "https://evil.example/x.js"),
           "host allowlists do not authorize scripts when strict-dynamic is on");
    Expect(!AllowsScript("script-src ''''", "https://page.example/a.js"),
           "nonsense matches nothing");
    // A directive with an empty source list allows nothing, which is `'none'`.
    Expect(Policies("script-src").Governs(Directive::Script),
           "an empty source list still governs");
    Expect(!AllowsScript("script-src", "https://page.example/a.js"),
           "and allows nothing");
    Expect(!AllowsScript("script-src 'none'", "https://page.example/a.js"),
           "as does 'none'");
  });

  AddTest(tests, "Csp/AnUnknownDirectiveDoesNotFailThePolicy", [] {
    // Failing the whole policy closed over a directive nobody has implemented
    // is how a browser breaks pages it has never seen.
    Expect(AllowsScript("sandbox allow-scripts; script-src 'self'",
                        "https://page.example/a.js"),
           "the directives we know still apply");
    Expect(!AllowsScript("sandbox allow-scripts; script-src 'self'",
                         "https://evil.example/a.js"),
           "and still refuse");
  });

  AddTest(tests, "Csp/ScriptSrcFallsBackToDefaultSrcAndFormActionDoesNot", [] {
    Expect(!AllowsScript("default-src 'self'", "https://evil.example/a.js"),
           "script-src falls back to default-src");
    Expect(AllowsScript("default-src 'none'; script-src 'self'",
                        "https://page.example/a.js"),
           "and an explicit script-src replaces it entirely");
    // The rule people are most often surprised by, and it is the
    // specification's: `default-src 'none'` does not stop a form submission.
    const PolicyList list = Policies("default-src 'none'");
    Expect(!list.Governs(Directive::FormAction), "form-action does not fall back");
    Expect(list.AllowsUrl(Directive::FormAction, UrlOf("https://evil.example/post"),
                          OriginOf("https://page.example/")),
           "so the submission is allowed");
    Expect(!list.Governs(Directive::BaseUri), "and neither does base-uri");
  });

  AddTest(tests, "Csp/ADirectiveGivenTwiceKeepsTheFirst", [] {
    Expect(!AllowsScript("script-src 'self'; script-src *", "https://evil.example/a.js"),
           "the second copy cannot loosen the first");
  });

  AddTest(tests, "Csp/TwoPoliciesBothHaveToAllow", [] {
    PolicyList list;
    list.AddFromHeader("script-src https://a.example https://b.example");
    list.AddFromHeader("script-src https://b.example");
    ExpectEqInt(static_cast<long long>(list.Size()), 2, "two headers are two policies");
    Expect(list.AllowsUrl(Directive::Script, UrlOf("https://b.example/x.js"),
                          OriginOf("https://page.example/")),
           "what both allow is allowed");
    Expect(!list.AllowsUrl(Directive::Script, UrlOf("https://a.example/x.js"),
                           OriginOf("https://page.example/")),
           "what only one allows is not");
    // One header may carry several policies, comma-separated, and they are not
    // merged either.
    PolicyList comma;
    comma.AddFromHeader("script-src https://a.example, script-src https://b.example");
    ExpectEqInt(static_cast<long long>(comma.Size()), 2, "a comma separates two policies");
    Expect(!comma.AllowsUrl(Directive::Script, UrlOf("https://a.example/x.js"),
                            OriginOf("https://page.example/")),
           "and neither of them alone is enough");
  });

  AddTest(tests, "Csp/AnOpaqueOriginHasNothingForSelfToName", [] {
    PolicyList list;
    list.AddFromHeader("script-src 'self'");
    // A `data:` document. `'self'` naming nothing is the specification's
    // answer, and the alternative -- treating it as "any" -- would make a
    // policy on a data URL meaningless.
    Expect(!list.AllowsUrl(Directive::Script, UrlOf("https://page.example/a.js"),
                           url::Origin{}),
           "'self' matches nothing from an opaque origin");
  });
}

}  // namespace microbrowser::tests

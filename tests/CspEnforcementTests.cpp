// Content-Security-Policy where it is actually enforced: a document served with
// one, and the resources it does and does not go on to request.
//
// The unit tests for the policy itself are in CspTests.cpp. These are the ones
// that would still pass if the parser were perfect and nothing consulted it --
// which is the failure mode ADR 0020 §3 names when it says "enforced, not
// logged". Every assertion here is about a request the server did or did not
// see, or a script that did or did not run.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
  }
};

// A 200 response with whatever extra header lines a test wants. Separate from
// OkResponse because the whole subject here is a header, and a helper that
// could not set one would be testing something else.
std::string ResponseWithHeaders(std::string_view extra_headers, std::string_view body) {
  std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
  response += extra_headers;
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  response += body;
  return response;
}

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  // Anything the document might go on to ask for, from any host. The exchanges
  // are claimed in order rather than matched, so `expected_host` is left empty
  // deliberately: which host a resource came from is not what these tests are
  // about, and pinning it would make a refusal and a missing fixture look the
  // same.
  void ServeSubresources() {
    for (int i = 0; i < 8; ++i) {
      factory.script.push_back(ScriptedTransport::Exchange{
          "", 443, true, OkResponse("text/javascript", "console.log('ran')")});
    }
  }

  void Load(std::string_view headers, std::string_view html) {
    factory.script.insert(factory.script.begin(),
                          ScriptedTransport::Exchange{"page.example", 443, true,
                                                      ResponseWithHeaders(headers, html)});
    ServeSubresources();
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    for (int turn = 0; turn < kMaxDriveTurns; ++turn) {
      const bool advanced = engine.Advance();
      const bool due = engine.RunDueWork();
      if (!advanced && !due && !engine.HasRunnableWork()) {
        break;
      }
    }
  }

  // Whether any request line the server saw mentions `needle`. The document's
  // own request is one of them, so a test asks about a path.
  bool Requested(std::string_view needle) const {
    for (const std::string& request : factory.log.requests) {
      if (request.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  std::string Console() const {
    std::string joined;
    for (const std::string& line : engine.ConsoleOutput()) {
      joined += joined.empty() ? "" : "|";
      joined += line;
    }
    return joined;
  }
};

}  // namespace

void RegisterCspEnforcementTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CspEnforcement/ANoncedInlineScriptRunsAndAnInjectedOneDoesNot", [] {
    // www.reddit.com's actual header, copied verbatim on 2026-08-05 with only
    // the nonce's random part changed. The second script is what an injection
    // looks like: same document, same origin, no nonce.
    Session session;
    session.Load(
        "Content-Security-Policy: default-src 'none'; "
        "script-src 'nonce-fbaef209-b0dd-4c7e-ba73-f3fffe633ee7'; style-src 'unsafe-inline'; "
        "img-src https://www.redditstatic.com; form-action 'self';\r\n",
        "<html><body>"
        "<script nonce=\"fbaef209-b0dd-4c7e-ba73-f3fffe633ee7\">console.log('nonced')</script>"
        "<script>console.log('injected')</script>"
        "<style>body { color: red }</style>"
        "<img src=\"https://www.redditstatic.com/ok.png\">"
        "<img src=\"https://evil.example/no.png\">"
        "</body></html>");
    ExpectEqString(session.Console(), "nonced",
                   "the nonced script ran and the injected one did not");
    // The other three directives of the same header, so that the whole of what
    // reddit asks for is asserted in one place rather than inferred.
    Expect(session.Requested("/ok.png"), "img-src names redditstatic, so that one loads");
    Expect(!session.Requested("/no.png"), "and nothing else does");
  });

  AddTest(tests, "CspEnforcement/AnInlineEventHandlerNeedsUnsafeInline", [] {
    // An `on*` **content attribute** is inline script, and it is the one kind
    // only `'unsafe-inline'` can permit: an attribute carries no nonce and CSP
    // never hashes one. The driver is an *external* same-origin script in both
    // halves, so what differs between them is the handler and nothing else --
    // an inline `<script>` would be refused by the second policy too and the
    // test would pass for the wrong reason.
    const std::string_view markup =
        "<html><body><div id=\"d\" onclick=\"console.log('handler ran')\"></div>"
        "<script src=\"/drive.js\"></script></body></html>";
    const std::string_view driver =
        "document.getElementById('d').dispatchEvent(new Event('click'))";
    {
      Session session;
      session.factory.script.push_back(ScriptedTransport::Exchange{
          "", 443, true, OkResponse("text/javascript", std::string(driver))});
      session.Load("Content-Security-Policy: script-src 'self' 'unsafe-inline'\r\n", markup);
      ExpectEqString(session.Console(), "handler ran",
                     "'unsafe-inline' is what compiles an onclick attribute");
    }
    {
      Session session;
      session.factory.script.push_back(ScriptedTransport::Exchange{
          "", 443, true, OkResponse("text/javascript", std::string(driver))});
      session.Load("Content-Security-Policy: script-src 'self'\r\n", markup);
      ExpectEqString(session.Console(), "",
                     "and without it the attribute is never compiled -- the event still "
                     "dispatches, and reaches nothing");
    }
  });

  AddTest(tests, "CspEnforcement/ARefusedScriptIsNeverRequested", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'self'\r\n",
                 "<html><body>"
                 "<script src=\"/local.js\"></script>"
                 "<script src=\"https://cdn.example/remote.js\"></script>"
                 "</body></html>");
    Expect(session.Requested("/local.js"), "the allowed script was fetched");
    Expect(!session.Requested("/remote.js"),
           "and the refused one never became a request -- which is what "
           "enforcement means rather than a script that is fetched and dropped");
    ExpectEqString(session.Console(), "ran", "only the allowed one ran");
  });

  AddTest(tests, "CspEnforcement/ANonceAllowsAnExternalScriptToo", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'nonce-abc'\r\n",
                 "<html><body>"
                 "<script nonce=\"abc\" src=\"https://cdn.example/remote.js\"></script>"
                 "<script src=\"https://cdn.example/other.js\"></script>"
                 "</body></html>");
    Expect(session.Requested("/remote.js"), "a nonce allows the fetch, per CSP2");
    Expect(!session.Requested("/other.js"), "and the one without it is refused");
  });

  AddTest(tests, "CspEnforcement/ATrustedDynamicScriptIsFetchedWithoutANonce", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'nonce-abc'\r\n",
                 "<html><body>"
                 "<script nonce=\"abc\">"
                 "const s = document.createElement('script');"
                 "s.src = 'https://cdn.example/late.js';"
                 "document.head.appendChild(s);"
                 "</script></body></html>");
    Expect(session.Requested("/late.js"),
           "a script a trusted script inserted is not refused for lacking a nonce");
    ExpectEqString(session.Console(), "ran", "and it ran when it arrived");
  });

  AddTest(tests, "CspEnforcement/StrictDynamicAllowsConcatOnDomContentLoaded", [] {
    Session session;
    session.Load(
        "Content-Security-Policy: script-src 'strict-dynamic' 'nonce-abc'\r\n",
        "<html><head>"
        "<script nonce=\"abc\">"
        "document.addEventListener('DOMContentLoaded', function() {"
        "  const s = document.createElement('script');"
        "  s.src = 'https://cdn.example/concat.js';"
        "  document.body.appendChild(s);"
        "});"
        "</script></head><body></body></html>");
    Expect(session.Requested("/concat.js"),
           "strict-dynamic lets DOMContentLoaded insert a script without a nonce");
    ExpectEqString(session.Console(), "ran", "and it ran when it arrived");
  });

  AddTest(tests, "CspEnforcement/ARefusedStylesheetIsNeitherFetchedNorApplied", [] {
    Session session;
    session.Load("Content-Security-Policy: style-src 'none'\r\n",
                 "<html><head>"
                 "<link rel=\"stylesheet\" href=\"/site.css\">"
                 "<style>body { color: rgb(1, 2, 3) }</style>"
                 "</head><body>text"
                 "<script>console.log(getComputedStyle(document.body).color)</script>"
                 "</body></html>");
    Expect(!session.Requested("/site.css"), "the linked sheet was not requested");
    // The inline sheet not being *applied* is the other half, asked through the
    // cascade rather than the log. `script-src` is absent and does not fall back
    // to a `style-src`, so the script that asks still runs.
    Expect(session.Console() != "rgb(1, 2, 3)",
           "and the inline sheet was not applied: " + session.Console());
  });

  AddTest(tests, "CspEnforcement/ARefusedImageIsNeverRequested", [] {
    Session session;
    session.Load("Content-Security-Policy: img-src 'self'\r\n",
                 "<html><body>"
                 "<img src=\"/local.png\">"
                 "<img src=\"https://cdn.example/remote.png\">"
                 "</body></html>");
    Expect(session.Requested("/local.png"), "the allowed image was fetched");
    Expect(!session.Requested("/remote.png"), "and the refused one was not");
  });

  AddTest(tests, "CspEnforcement/APromiseContinuationInsertsAScriptWithoutANonce", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'nonce-abc'\r\n",
                 "<html><body>"
                 "<script nonce=\"abc\">"
                 "Promise.resolve(0).then(function() {"
                 "  const s = document.createElement('script');"
                 "  s.src = 'https://cdn.example/after-promise.js';"
                 "  document.body.appendChild(s);"
                 "});"
                 "</script></body></html>");
    Expect(session.Requested("/after-promise.js"),
           "a promise continuation from a permitted script may insert another");
    ExpectEqString(session.Console(), "ran", "and it ran when it arrived");
  });

  AddTest(tests, "CspEnforcement/AFetchContinuationInsertsAScriptWithoutANonce", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'nonce-abc'\r\n",
                 "<html><body>"
                 "<script nonce=\"abc\">"
                 "fetch('/late.js').then(function() {"
                 "  console.log('in-then');"
                 "  const s = document.createElement('script');"
                 "  s.src = '/after-fetch.js';"
                 "  document.body.appendChild(s);"
                 "});"
                 "</script></body></html>");
    Expect(session.Requested("/late.js"), "the fetch went out");
    Expect(session.Requested("/after-fetch.js"),
           "a fetch continuation from a permitted script may insert another");
    ExpectEqString(session.Console(), "in-then|ran", "both scripts ran when they arrived");
  });

  AddTest(tests, "CspEnforcement/ASetTimeoutContinuationInsertsAScriptWithoutANonce", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'nonce-abc'\r\n",
                 "<html><body>"
                 "<script nonce=\"abc\">"
                 "setTimeout(function() {"
                 "  const s = document.createElement('script');"
                 "  s.src = 'https://cdn.example/after-timeout.js';"
                 "  document.body.appendChild(s);"
                 "}, 0);"
                 "</script></body></html>");
    Expect(session.Requested("/after-timeout.js"),
           "a timer continuation from a permitted script may insert another");
    ExpectEqString(session.Console(), "ran", "and it ran when it arrived");
  });

  AddTest(tests, "CspEnforcement/ConnectSrcStopsAPagesOwnFetch", [] {
    Session session;
    session.Load("Content-Security-Policy: connect-src 'self'\r\n",
                 "<html><body><script>"
                 "fetch('https://cdn.example/api').then(function () { console.log('ok') },"
                 "  function (e) { console.log('rejected ' + e.name) });"
                 "</script></body></html>");
    Expect(!session.Requested("/api"), "the request never went out");
    ExpectEqString(session.Console(), "rejected TypeError",
                   "and the promise rejected the way a network failure does, "
                   "without telling the page which of the two it was");
  });

  AddTest(tests, "CspEnforcement/ConnectSrcDoesNotBlockADataFetch", [] {
    Session session;
    session.Load("Content-Security-Policy: connect-src 'self'\r\n",
                 "<html><body><script>"
                 "fetch('data:text/plain,ok').then(function (r) { return r.text(); })"
                 "  .then(function (t) { console.log(t); });"
                 "</script></body></html>");
    ExpectEqString(session.Console(), "ok",
                   "a data: URL is answered locally and is not a connect-src violation");
  });

  AddTest(tests, "CspEnforcement/AMetaPolicyGovernsTheDocumentThatCarriesIt", [] {
    Session session;
    session.Load("",
                 "<html><head>"
                 "<meta http-equiv=\"Content-Security-Policy\" "
                 "content=\"script-src 'nonce-m'\">"
                 "</head><body>"
                 "<script nonce=\"m\">console.log('nonced')</script>"
                 "<script>console.log('bare')</script>"
                 "</body></html>");
    ExpectEqString(session.Console(), "nonced", "a <meta> policy is enforced");
  });

  AddTest(tests, "CspEnforcement/AMetaPolicyCannotLoosenTheHeader", [] {
    Session session;
    session.Load("Content-Security-Policy: script-src 'none'\r\n",
                 "<html><head>"
                 "<meta http-equiv=\"Content-Security-Policy\" content=\"script-src *\">"
                 "</head><body><script>console.log('ran')</script></body></html>");
    ExpectEqString(session.Console(), "", "every policy has to allow, so a second cannot widen");
  });

  AddTest(tests, "CspEnforcement/ReportOnlyIsNeitherEnforcedNorSent", [] {
    Session session;
    session.Load("Content-Security-Policy-Report-Only: script-src 'none'; report-uri /csp\r\n",
                 "<html><body><script>console.log('ran')</script></body></html>");
    ExpectEqString(session.Console(), "ran", "report-only does not block");
    Expect(!session.Requested("/csp"),
           "and nothing is reported: a violation report is an outbound request "
           "the user did not cause");
  });

  AddTest(tests, "CspEnforcement/BaseHrefMovesWhatARelativeUrlMeans", [] {
    Session session;
    session.Load("",
                 "<html><head><base href=\"https://cdn.example/assets/\"></head>"
                 "<body><script src=\"app.js\"></script></body></html>");
    Expect(session.Requested("/assets/app.js"),
           "a relative script resolves against <base href>");
    ExpectEqString(session.Console(), "ran", "and the script it named ran");
  });

  AddTest(tests, "CspEnforcement/BaseUriRefusesTheBaseAndLeavesItWhereItWas", [] {
    Session session;
    session.Load("Content-Security-Policy: base-uri 'self'\r\n",
                 "<html><head><base href=\"https://cdn.example/assets/\"></head>"
                 "<body><script src=\"app.js\"></script></body></html>");
    Expect(!session.Requested("/assets/app.js"), "the refused base did not take effect");
    Expect(session.Requested("GET /app.js"),
           "and the script resolved against the document, where the base still was");
  });
}

}  // namespace microbrowser::tests

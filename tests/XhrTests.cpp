// `XMLHttpRequest`, over the same machinery `fetch` uses.
//
// ADR 0020 §1: one request path. The assertions worth reading are the ones that
// would fail if this were a *second* path -- that `connect-src` stops an XHR,
// that a navigation cancels one, and that the request the server sees is the
// same shape a `fetch` produces.

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

std::string PageRunning(std::string_view script) {
  std::string html = "<html><body><script>";
  html += script;
  html += "</script></body></html>";
  return html;
}

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  void Serve(std::string_view response) {
    factory.script.push_back(ScriptedTransport::Exchange{"", 443, true, std::string(response)});
  }

  void Run(std::string_view script, std::string_view headers = {}) {
    std::string document = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
    document += headers;
    const std::string html = PageRunning(script);
    document += "Content-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
    factory.script.insert(factory.script.begin(),
                          ScriptedTransport::Exchange{"page.example", 443, true, document});
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
  }

  std::string Console() const {
    std::string joined;
    for (const std::string& line : engine.ConsoleOutput()) {
      joined += joined.empty() ? "" : "|";
      joined += line;
    }
    return joined;
  }

  std::string Errors() const {
    std::string joined;
    for (const std::string& line : engine.ScriptErrors()) {
      joined += line;
      joined += "\n";
    }
    return joined;
  }
};

// A response a test asks for by name, so the assertions are about the XHR and
// not about HTTP framing.
std::string Json(std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-Custom: yes\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

}  // namespace

void RegisterXhrTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Xhr/EveryConstructorPublishesThePrototypeItsInstancesUse", [] {
    // **A class-wide assertion, not two instance ones.** `XMLHttpRequest` and
    // `Worker` each built a prototype, installed it on every instance, and
    // never put it on the constructor -- so `instanceof` was false and
    // `X.prototype` was undefined, while everything else about them worked.
    // youtube's bundle reads `XMLHttpRequest.prototype.fetch` to choose
    // between two transports and took a TypeError on `undefined.fetch`.
    //
    // Listing the constructors rather than testing one is the point: the two
    // that were wrong were the two that did not go through `MakeInterface`, so
    // the next hand-rolled one will be wrong the same way and this is what
    // says so.
    Session session;
    session.Run(
        "const named = ['XMLHttpRequest', 'Worker', 'Range', 'MessagePort', 'Event',"
        "               'Node', 'Element', 'HTMLElement', 'Document', 'EventTarget'];"
        "const broken = named.filter(name => {"
        "  const c = globalThis[name];"
        "  return typeof c !== 'function' || typeof c.prototype !== 'object' ||"
        "         c.prototype === null || c.prototype.constructor !== c;"
        "});"
        "console.log('broken: ' + broken.join(','));"
        // And the property that omission actually costs, both ways round.
        "console.log('xhr instanceof: ' + (new XMLHttpRequest() instanceof XMLHttpRequest));"
        "console.log('patchable: ' + (function () {"
        "  XMLHttpRequest.prototype.probe = 7; return new XMLHttpRequest().probe;"
        "})());");
    ExpectEqString(session.Console(),
                   "broken: |xhr instanceof: true|patchable: 7",
                   "every constructor's prototype is reachable from it");
  });

  AddTest(tests, "Xhr/DeliversAResponseThroughReadyStateAndLoad", [] {
    Session session;
    session.Serve(Json("{\"a\":1}"));
    session.Run(
        "var seen = [];"
        "var x = new XMLHttpRequest();"
        "x.onreadystatechange = function () { seen.push(x.readyState) };"
        "x.addEventListener('load', function () {"
        "  console.log(seen.join(','));"
        "  console.log(x.status + ' ' + x.statusText);"
        "  console.log(x.responseText);"
        "});"
        "x.open('GET', '/data.json');"
        "x.send();");
    ExpectEqString(session.Console(), "1,2,3,4|200 OK|{\"a\":1}",
                   "every readyState in order, then load. Errors: " + session.Errors());
  });

  AddTest(tests, "Xhr/ReadsTheHeadersItIsAllowedToRead", [] {
    Session session;
    session.Serve(Json("{}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "x.onload = function () {"
        "  console.log(x.getResponseHeader('X-Custom'));"
        "  console.log(String(x.getResponseHeader('nope')));"
        "  console.log(x.getAllResponseHeaders().indexOf('x-custom: yes') >= 0);"
        "};"
        "x.open('GET', '/data.json');"
        "x.send();");
    ExpectEqString(session.Console(), "yes|null|true",
                   "a header by name is case-insensitive, a missing one is null, and the "
                   "whole set is CRLF-separated. Errors: " + session.Errors());
  });

  AddTest(tests, "Xhr/SendsTheMethodBodyAndHeadersItWasGiven", [] {
    Session session;
    session.Serve(Json("{}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "x.open('POST', '/submit');"
        "x.setRequestHeader('X-Token', 'abc');"
        "x.send('name=value');");
    Expect(session.factory.log.requests.size() >= 2,
           "the document and the XHR. Errors: " + session.Errors());
    const std::string& request = session.factory.log.requests.at(1);
    Expect(request.rfind("POST /submit ", 0) == 0, "the method and path: " + request);
    Expect(request.find("x-token: abc") != std::string::npos ||
               request.find("X-Token: abc") != std::string::npos,
           "the header it set: " + request);
    Expect(request.find("name=value") != std::string::npos, "and the body: " + request);
  });

  AddTest(tests, "Xhr/AForbiddenHeaderIsDroppedRatherThanRefused", [] {
    Session session;
    session.Serve(Json("{}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "x.open('GET', '/data');"
        "x.setRequestHeader('Host', 'evil.example');"
        "x.send();"
        "console.log('no throw');");
    ExpectEqString(session.Console(), "no throw",
                   "a page that sets Host gets no error. Errors: " + session.Errors());
    const std::string& request = session.factory.log.requests.at(1);
    Expect(request.find("evil.example") == std::string::npos,
           "and no header either: " + request);
  });

  AddTest(tests, "Xhr/ResponseTypeJsonParsesAndAnythingElseIsIgnored", [] {
    Session session;
    session.Serve(Json("{\"a\":[1,2]}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "x.open('GET', '/data.json');"
        "x.responseType = 'json';"
        "console.log(x.responseType);"
        "x.responseType = 'arraybuffer';"
        "console.log('after arraybuffer: ' + x.responseType);"
        "x.onload = function () { console.log(x.response.a[1]) };"
        "x.send();");
    // The middle line is ADR 0012's rule where it is cheapest to get wrong: an
    // unsupported responseType is *ignored*, so it reads back as the last value
    // that took -- which is how a page detects that arraybuffer is unavailable
    // rather than being handed a string that pretends to be one.
    ExpectEqString(session.Console(), "json|after arraybuffer: json|2",
                   "Errors: " + session.Errors());
  });

  AddTest(tests, "Xhr/ASynchronousRequestThrowsRatherThanBlockingTheLoop", [] {
    Session session;
    session.Serve(Json("{}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "try { x.open('GET', '/data', false) } catch (e) { console.log(e.name) }");
    ExpectEqString(session.Console(), "InvalidAccessError",
                   "one loop, and a legible failure beats a lie. Errors: " + session.Errors());
  });

  AddTest(tests, "Xhr/SendBeforeOpenIsAnInvalidStateError", [] {
    Session session;
    session.Run(
        "var x = new XMLHttpRequest();"
        "try { x.send() } catch (e) { console.log(e.name) }"
        "try { x.setRequestHeader('a', 'b') } catch (e) { console.log(e.name) }");
    ExpectEqString(session.Console(), "InvalidStateError|InvalidStateError",
                   "Errors: " + session.Errors());
  });

  AddTest(tests, "Xhr/AbortCancelsTheRequestAndFiresAbortOnce", [] {
    Session session;
    session.Serve(Json("{}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "x.onabort = function () { console.log('abort ' + x.readyState) };"
        "x.onload = function () { console.log('load') };"
        "x.open('GET', '/data');"
        "x.send();"
        "x.abort();"
        "x.abort();"
        "console.log('state ' + x.readyState);");
    // Two things at once: the handler sees DONE, and the second abort fires
    // nothing -- a page's cleanup running twice is the bug this guards.
    ExpectEqString(session.Console(), "abort 4|state 0", "Errors: " + session.Errors());
  });

  AddTest(tests, "Xhr/ConnectSrcRefusesItBeforeItStarts", [] {
    Session session;
    session.Serve(Json("{}"));
    session.Run(
        "var x = new XMLHttpRequest();"
        "x.onerror = function () { console.log('error ' + x.status) };"
        "x.onloadend = function () { console.log('loadend') };"
        "x.open('GET', 'https://cdn.example/data');"
        "x.send();",
        "Content-Security-Policy: connect-src 'self'\r\n");
    ExpectEqString(session.Console(), "error 0|loadend",
                   "a policy refusal is a network error to the page, and it cannot tell "
                   "which. Errors: " + session.Errors());
    for (const std::string& request : session.factory.log.requests) {
      Expect(request.find("/data") == std::string::npos, "and nothing went out: " + request);
    }
  });

  AddTest(tests, "Xhr/IsAbsentWhenThereIsNoNetworkBehindTheBindings", [] {
    // The one property ADR 0012 cares about most, asked of a Page with no
    // NetworkSource: the constructor is not defined, so a page falls back
    // rather than getting an object that never answers.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(PageRunning("console.log(typeof XMLHttpRequest); console.log(typeof fetch);"),
              "https://page.example/");
    page.RunScripts(0);
    const std::vector<std::string>& output = page.ConsoleOutput();
    Expect(output.size() == 2, "both were asked about");
    ExpectEqString(output.at(0), "undefined", "no XMLHttpRequest");
    ExpectEqString(output.at(1), "undefined", "and no fetch, for the same reason");
  });
}

}  // namespace microbrowser::tests

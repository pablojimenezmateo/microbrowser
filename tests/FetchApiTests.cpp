// `fetch` as a page sees it: ADR 0020 §1.
//
// Driven through the whole engine rather than against the binding layer alone,
// because the thing worth asserting is the round trip -- a script asks, the
// request goes out through the privacy verdict and CORS, the answer comes back
// on a later turn of the loop, and the promise settles. A test that stubbed the
// middle would prove that a promise can be settled.
//
// The page says what it learned with `console.log`, which is the one channel a
// page has to the outside of this browser. `ScriptErrors` is the other half:
// without it a page whose every script threw and a page with no script at all
// look identical.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "gfx/Geometry.h"
#include "ipc/Message.h"
#include "ipc/Transport.h"
#include "engine/Engine.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

// A document that runs `script` and nothing else.
std::string PageRunning(std::string_view script) {
  std::string html = "<html><body><script>";
  html += script;
  html += "</script></body></html>";
  return html;
}

// A font stack with no system fonts in it, so that nothing here depends on
// which typefaces the machine has installed.
struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
  }
};

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  void Serve(std::string_view host, std::string_view response) {
    factory.script.push_back(
        ScriptedTransport::Exchange{std::string(host), 443, true, std::string(response)});
  }

  // Loads a document that runs `script`, and turns the crank until nothing is
  // outstanding -- which since ADR 0020 includes the requests the script itself
  // made, because a snapshot taken before those landed shows a page that looks
  // exactly like one whose fetch never worked.
  void Run(std::string_view script) {
    Start(script);
    RunEngineToIdle(engine);
  }

  // The same without the driving, for a test that has to act while something
  // is still outstanding.
  void Start(std::string_view script) {
    factory.script.insert(factory.script.begin(),
                          ScriptedTransport::Exchange{"page.example", 443, true,
                                                      OkResponse("text/html",
                                                                 PageRunning(script))});
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
  }

  void Pump(int turns) {
    for (int turn = 0; turn < turns; ++turn) {
      engine.Advance();
    }
  }

  std::string Console() const {
    std::string joined;
    for (const std::string& line : engine.ConsoleOutput()) {
      if (!joined.empty()) {
        joined += "|";
      }
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

std::string JsonResponse(std::string_view allow_origin, std::string_view body) {
  std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
  out += std::to_string(body.size());
  out += "\r\n";
  if (!allow_origin.empty()) {
    out += "Access-Control-Allow-Origin: ";
    out += allow_origin;
    out += "\r\n";
  }
  out += "\r\n";
  out += body;
  return out;
}

}  // namespace

void RegisterFetchApiTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Fetch/SameOriginRoundTrip", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "hello"));
    session.Run(
        "fetch('/data.txt').then(r => r.text()).then(t => console.log('got ' + t))"
        ".catch(e => console.log('threw ' + e.message));");
    ExpectEqString(session.Console(), "got hello",
                   "a script asked, the loop turned, and the promise settled with the body: "
                   + session.Errors());
  });

  AddTest(tests, "Fetch/StatusAndHeadersAreReadable", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "x"));
    session.Run(
        "fetch('/d').then(r => console.log(r.status + ' ' + r.ok + ' ' + "
        "r.headers.get('content-type') + ' ' + r.url));");
    ExpectEqString(session.Console(), "200 true text/plain https://page.example/d",
                   "status, ok, one header and the URL the bytes came from: " + session.Errors());
  });

  AddTest(tests, "Fetch/A404IsNotAFailure", [] {
    Session session;
    session.Serve("page.example",
                  "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nno!");
    session.Run(
        "fetch('/missing').then(r => console.log('settled ' + r.status + ' ' + r.ok))"
        ".catch(() => console.log('rejected'));");
    ExpectEqString(session.Console(), "settled 404 false",
                   "a 404 that arrived is a response: `ok` is about the status, and a page "
                   "that treats every settled fetch as success is the commonest bug in this "
                   "API -- which is why the two are separate");
  });

  AddTest(tests, "Fetch/JsonGoesThroughThePageOwnParser", [] {
    Session session;
    session.Serve("page.example", JsonResponse("", "{\"a\":[1,2]}"));
    session.Run("fetch('/d.json').then(r => r.json()).then(j => console.log(j.a[1]));");
    ExpectEqString(session.Console(), "2",
                   "a browser with two JSON parsers has two answers for a duplicate key, and "
                   "the one reached through fetch would be the one nobody tested: "
                   + session.Errors());
  });

  AddTest(tests, "Fetch/ABodyMayBeReadOnce", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "once"));
    session.Run(
        "fetch('/d').then(r => r.text().then(() => r.text()))"
        ".then(() => console.log('read twice'), e => console.log('refused: ' + e.message));");
    ExpectEqString(session.Console(), "refused: body already read",
                   "which is what a page written against a streaming implementation depends on");
  });

  AddTest(tests, "Fetch/ArrayBufferCarriesTheBytes", [] {
    Session session;
    session.Serve("page.example", OkResponse("application/octet-stream", "AB"));
    session.Run(
        "fetch('/d.bin').then(r => r.arrayBuffer()).then(b => {"
        "  const v = new Uint8Array(b);"
        "  console.log(b.byteLength + ' ' + v[0] + ' ' + v[1]);"
        "});");
    ExpectEqString(session.Console(), "2 65 66", "the bytes, not the string: " + session.Errors());
  });

  AddTest(tests, "Fetch/CrossOriginWithoutPermissionRejects", [] {
    Session session;
    session.Serve("api.example", OkResponse("text/plain", "secret"));
    session.Run(
        "fetch('https://api.example/d').then(r => r.text()).then("
        "  t => console.log('read ' + t), e => console.log('rejected ' + e.name));");
    ExpectEqString(session.Console(), "rejected TypeError",
                   "the response was discarded inside net, so there is nothing for the page "
                   "to have read");
  });

  AddTest(tests, "Fetch/CrossOriginWithPermissionResolves", [] {
    Session session;
    session.Serve("api.example", JsonResponse("https://page.example", "{\"ok\":1}"));
    session.Run(
        "fetch('https://api.example/d').then(r => r.json()).then(j => console.log('n=' + j.ok),"
        "  e => console.log('rejected ' + e.message));");
    ExpectEqString(session.Console(), "n=1", "the server named this origin: " + session.Errors());
  });

  AddTest(tests, "Fetch/NoCorsIsOpaqueRatherThanRefused", [] {
    Session session;
    session.Serve("cdn.example", OkResponse("text/plain", "pixels"));
    session.Run(
        "fetch('https://cdn.example/i', {mode: 'no-cors'}).then(r => r.text().then("
        "  t => console.log(r.type + ' ' + r.status + ' [' + t + ']')));");
    ExpectEqString(session.Console(), "opaque 0 []",
                   "status 0, no body, and nothing behind the type to read through: "
                   + session.Errors());
  });

  AddTest(tests, "Fetch/AbortRejectsAndStopsTheRequest", [] {
    Session session;
    // The abort runs in the same script turn as the fetch, so the request is
    // still queued when it arrives -- which is the case that matters: a browser
    // that only cancelled requests already on the wire would leave the queued
    // ones to run and answer nobody.
    session.Serve("page.example", OkResponse("text/plain", "late"));
    session.Run(
        "const c = new AbortController();"
        "fetch('/slow', {signal: c.signal}).then("
        "  () => console.log('resolved'), e => console.log('rejected ' + e.name));"
        "c.abort();"
        "console.log('aborted=' + c.signal.aborted);");
    ExpectEqString(session.Console(), "rejected AbortError|aborted=true",
                   "the promise rejected with the name every cancellable request tests for: "
                   + session.Errors());
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()), 1,
                "and the request never went out at all");
  });

  AddTest(tests, "Fetch/AnAlreadyAbortedSignalMakesNoRequest", [] {
    Session session;
    session.Run(
        "const c = new AbortController();"
        "c.abort();"
        "fetch('/never', {signal: c.signal}).then("
        "  () => console.log('resolved'), e => console.log('rejected ' + e.name));");
    ExpectEqString(session.Console(), "rejected AbortError", "rejected without a request");
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()), 1,
                "and only the document was fetched, which is the difference between a signal "
                "and a flag");
  });

  AddTest(tests, "Fetch/AbortFiresAtTheSignal", [] {
    Session session;
    session.Run(
        "const c = new AbortController();"
        "c.signal.addEventListener('abort', () => console.log('heard ' + c.signal.aborted));"
        "c.abort();");
    ExpectEqString(session.Console(), "heard true",
                   "the event fires after `aborted` is true, because a handler reads both");
  });

  AddTest(tests, "Fetch/PostSendsTheBodyAndTheHeaders", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "ok"));
    session.Run(
        "fetch('/submit', {method: 'post', body: 'a=1',"
        "  headers: {'X-Token': 'abc'}}).then(r => r.text()).then(t => console.log(t));");
    ExpectEqString(session.Console(), "ok", session.Errors());
    const std::string& request = session.factory.log.requests.at(1);
    Expect(request.rfind("POST /submit ", 0) == 0,
           "the method is uppercased, because `post` has to be a POST");
    Expect(request.find("x-token: abc\r\n") != std::string::npos,
           "the author header went out, folded -- a Headers name is lowercased on the way in "
           "and a page reads it back the same way");
    Expect(request.find("Content-Type: text/plain;charset=UTF-8\r\n") != std::string::npos,
           "and a string body gets the type the specification gives it -- which is also what "
           "keeps a POST of a string a *simple* request");
    Expect(request.find("\r\n\r\na=1") != std::string::npos, "the body followed the headers");
  });

  AddTest(tests, "Fetch/APageCannotSetAForbiddenHeader", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "ok"));
    session.Run(
        "fetch('/d', {headers: {'Cookie': 'sid=stolen', 'Origin': 'https://bank.example',"
        "  'X-Fine': 'yes'}}).then(r => console.log('done'));");
    const std::string& request = session.factory.log.requests.at(1);
    Expect(request.find("sid=stolen") == std::string::npos, "no forged Cookie");
    Expect(request.find("bank.example") == std::string::npos, "no forged Origin");
    Expect(request.find("x-fine: yes\r\n") != std::string::npos,
           "and an ordinary header is untouched");
  });

  AddTest(tests, "Fetch/ARequestObjectIsWhatFetchReads", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "ok"));
    session.Run(
        "const req = new Request('/api', {method: 'PUT', headers: {'X-A': '1'}});"
        "console.log(req.method + ' ' + req.url + ' ' + req.headers.get('x-a'));"
        "fetch(req).then(r => r.text()).then(t => console.log(t));");
    ExpectEqString(session.Console(), "PUT /api 1|ok",
                   "a Request is the arguments to a fetch as a value a page can pass around, "
                   "and `fetch` reads the same three properties off anything: " +
                       session.Errors());
    Expect(session.factory.log.requests.at(1).rfind("PUT /api ", 0) == 0,
           "and the request that went out is the one the object described");
  });

  AddTest(tests, "Fetch/HeadersAreACollection", [] {
    Session session;
    session.Run(
        "const h = new Headers({'A': '1'});"
        "h.append('b', '2'); h.append('B', '3'); h.set('a', '9');"
        "let seen = []; h.forEach((v, k) => seen.push(k + '=' + v));"
        "console.log(h.get('B') + ' ' + h.has('c') + ' ' + seen.join(','));"
        "h.delete('b'); console.log(h.get('b'));");
    ExpectEqString(session.Console(), "2, 3 false a=9,b=2,b=3|null",
                   "folded names, repeated values joined, insertion order kept, and a `set` "
                   "that replaces in place rather than moving the field to the end: "
                   + session.Errors());
  });

  AddTest(tests, "Fetch/ResponseBodyIsAbsentRatherThanFake", [] {
    Session session;
    session.Serve("page.example", OkResponse("text/plain", "x"));
    session.Run("fetch('/d').then(r => console.log('body ' + (r.body === undefined)));");
    ExpectEqString(session.Console(), "body true",
                   "a `body` that was present and buffering would make `if (response.body)` "
                   "lie to every page that streams -- ADR 0012's rule about stubs");
  });

  AddTest(tests, "Fetch/ANavigationCancelsWhatThePageWasFetching", [] {
    Session session;
    // Every response is held, so the test decides when each arrives: the
    // document, and then nothing else until it says so.
    session.factory.delivery = ScriptedFactory::Delivery::Held;
    session.Serve("page.example", OkResponse("text/plain", "late"));
    session.Serve("page.example", OkResponse("text/html", "<html><body>next</body></html>"));
    session.Start(
        "fetch('/slow').then(() => console.log('resolved'), () => console.log('rejected'));");
    session.Pump(2);
    Expect(session.factory.Release("GET / HTTP"), "the document's connection was let go");
    session.Pump(4);
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()), 2,
                "the page ran and its fetch went out");
    Expect(session.factory.Held() == 1, "and is still waiting for an answer");

    session.channel.Ui().Send(ipc::NavigateMessage{"https://page.example/next"});
    session.engine.HandlePendingMessages();
    Expect(session.factory.Held() == 1,
           "the fetch's connection is closed by the navigation, and the next document's is "
           "the one now waiting");
    session.factory.ReleaseAll();
    RunEngineToIdle(session.engine);
    ExpectEqString(session.Console(), "",
                   "a fetch belongs to the document that asked for it: the navigation dropped "
                   "it, so neither handler ran -- a promise from a document that is gone "
                   "settles never rather than into an interpreter that has been torn down");
  });
}

}  // namespace microbrowser::tests

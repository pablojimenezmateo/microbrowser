// `sessionStorage` and `localStorage`, from a page's own script.
//
// ADR 0021. The unit-level assertions are in StorageTests.cpp; these are the ones that
// only a real document can make -- that the names exist at all, that a write survives
// a same-document navigation, that the property form and the method form are the same
// store, and that a quota failure is a `QuotaExceededError` a page can catch.

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

}  // namespace

void RegisterStorageScriptTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StorageScript/BothStoresExistAndAreSeparate", [] {
    // Plex's very first inline script, before any bundle loads, reads
    // `sessionStorage` -- so "does the name exist" is the assertion this whole session
    // was ordered around.
    Session session;
    session.Run(
        "sessionStorage.setItem('k', 'session');"
        "localStorage.setItem('k', 'local');"
        "console.log(sessionStorage.getItem('k') + '/' + localStorage.getItem('k') +"
        " '/' + typeof sessionStorage + '/' + sessionStorage.length);");
    ExpectEqString(session.Console(), "session/local/object/1",
                   "two stores, and a write to one is not a write to the other");
  });

  AddTest(tests, "StorageScript/ThePropertyFormAndTheMethodFormAreOneStore", [] {
    // `localStorage.theme` is more common in real code than `getItem('theme')`, and a
    // page mixes them freely. A `Proxy` is what makes both work over one store.
    Session session;
    session.Run(
        "localStorage.theme = 'dark';"
        "const viaMethod = localStorage.getItem('theme');"
        "localStorage.setItem('lang', 'en');"
        "const viaProperty = localStorage.lang;"
        "delete localStorage.theme;"
        "console.log(viaMethod + '/' + viaProperty + '/' + localStorage.getItem('theme') +"
        " '/' + ('lang' in localStorage) + '/' + JSON.stringify(Object.keys(localStorage)));");
    // A missing key is `null` from `getItem` and `undefined` as a property, which is
    // the specification's asymmetry rather than an oversight -- and `Object.keys`
    // returns the *stored* keys, not `length` and the methods.
    ExpectEqString(session.Console(), "dark/en/null/true/[\"lang\"]",
                   "one store, two spellings, and enumeration sees only the data");
  });

  AddTest(tests, "StorageScript/AWriteSurvivesASameDocumentNavigation", [] {
    // "Signing in survives a reload within a session" is this session's check, and this
    // is the part of it a single document can assert: the store is owned by the tab
    // rather than by the document, so what a script writes is still there after the
    // page navigates within the session.
    Session session;
    session.Serve(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 96\r\n\r\n"
        "<html><body><script>console.log('second:' + sessionStorage.getItem('token'))"
        "</script></body></html>");
    session.Run("sessionStorage.setItem('token', 'abc');");
    session.channel.Ui().Send(ipc::NavigateMessage{"https://page.example/second"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    Expect(session.Console().find("second:abc") != std::string::npos,
           "the second document reads what the first one wrote");
  });

  AddTest(tests, "StorageScript/AQuotaFailureIsAnErrorAPageCanCatch", [] {
    // The specified failure, and one real pages handle because Safari's quotas trained
    // them to. A `setItem` that silently did nothing would be a page that believes it
    // saved.
    Session session;
    session.Run(
        "let caught = 'none';"
        "try { localStorage.setItem('k', 'x'.repeat(6 * 1024 * 1024)); }"
        "catch (e) { caught = e.name; }"
        "console.log(caught + '/' + localStorage.length);");
    ExpectEqString(session.Console(), "QuotaExceededError/0",
                   "the write threw and stored nothing");
  });

  AddTest(tests, "StorageScript/AnOpaqueOriginHasNeitherNameRatherThanAnEmptyStore", [] {
    // A `data:` document is not a site, so there is no partition to key a store by.
    // Chrome and Firefox throw `SecurityError` on access; this browser does not declare
    // the names at all, which is the same answer in the form ADR 0012 argues for -- and
    // the one that survives feature detection, since `if (window.localStorage)` then
    // takes the fallback path instead of the native one.
    //
    // The alternative that was written first is the trap: an empty store that accepts
    // writes and forgets them is a page that believes it saved.
    Session session;
    session.factory.script.push_back(ScriptedTransport::Exchange{"page.example", 443, true,
                                                                 std::string()});
    session.engine.PageLoader().SetTransport(session.factory);
    session.channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.engine.HandlePendingMessages();
    session.channel.Ui().Send(ipc::NavigateMessage{
        "data:text/html,<script>console.log(typeof sessionStorage + '/' + typeof localStorage)"
        "</script>"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    ExpectEqString(session.Console(), "undefined/undefined",
                   "neither name exists in a document with an opaque origin");
  });

  AddTest(tests, "StorageScript/ClearAndKeyWalkTheSameOrderAPageWrote", [] {
    Session session;
    session.Run(
        "localStorage.setItem('b', '1'); localStorage.setItem('a', '2');"
        "const keys = localStorage.key(0) + ',' + localStorage.key(1) + ',' + localStorage.key(9);"
        "localStorage.clear();"
        "console.log(keys + '/' + localStorage.length);");
    // `key(9)` past the end is `null`, not an error, and `clear()` empties the store.
    ExpectEqString(session.Console(), "b,a,null/0", "insertion order, then nothing");
  });
}

}  // namespace microbrowser::tests

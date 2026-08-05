// The module loader: a static graph closed before evaluation, and a dynamic
// `import()` answered later.
//
// Ledger session 50, and ADR 0011's one unanswered design question. The
// assertions worth reading are the two that say *when* things happen: a module
// whose import has not arrived is not evaluated, and a dynamic import's promise
// is pending across a turn of the loop rather than settled synchronously.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "engine/ModuleLoader.h"
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

std::string JsResponse(std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: text/javascript\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  void Serve(std::string_view body) {
    factory.script.push_back(ScriptedTransport::Exchange{"", 443, true, JsResponse(body)});
  }

  void Load(std::string_view html) {
    factory.script.insert(factory.script.begin(),
                          ScriptedTransport::Exchange{"page.example", 443, true,
                                                      OkResponse("text/html", html)});
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/index.html"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    // Turns past the load, because a dynamic import is answered on a *later*
    // turn by construction and a test that stopped at idle would be asserting
    // that it never resolves.
    for (int turn = 0; turn < 8; ++turn) {
      engine.Advance();
    }
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

  bool Requested(std::string_view needle) const {
    for (const std::string& request : factory.log.requests) {
      if (request.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

void RegisterModuleLoaderTests(std::vector<TestCase>& tests) {
  // --- ModuleLoader on its own -----------------------------------------------

  AddTest(tests, "ModuleLoader/ResolvesAgainstTheReferrerThenTheDocument", [] {
    engine::ModuleLoader loader;
    loader.SetDocumentUrl("https://page.example/app/index.html");
    ExpectEqString(loader.Resolve("./x.js", "https://page.example/app/main.js").value_or("-"),
                   "https://page.example/app/x.js", "relative to the referrer");
    ExpectEqString(loader.Resolve("../y.js", "https://page.example/app/main.js").value_or("-"),
                   "https://page.example/y.js", "and up out of its directory");
    ExpectEqString(loader.Resolve("/abs.js", "https://page.example/app/main.js").value_or("-"),
                   "https://page.example/abs.js", "an absolute path");
    ExpectEqString(loader.Resolve("https://cdn.example/z.js", "").value_or("-"),
                   "https://cdn.example/z.js", "a full URL needs no base");
    // A `data:` referrer is not a base -- there is no directory to be relative to
    // -- so a relative import inside one resolves against the document, which is
    // what a browser does with it.
    ExpectEqString(loader.Resolve("./x.js", "data:text/javascript,0").value_or("-"),
                   "https://page.example/app/x.js", "a data: referrer falls back to the document");
    // A bare specifier names nothing: there is no import map and no
    // node_modules, and guessing a URL would fetch something the page did not ask
    // for.
    Expect(!loader.Resolve("react", "https://page.example/app/main.js").has_value(),
           "a bare specifier does not resolve");
  });

  AddTest(tests, "ModuleLoader/ADataUrlIsItsOwnSourceAndNeedsNoFetch", [] {
    engine::ModuleLoader loader;
    loader.SetDocumentUrl("https://page.example/");
    const std::string url = "data:text/javascript,export default 7";
    ExpectEqString(loader.Resolve(url, "").value_or("-"), url, "it resolves to itself");
    Expect(loader.AddDataUrl(url), "and decodes without a fetch");
    const std::string* source = loader.Source(url);
    Expect(source != nullptr, "the source is there");
    ExpectEqString(*source, "export default 7", "decoded");
  });

  AddTest(tests, "ModuleLoader/NamesWhatTheGraphIsStillMissing", [] {
    engine::ModuleLoader loader;
    loader.SetDocumentUrl("https://page.example/");
    loader.Add("https://page.example/a.js", "import './b.js'; export * from './c.js';");
    const std::vector<std::string> missing = loader.MissingFrom("https://page.example/a.js");
    ExpectEqInt(static_cast<long long>(missing.size()), 2,
                "an import and a re-export both name a module");
    // A re-export that was not followed would leave a module unfetched, and the
    // resolver -- which cannot fetch -- would then fail on it.
    Expect(missing.at(0).find("b.js") != std::string::npos ||
               missing.at(1).find("b.js") != std::string::npos,
           "the import");
    Expect(missing.at(0).find("c.js") != std::string::npos ||
               missing.at(1).find("c.js") != std::string::npos,
           "and the re-export");
    loader.Add("https://page.example/b.js", "");
    loader.Add("https://page.example/c.js", "");
    Expect(loader.MissingFrom("https://page.example/a.js").empty(),
           "and the graph closes once they are there");
  });

  AddTest(tests, "ModuleLoader/AGraphIsWalkedALayerAtATimeUntilItCloses", [] {
    engine::ModuleLoader loader;
    loader.SetDocumentUrl("https://page.example/");
    loader.Add("https://page.example/a.js", "import './b.js';");
    ExpectEqInt(static_cast<long long>(loader.MissingFrom("").size()), 1, "b");
    loader.Add("https://page.example/b.js", "import './c.js';");
    // The second layer is only visible once the first has arrived, which is why
    // this is asked again after every arrival rather than once at the start.
    ExpectEqInt(static_cast<long long>(loader.MissingFrom("").size()), 1, "then c");
    loader.Add("https://page.example/c.js", "import './a.js';");
    Expect(loader.MissingFrom("").empty(), "and a cycle closes rather than looping");
  });

  // --- through a real engine -------------------------------------------------

  AddTest(tests, "ModuleLoader/AStaticImportIsFetchedBeforeAnythingIsEvaluated", [] {
    Session session;
    session.Serve("import { value } from './dep.js'; console.log('main saw ' + value);");
    session.Serve("export const value = 42;");
    session.Load("<html><body><script type=\"module\" src=\"/main.js\"></script></body></html>");
    ExpectEqString(session.Console(), "main saw 42",
                   "the dependency was fetched and linked before the module ran. Errors: " +
                       session.Errors());
    Expect(session.Requested("/dep.js"), "and it really was fetched");
  });

  AddTest(tests, "ModuleLoader/ADynamicImportIsAPendingPromiseAnsweredOnALaterTurn", [] {
    Session session;
    session.Serve(
        "let settled = false;"
        "const p = import('./late.js');"
        "console.log('pending ' + !settled);"
        "p.then(function (m) { console.log('later ' + m.answer) });");
    session.Serve("export const answer = 'yes';");
    session.Load("<html><body><script type=\"module\" src=\"/main.js\"></script></body></html>");
    // The order is the assertion: the module ran to completion with the promise
    // *pending*, and the answer arrived afterwards. A synchronous import would
    // have printed them the other way round or not at all.
    ExpectEqString(session.Console(), "pending true|later yes", "Errors: " + session.Errors());
    Expect(session.Requested("/late.js"), "and the fetch went out");
  });

  AddTest(tests, "ModuleLoader/ADynamicImportOfADataUrlNeedsNoNetwork", [] {
    // reddit's entry point is a `data:` module, and its own imports are what made
    // this session necessary.
    Session session;
    session.Load(
        "<html><body><script type=\"module\">"
        "import('data:text/javascript,export const n = 5')"
        "  .then(function (m) { console.log('data ' + m.n) });"
        "</script></body></html>");
    ExpectEqString(session.Console(), "data 5", "Errors: " + session.Errors());
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()), 1,
                "one request: the document");
  });

  AddTest(tests, "ModuleLoader/ADynamicImportThatCannotResolveRejectsRatherThanHanging", [] {
    Session session;
    session.Load(
        "<html><body><script type=\"module\">"
        "import('react').then(function () { console.log('resolved') },"
        "  function (e) { console.log('rejected ' + e.name) });"
        "</script></body></html>");
    // A bare specifier names nothing here. Rejected, because a promise nobody
    // will settle is a page that waits forever with no error anywhere.
    ExpectEqString(session.Console(), "rejected TypeError", "Errors: " + session.Errors());
  });

  AddTest(tests, "ModuleLoader/ADynamicImportsOwnStaticGraphIsClosedFirst", [] {
    Session session;
    session.Serve("import('./one.js').then(function (m) { console.log('got ' + m.total) });");
    session.Serve("import { part } from './two.js'; export const total = part + 1;");
    session.Serve("export const part = 9;");
    session.Load("<html><body><script type=\"module\" src=\"/main.js\"></script></body></html>");
    ExpectEqString(session.Console(), "got 10",
                   "the dynamically imported module's own import was fetched too. Errors: " +
                       session.Errors());
    Expect(session.Requested("/two.js"), "the second layer was fetched");
  });

  AddTest(tests, "ModuleLoader/AModuleFetchedTwiceIsOneModule", [] {
    Session session;
    session.Serve(
        "import './dep.js';"
        "import('./dep.js').then(function () { console.log('again') });");
    session.Serve("console.log('dep ran');");
    session.Load("<html><body><script type=\"module\" src=\"/main.js\"></script></body></html>");
    // Keyed by resolved URL, so a shared dependency is shared rather than run
    // twice -- which is the difference between a module system and an include.
    ExpectEqString(session.Console(), "dep ran|again", "Errors: " + session.Errors());
    std::size_t dep_requests = 0;
    for (const std::string& request : session.factory.log.requests) {
      dep_requests += request.find("/dep.js") != std::string::npos ? 1u : 0u;
    }
    ExpectEqInt(static_cast<long long>(dep_requests), 1, "and fetched once");
  });
}

}  // namespace microbrowser::tests

// Session history, `pushState`, and the origin check that is the whole point.
//
// ADR 0026 §1-3. Two groups: `engine::SessionHistory` on its own, and the
// feature end to end through a real engine. The ones that matter most are in the
// second group and they are all the same assertion from different angles -- **a
// page cannot move the URL bar to an origin that is not its own**, because a page
// that could would be able to spoof any site it liked.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "engine/SessionHistory.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

using engine::SessionHistory;

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
  }
};

std::string PageRunning(std::string_view script) {
  std::string html = "<html><body><p id=one>text</p><script>";
  html += script;
  html += "</script></body></html>";
  return html;
}

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  // The document, plus spare answers for anything else it asks for.
  void Run(std::string_view script) {
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", PageRunning(script))});
    for (int i = 0; i < 4; ++i) {
      factory.script.push_back(ScriptedTransport::Exchange{
          "", 443, true, OkResponse("text/html", PageRunning("console.log('second')"))});
    }
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/start"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    // The turn boundary, where a traversal a script asked for is taken. A test
    // that skipped it would be asserting that `history.back()` did nothing.
    for (int turn = 0; turn < 4; ++turn) {
      engine.Advance();
    }
  }

  void Traverse(int delta) {
    channel.Ui().Send(ipc::TraverseHistoryMessage{delta});
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

  // The last URL the engine told the chrome to display. This is the sentence
  // ADR 0026 §2 holds every feature to: the URL bar shows the origin of the
  // document that is displayed.
  std::string DisplayedUrl() {
    std::string url;
    while (std::optional<ipc::EngineToUi> message = channel.Ui().TryReceive()) {
      if (const auto* committed = std::get_if<ipc::NavigationCommittedMessage>(&*message)) {
        url = committed->url;
      }
    }
    return url;
  }
};

}  // namespace

void RegisterHistoryTests(std::vector<TestCase>& tests) {
  // --- SessionHistory on its own ---------------------------------------------

  AddTest(tests, "SessionHistory/BackAndForwardWalkTheList", [] {
    SessionHistory history;
    Expect(!history.CanGoBack() && !history.CanGoForward(), "an empty history goes nowhere");
    history.PushDocument("a", 1);
    Expect(!history.CanGoBack(), "one entry is not something to go back from");
    history.PushDocument("b", 2);
    history.PushDocument("c", 3);
    Expect(history.CanGoBack() && !history.CanGoForward(), "at the end of the list");
    ExpectEqString(history.Go(-1)->url, "b", "back one");
    ExpectEqString(history.Go(-1)->url, "a", "and one more");
    Expect(!history.CanGoBack() && history.CanGoForward(), "now at the start");
    ExpectEqString(history.Go(1)->url, "b", "and forward again");
    ExpectEqString(history.Go(1)->url, "c", "to the end");
    history.Go(-2);
    ExpectEqString(history.Current()->url, "a", "and two at once, the other way");
  });

  AddTest(tests, "SessionHistory/AMoveOffTheEndMovesNothing", [] {
    // Deliberately not a clamp: a page that called `history.go(-100)` meant
    // somewhere specific, and landing it at the start instead would be a
    // navigation it did not ask for.
    SessionHistory history;
    history.PushDocument("a", 1);
    history.PushDocument("b", 2);
    Expect(history.Go(-5) == nullptr, "past the start is nothing");
    ExpectEqString(history.Current()->url, "b", "and the cursor did not move");
    Expect(history.Go(3) == nullptr, "past the end is nothing");
    Expect(history.Go(0) == nullptr, "and zero traverses nothing at all");
  });

  AddTest(tests, "SessionHistory/NavigatingAfterGoingBackTruncatesTheForwardEntries", [] {
    SessionHistory history;
    history.PushDocument("a", 1);
    history.PushDocument("b", 2);
    history.PushDocument("c", 3);
    history.Go(-1);
    history.PushDocument("d", 4);
    Expect(!history.CanGoForward(), "forward stops working after taking a different path");
    ExpectEqInt(static_cast<long long>(history.Length()), 3, "a, b, d");
    ExpectEqString(history.Current()->url, "d", "and d is where we are");
  });

  AddTest(tests, "SessionHistory/APushStateEntryBelongsToTheDocumentThatMadeIt", [] {
    // The whole same-document/cross-document distinction, and a URL comparison
    // cannot answer it: two loads of `/a` and two `pushState('/a')` calls look
    // identical as URLs and are nothing alike.
    SessionHistory history;
    history.PushDocument("https://a.test/", 7);
    history.PushState("https://a.test/one", {});
    history.PushState("https://a.test/two", {});
    ExpectEqInt(static_cast<long long>(history.Length()), 3, "three entries");
    for (int back = 0; back < 2; ++back) {
      Expect(history.Go(-1)->document == 7,
             "every one of them belongs to the document that pushed it");
    }
  });

  AddTest(tests, "SessionHistory/ReplaceStateRewritesRatherThanAdds", [] {
    SessionHistory history;
    history.PushDocument("https://a.test/", 1);
    history.PushState("https://a.test/one", {});
    history.ReplaceState("https://a.test/two", {});
    ExpectEqInt(static_cast<long long>(history.Length()), 2, "still two entries");
    ExpectEqString(history.Current()->url, "https://a.test/two", "with the new URL");
  });

  AddTest(tests, "SessionHistory/TheStateGenerationMovesWheneverTheAnswerWould", [] {
    // What `history.state`'s memo is keyed on. If it did not move, a page would
    // read the previous entry's state after a traversal.
    SessionHistory history;
    history.PushDocument("a", 1);
    const std::uint64_t after_push = history.StateGeneration();
    history.PushState("b", {});
    Expect(history.StateGeneration() != after_push, "pushState moves it");
    const std::uint64_t after_state = history.StateGeneration();
    history.Go(-1);
    Expect(history.StateGeneration() != after_state, "and so does a traversal");
    const std::uint64_t after_go = history.StateGeneration();
    history.SetCurrentTitle("t");
    Expect(history.StateGeneration() == after_go, "a title does not: it is not the state");
  });

  // --- The feature, through a real engine ------------------------------------

  AddTest(tests, "History/PushStateMovesTheUrlBarWithoutLoading", [] {
    Session session;
    session.Run(
        "history.pushState({page: 1}, '', '/one');"
        "console.log(location.pathname + ' ' + history.length + ' ' + history.state.page);");
    ExpectEqString(session.Console(), "/one 2 1",
                   "the address moved, an entry appeared, and the state came back. Errors: " +
                       session.Errors());
    ExpectEqString(session.DisplayedUrl(), "https://page.example/one",
                   "and the URL bar shows the document that is displayed");
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()), 1,
                "one request: the document. A pushState is not a load");
  });

  AddTest(tests, "History/StateIsTheSameObjectEachTimeItIsRead", [] {
    // Deserializing on every read would make this false, and a page that stashes
    // `history.state` and compares later would see a change that did not happen.
    Session session;
    session.Run(
        "history.pushState({a: [1]}, '', '/one');"
        "console.log(history.state === history.state);"
        "const first = history.state;"
        "console.log(first.a[0] + ':' + (history.state === first));");
    ExpectEqString(session.Console(), "true|1:true", "Errors: " + session.Errors());
  });

  AddTest(tests, "History/AStateThatCannotBeClonedIsADataCloneError", [] {
    Session session;
    session.Run(
        "try { history.pushState({run: function () {}}, '', '/one') }"
        "catch (e) { console.log(e.name) }"
        "console.log(location.pathname);");
    ExpectEqString(session.Console(), "DataCloneError|/start",
                   "and the URL did not move either. Errors: " + session.Errors());
  });

  AddTest(tests, "History/ACrossOriginPushStateIsRefusedAndTheUrlBarDoesNotMove", [] {
    // **The load-bearing assertion.** Each of these looks like it should work and
    // must not: they are the shapes an address-bar spoof is built from. ADR
    // 0026 §2 names all four.
    Session session;
    session.Run(
        "const targets = ['https://evil.example/', 'https://page.example:8443/',"
        "  'http://page.example/', 'data:text/html,x', 'javascript:1'];"
        "for (const target of targets) {"
        "  try { history.pushState({}, '', target); console.log('MOVED ' + target) }"
        "  catch (e) { console.log(e.name) }"
        "}"
        "console.log(location.href);");
    ExpectEqString(
        session.Console(),
        "SecurityError|SecurityError|SecurityError|SecurityError|SecurityError|"
        "https://page.example/start",
        "a different host, a different port, a different scheme, a data: URL and a "
        "javascript: URL are all refused, and the address never moved. Errors: " +
            session.Errors());
  });

  AddTest(tests, "History/ASameOriginPathIsAllowedAndAnUnparseableOneIsASyntaxError", [] {
    Session session;
    session.Run(
        "history.pushState({}, '', 'https://page.example/deep/path?q=1#f');"
        "console.log(location.pathname + location.search + location.hash);"
        "try { history.pushState({}, '', 'http://[') } catch (e) { console.log(e.name) }");
    ExpectEqString(session.Console(), "/deep/path?q=1#f|SyntaxError",
                   "Errors: " + session.Errors());
  });

  AddTest(tests, "History/BackWithinADocumentFiresPopStateAndDoesNotLoad", [] {
    Session session;
    session.Run(
        "addEventListener('popstate', function (e) {"
        "  console.log('popstate ' + (e.state ? e.state.n : 'null') + ' ' + location.pathname)"
        "});"
        "history.pushState({n: 1}, '', '/one');"
        "history.pushState({n: 2}, '', '/two');"
        "history.back();");
    ExpectEqString(session.Console(), "popstate 1 /one",
                   "the handler saw the entry it went back to. Errors: " + session.Errors());
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()), 1,
                "and nothing was fetched: a same-document traversal is a paint");
    ExpectEqString(session.DisplayedUrl(), "https://page.example/one",
                   "and the URL bar followed");
  });

  AddTest(tests, "History/TheChromesBackButtonTraversesTheSameWay", [] {
    Session session;
    session.Run(
        "addEventListener('popstate', function () { console.log('popstate ' + location.pathname) });"
        "history.pushState({}, '', '/one');");
    session.Traverse(-1);
    ExpectEqString(session.Console(), "popstate /start",
                   "a delta from the chrome and one from a script take the same road. Errors: " +
                       session.Errors());
  });

  AddTest(tests, "History/AFragmentLinkIsAnEntryAndAHashChangeRatherThanALoad", [] {
    Session session;
    session.Run(
        "addEventListener('hashchange', function (e) {"
        "  console.log('hash [' + location.hash + '] from ' + e.oldURL + ' to ' + e.newURL)"
        "});"
        "history.pushState({}, '', '#one');"
        "console.log(location.hash + ' ' + history.length);");
    // `pushState` does *not* fire hashchange -- only a traversal or a link does,
    // which is the distinction a router depends on.
    ExpectEqString(session.Console(), "#one 2", "Errors: " + session.Errors());
    session.Traverse(-1);
    // Both URLs, because the event's whole content is the pair -- and the one
    // being left is the one that had the fragment.
    ExpectEqString(session.Console(),
                   "#one 2|hash [] from https://page.example/start#one to "
                   "https://page.example/start",
                   "going back off a fragment fires hashchange. Errors: " + session.Errors());
  });

  AddTest(tests, "History/GoingBackToAnotherDocumentLoadsItAndDoesNotPushAnEntry", [] {
    Session session;
    session.Run("console.log('first');");
    // A real navigation, so the second entry belongs to a different document.
    session.channel.Ui().Send(ipc::NavigateMessage{"https://page.example/second"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    ExpectEqString(session.Console(), "second",
                   "the second document replaced the first. Errors: " + session.Errors());

    const std::size_t requests_before = session.factory.log.requests.size();
    session.Traverse(-1);
    Expect(session.factory.log.requests.size() > requests_before,
           "a cross-document traversal is a load");
    ExpectEqString(session.DisplayedUrl(), "https://page.example/start",
                   "and it lands on the entry that was there");
    // The entry it traversed to is still at its own index, so forward works.
    session.channel.Ui().Send(ipc::TraverseHistoryMessage{1});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    ExpectEqString(session.DisplayedUrl(), "https://page.example/second",
                   "a traversal must not push a new entry and strand what was in front of it");
  });

  AddTest(tests, "History/PopStateNeverFiresOnTheInitialLoad", [] {
    Session session;
    session.Run(
        "addEventListener('popstate', function () { console.log('popstate') });"
        "console.log('loaded ' + (history.state === null));");
    ExpectEqString(session.Console(), "loaded true",
                   "a fresh document has no state and fires nothing. Errors: " +
                       session.Errors());
  });

  AddTest(tests, "History/LocationAssignNavigatesAfterTheTurn", [] {
    // youtube consent Accept sets SOCS then location.assign(savePreferenceUrl).
    // Without assign the dialog never leaves; without the turn boundary the
    // assign would tear the document down under the script that asked.
    Session session;
    session.Run(
        "console.log(typeof location.assign + ' ' + typeof location.replace);");
    ExpectEqString(session.Console(), "function function",
                   "assign and replace exist. Errors: " + session.Errors());

    Session navigate;
    navigate.Run("location.assign('https://page.example/second');");
    ExpectEqString(navigate.DisplayedUrl(), "https://page.example/second",
                   "location.assign loads after the turn. Errors: " + navigate.Errors());
  });

  AddTest(tests, "History/LocationReplaceRewritesTheCurrentEntry", [] {
    Session session;
    session.Run("location.replace('https://page.example/second');");
    ExpectEqString(session.DisplayedUrl(), "https://page.example/second",
                   "replace navigates. Errors: " + session.Errors());
    const std::size_t requests_before = session.factory.log.requests.size();
    session.Traverse(-1);
    ExpectEqInt(static_cast<long long>(session.factory.log.requests.size()),
                static_cast<long long>(requests_before),
                "replace left no prior entry, so back loads nothing");
  });

  AddTest(tests, "History/LocationHrefSetterAssigns", [] {
    Session session;
    session.Run("location.href = 'https://page.example/second';");
    ExpectEqString(session.DisplayedUrl(), "https://page.example/second",
                   "href= is assign. Errors: " + session.Errors());
  });
}

}  // namespace microbrowser::tests

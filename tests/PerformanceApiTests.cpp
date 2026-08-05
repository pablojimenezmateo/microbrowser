// `performance` and `PerformanceObserver`.
//
// Ledger session 50. The assertion that matters most is the one about
// `supportedEntryTypes`: it has to list exactly what this browser delivers, and
// `longtask` must not be in it. A browser that claimed support and delivered
// nothing leaves a page waiting on a callback that never comes, which is ADR
// 0012's rule at the place a page can actually check it.

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

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  void Run(std::string_view head, std::string_view script) {
    std::string html = "<html><head>";
    html += head;
    html += "</head><body>text<script>";
    html += script;
    html += "</script></body></html>";
    factory.script.push_back(
        ScriptedTransport::Exchange{"page.example", 443, true, OkResponse("text/html", html)});
    for (int i = 0; i < 4; ++i) {
      factory.script.push_back(
          ScriptedTransport::Exchange{"", 443, true, OkResponse("text/css", "p{color:red}")});
    }
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    // The frame is where observers deliver -- ADR 0018 §5's one place -- so a
    // test that never reached one would be asserting that nothing fires.
    for (int turn = 0; turn < 4; ++turn) {
      engine.Advance();
      engine.RunDueWork();
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
};

}  // namespace

void RegisterPerformanceApiTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PerformanceApi/SupportedEntryTypesListsWhatIsActuallyDelivered", [] {
    // The whole point of the list. reddit reads it and only observes `longtask`
    // when it is there; a browser that lied would leave its page-load timer
    // waiting forever. Safari does not support `longtask` either, so this is a
    // shape the web already handles.
    Session session;
    session.Run("",
                "const t = PerformanceObserver.supportedEntryTypes;"
                "console.log(t.join(','));"
                "console.log(t.includes('longtask'));");
    ExpectEqString(session.Console(), "mark,measure,navigation,resource|false",
                   "Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/MarkAndMeasureAreEntriesAPageCanReadBack", [] {
    Session session;
    session.Run("",
                "performance.mark('a');"
                "performance.mark('b', {startTime: 50});"
                "performance.measure('span', 'a', 'b');"
                "const m = performance.getEntriesByName('span')[0];"
                "console.log(m.entryType + ' ' + m.startTime + ' ' + m.duration);"
                "console.log(performance.getEntriesByType('mark').length);"
                "performance.clearMarks('a');"
                "console.log(performance.getEntriesByType('mark').length);");
    ExpectEqString(session.Console(), "measure 0 50|2|1",
                   "a measure between two marks, and clearing one by name. Errors: " +
                       session.Errors());
  });

  AddTest(tests, "PerformanceApi/MeasureTakesTheObjectFormTooBecauseRealPagesUseIt", [] {
    Session session;
    session.Run("",
                "performance.measure('x', {start: 10, end: 40});"
                "const m = performance.getEntriesByName('x')[0];"
                "console.log(m.startTime + ' ' + m.duration);");
    ExpectEqString(session.Console(), "10 30", "Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/AnObserverIsCalledWithWhatItWatches", [] {
    Session session;
    session.Run("",
                "new PerformanceObserver(function (list, observer) {"
                "  console.log('saw ' + list.getEntries().length + ' ' +"
                "    list.getEntries().map(function (e) { return e.name }).join('/'));"
                "  console.log('byType ' + list.getEntriesByType('mark').length);"
                "  console.log('self ' + (observer instanceof PerformanceObserver));"
                "}).observe({entryTypes: ['mark']});"
                "performance.mark('one');"
                "performance.mark('two');"
                "performance.measure('ignored', {start: 0, end: 1});");
    ExpectEqString(session.Console(), "saw 2 one/two|byType 2|self true",
                   "the measure was not delivered, because it was not watched. Errors: " +
                       session.Errors());
  });

  AddTest(tests, "PerformanceApi/BufferedDeliversWhatAlreadyHappened", [] {
    // What reddit needs: it observes `navigation` from a script that runs after
    // the navigation it wants to hear about.
    Session session;
    session.Run("",
                "performance.mark('early');"
                "new PerformanceObserver(function (list) {"
                "  console.log('buffered ' + list.getEntries()[0].name);"
                "}).observe({type: 'mark', buffered: true});");
    ExpectEqString(session.Console(), "buffered early", "Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/ObservingAnUnsupportedTypeIsSilentRatherThanAThrow", [] {
    // The specification's behaviour, and the reason `supportedEntryTypes` is the
    // honest way to find out: a page that observes `longtask` gets no callback
    // and no error.
    Session session;
    session.Run("",
                "try {"
                "  new PerformanceObserver(function () { console.log('fired') })"
                "    .observe({type: 'longtask'});"
                "  console.log('no throw');"
                "} catch (e) { console.log('threw ' + e.name) }");
    ExpectEqString(session.Console(), "no throw", "Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/DisconnectStopsDeliveryAndTakeRecordsDrains", [] {
    Session session;
    session.Run("",
                "const o = new PerformanceObserver(function () { console.log('fired') });"
                "o.observe({type: 'mark'});"
                "performance.mark('a');"
                "console.log('took ' + o.takeRecords().length);"
                "o.disconnect();"
                "performance.mark('b');"
                "console.log('after disconnect ' + o.takeRecords().length);");
    // `takeRecords` drained the queue, so the frame delivered nothing; after
    // `disconnect` nothing is queued at all.
    ExpectEqString(session.Console(), "took 1|after disconnect 0",
                   "and the callback never ran. Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/TheNavigationEntryCarriesDomContentLoaded", [] {
    // reddit's perf module reports its metric only when
    // `domContentLoadedEventStart` is non-zero, so an entry that answered zero
    // would be one that silently does nothing.
    Session session;
    session.Run("",
                "new PerformanceObserver(function (list) {"
                "  const e = list.getEntries()[0];"
                "  console.log(e.entryType + ' ' + (typeof e.domContentLoadedEventStart) +"
                "    ' ' + (e.domContentLoadedEventStart >= 0) + ' ' + e.type);"
                "}).observe({type: 'navigation', buffered: true});");
    ExpectEqString(session.Console(), "navigation number true navigate",
                   "Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/AResourceEntryExistsForEverySubresource", [] {
    Session session;
    session.Run("<link rel=\"stylesheet\" href=\"/a.css\">",
                "new PerformanceObserver(function (list) {"
                "  for (const e of list.getEntries()) {"
                "    console.log(e.initiatorType + ' ' + (e.name.indexOf('/a.css') >= 0) +"
                "      ' ' + (e.decodedBodySize > 0));"
                "  }"
                "}).observe({type: 'resource', buffered: true});");
    ExpectEqString(session.Console(), "css true true",
                   "one entry, named by the URL it resolved to. Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/NowIsThePagesClockAndNotAWallClock", [] {
    Session session;
    session.Run("",
                "console.log(typeof performance.now());"
                "console.log(performance.now() >= 0 && performance.now() < 3600000);"
                "console.log(performance.timeOrigin);");
    // Small and non-negative: a duration since this document started. A wall
    // clock would be about 1.7e12, which is the number this must never be.
    ExpectEqString(session.Console(), "number|true|0", "Errors: " + session.Errors());
  });

  AddTest(tests, "PerformanceApi/AnEntryProducedInACallbackWaitsForTheNextDelivery", [] {
    // Otherwise a callback that marks would re-enter the delivery it is in, and a
    // page controls how deep that goes.
    Session session;
    session.Run("",
                "let depth = 0;"
                "new PerformanceObserver(function (list) {"
                "  depth++;"
                "  console.log('call ' + depth + ' saw ' + list.getEntries().length);"
                "  if (depth < 3) { performance.mark('again') }"
                "}).observe({type: 'mark'});"
                "performance.mark('first');");
    // Three deliveries, one entry each, and never a nested call.
    ExpectEqString(session.Console(), "call 1 saw 1|call 2 saw 1|call 3 saw 1",
                   "Errors: " + session.Errors());
  });
}

}  // namespace microbrowser::tests

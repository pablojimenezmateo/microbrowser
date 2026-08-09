// `indexedDB`, from a page's own script.
//
// ADR 0038. The unit-level assertions for the store itself are in IndexedDbTests.cpp;
// these are the ones that only a real document and a real interpreter can make -- that
// `open` delivers `upgradeneeded` then `success` on later turns, that a put/get/delete
// round-trips through structured clone, that a `keyPath` derives a key from the value
// rather than needing one passed, and that the two shapes youtube's Woffle/PES path and
// its EntityStore actually use both work: `createObjectStore(name, {keyPath})` plus a
// transaction's get/put/delete, and a non-unique index walked with `IDBKeyRange.only()`
// and deleted with a cursor.

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

  // Runs `script` against a fresh document and drives the loop past both the
  // navigation and every task an IDB request queued behind it -- `RunEngineToIdle`
  // alone stops the moment `IsLoading()` goes false, which is before a request's
  // `TimerQueue::QueueTask` delivery ever gets a turn (see CspEnforcementTests.cpp's
  // `Load` for the same shape against CSP's own post-navigation fetches).
  void Run(std::string_view script) {
    const std::string html = PageRunning(script);
    std::string document = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n";
    document += "Content-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
    factory.script.insert(factory.script.begin(),
                          ScriptedTransport::Exchange{"page.example", 443, true, document});
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

  // A post-load probe against the page's own interpreter, for the tests that
  // need to read a JavaScript value only ADR 0038's async delivery produced --
  // `microbrowser_snapshot -eval` is the production version of the same seam.
  // `open()` is asynchronous even for a brand-new database (ADR 0038's model,
  // matching `MessagePort`), so a script cannot `console.log` an IDB result
  // in the same turn it made the request -- these tests read the log array
  // back afterwards instead, once every queued task has actually run.
  std::string EvaluateOrder() { return engine.EvaluateScript("order.join(',')"); }
  std::string EvaluateLog() { return engine.EvaluateScript("log.join(',')"); }

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

void RegisterIndexedDbScriptTests(std::vector<TestCase>& tests) {
  AddTest(tests, "IndexedDbScript/TheYpsShapedFeatureDetectPasses", [] {
    // youtube's Woffle/PES path checks `indexedDB`, `BroadcastChannel` and the
    // interface names it is about to `instanceof` before it builds anything --
    // a stub that answered `typeof indexedDB === 'object'` but had no real
    // `IDBDatabase`/`IDBTransaction`/`IDBObjectStore` behind it would still fail
    // that check, which is the case ADR 0012 calls worse than an absence.
    //
    // The second half is the exact gate in `yPS`:
    //   "IDBTransaction" in self && "objectStoreNames" in IDBTransaction.prototype
    // The first half needs the global `in` operator to see scope bindings (same
    // rule as `globalThis.Math`); the second needs the accessor on the
    // prototype, not an own property on each transaction instance.
    Session session;
    session.Run(
        "const have = [typeof indexedDB, typeof IDBDatabase, typeof IDBTransaction,"
        " typeof IDBObjectStore, typeof IDBIndex, typeof IDBKeyRange, typeof IDBCursor,"
        " typeof IDBCursorWithValue, typeof IDBRequest, typeof IDBOpenDBRequest,"
        " typeof BroadcastChannel,"
        " ('IDBTransaction' in self),"
        " ('objectStoreNames' in IDBTransaction.prototype)"
        "].join(',');"
        "console.log(have);");
    ExpectEqString(session.Console(),
                   "object,function,function,function,function,function,function,"
                   "function,function,function,function,true,true",
                   "every yPS-relevant name exists, is `in self`, and objectStoreNames "
                   "is on the prototype");
  });

  AddTest(tests, "IndexedDbScript/OpenDeliversUpgradeneededThenSuccessOnLaterTurns", [] {
    // The specified order, and the async model ADR 0038 commits to: neither
    // event fires inside the call that made the request, which is what lets a
    // page attach `onupgradeneeded` and `onsuccess` after calling `open()`
    // and still see both -- and both fire after the turn that opened, never
    // inside it.
    Session session;
    session.Run(
        "globalThis.order = [];"
        "const req = indexedDB.open('actualDb', 1);"
        "req.onupgradeneeded = () => {"
        "  order.push('upgrade:' + req.result.objectStoreNames.length);"
        "  order.push('txn:' + (req.transaction && req.transaction.mode));"
        "  order.push('txnNull:' + (req.transaction === null));"
        "};"
        "req.onsuccess = () => order.push('success:' + (req.result instanceof IDBDatabase)"
        " + ':txnAfter:' + (req.transaction === null));"
        "console.log(order.join(','));"  // logged before either event: nothing yet.
        "order.push('__marker');");
    const std::string console = session.Console();
    Expect(console.find("upgrade") == std::string::npos && console.find("success") == std::string::npos,
           "the console line printed synchronously saw neither event");
    // By the time the whole load (including its queued tasks) has run, a brand
    // new database has upgraded from nothing and then succeeded, in that order,
    // after the marker the synchronous part of the script pushed itself.
    // youtube's EntityStore does `new v_(a.transaction)` inside upgradeneeded
    // and throws if that is null/undefined — so the mode must be present, and
    // cleared to null by the time success fires.
    Expect(session.EvaluateOrder() ==
               "__marker,upgrade:0,txn:versionchange,txnNull:false,success:true:txnAfter:true",
           "upgrade exposes a versionchange transaction; success clears it");
  });

  AddTest(tests, "IndexedDbScript/YpsCreatesTheDatabasesStoreAndPutGetDeleteRoundTrip", [] {
    // youtube's own shape: `createObjectStore("databases", {keyPath:
    // "actualName"})`, then get/put/delete inside a transaction, then the
    // transaction completes.
    Session session;
    session.Run(
        "globalThis.log = [];"
        "const open = indexedDB.open('ypsDb', 1);"
        "open.onupgradeneeded = () => {"
        "  open.result.createObjectStore('databases', {keyPath: 'actualName'});"
        "};"
        "open.onsuccess = () => {"
        "  const db = open.result;"
        "  const tx = db.transaction(['databases'], 'readwrite');"
        "  const store = tx.objectStore('databases');"
        "  const putReq = store.put({actualName: 'entities', version: 3});"
        "  putReq.onsuccess = () => {"
        "    log.push('putKey:' + putReq.result);"
        "    const getReq = store.get('entities');"
        "    getReq.onsuccess = () => {"
        "      log.push('got:' + getReq.result.version);"
        "      const delReq = store.delete('entities');"
        "      delReq.onsuccess = () => log.push('deleted');"
        "    };"
        "  };"
        "  tx.oncomplete = () => log.push('complete');"
        "};");
    ExpectEqString(session.EvaluateLog(), "putKey:entities,got:3,deleted,complete",
                   "the key came from keyPath, the value round-tripped, and the "
                   "transaction completed once every request on it settled");
  });

  AddTest(tests, "IndexedDbScript/PutClonesTheValueRatherThanAliasingIt", [] {
    // Structured clone, the same property StorageScript's own tests assert for
    // sessionStorage and MessageChannel asserts for a port: mutating the
    // object after the call must not change what is stored.
    Session session;
    session.Run(
        "globalThis.log = [];"
        "const open = indexedDB.open('cloneDb', 1);"
        "open.onupgradeneeded = () => { open.result.createObjectStore('s'); };"
        "open.onsuccess = () => {"
        "  const store = open.result.transaction(['s'], 'readwrite').objectStore('s');"
        "  const obj = {n: 1};"
        "  const putReq = store.put(obj, 'k');"
        "  putReq.onsuccess = () => {"
        "    obj.n = 99;"
        "    const getReq = store.get('k');"
        "    getReq.onsuccess = () => log.push('' + getReq.result.n);"
        "  };"
        "};");
    ExpectEqString(session.EvaluateLog(), "1", "the stored copy is unaffected by the later mutation");
  });

  AddTest(tests, "IndexedDbScript/EntityStoreShapedIndexQueryAndCursorDelete", [] {
    // The other shape from the player analysis: `createObjectStore` with a
    // compound keyPath, `createIndex` over one field, a lookup with
    // `IDBKeyRange.only()`, `index.getAll()`, and deleting every match with a
    // cursor's `continue()`/`delete()` rather than one call.
    Session session;
    session.Run(
        "globalThis.log = [];"
        "const open = indexedDB.open('entityDb', 1);"
        "open.onupgradeneeded = () => {"
        "  const store = open.result.createObjectStore('entities', {"
        "    keyPath: ['parentEntityKey', 'childEntityKey']});"
        "  store.createIndex('byParent', 'parentEntityKey', {unique: false});"
        "};"
        "open.onsuccess = () => {"
        "  const store = open.result.transaction(['entities'], 'readwrite').objectStore('entities');"
        "  store.put({parentEntityKey: 'p1', childEntityKey: 'c1', v: 'a'});"
        "  store.put({parentEntityKey: 'p1', childEntityKey: 'c2', v: 'b'});"
        "  const putThird = store.put({parentEntityKey: 'p2', childEntityKey: 'c3', v: 'c'});"
        "  putThird.onsuccess = () => {"
        "    log.push('key3:' + JSON.stringify(putThird.result));"
        "    const index = store.index('byParent');"
        "    const allReq = index.getAll(IDBKeyRange.only('p1'));"
        "    allReq.onsuccess = () => {"
        "      log.push('count:' + allReq.result.length);"
        "      const cursorReq = index.openCursor(IDBKeyRange.only('p1'));"
        "      cursorReq.onsuccess = function step() {"
        "        const cursor = cursorReq.result;"
        "        if (cursor === null) {"
        "          log.push('cursorDone');"
        "          const recheck = index.getAll(IDBKeyRange.only('p1'));"
        "          recheck.onsuccess = () => log.push('afterDelete:' + recheck.result.length);"
        "          return;"
        "        }"
        "        cursor.delete();"
        "        cursor.continue();"
        "      };"
        "    };"
        "  };"
        "};");
    ExpectEqString(session.EvaluateLog(),
                   "key3:[\"p2\",\"c3\"],count:2,cursorDone,afterDelete:0",
                   "a compound keyPath key, an index of two records under p1, and a "
                   "cursor walk that deletes both and leaves p2's record alone");
  });

  AddTest(tests, "IndexedDbScript/AGetRequestForAMissingKeyDeliversUndefinedNotAnError", [] {
    Session session;
    session.Run(
        "globalThis.log = [];"
        "const open = indexedDB.open('missDb', 1);"
        "open.onupgradeneeded = () => { open.result.createObjectStore('s'); };"
        "open.onsuccess = () => {"
        "  const req = open.result.transaction(['s']).objectStore('s').get('nope');"
        "  req.onsuccess = () => log.push(typeof req.result);"
        "  req.onerror = () => log.push('error');"
        "};");
    ExpectEqString(session.EvaluateLog(), "undefined", "a miss succeeds with undefined, not a failure");
  });

  AddTest(tests, "IndexedDbScript/ADuplicateUniqueIndexValueDeliversAConstraintErrorAPageCanCatch", [] {
    Session session;
    session.Run(
        "globalThis.log = [];"
        "const open = indexedDB.open('uniqDb', 1);"
        "open.onupgradeneeded = () => {"
        "  const store = open.result.createObjectStore('s');"
        "  store.createIndex('byEmail', 'email', {unique: true});"
        "};"
        "open.onsuccess = () => {"
        "  const store = open.result.transaction(['s'], 'readwrite').objectStore('s');"
        "  store.put({email: 'a@example.com'}, 'k1');"
        "  const dup = store.put({email: 'a@example.com'}, 'k2');"
        "  dup.onerror = () => log.push(dup.error.name);"
        "  dup.onsuccess = () => log.push('no error');"
        "};");
    ExpectEqString(session.EvaluateLog(), "ConstraintError",
                   "the second record under the same unique index value is refused");
  });

  AddTest(tests, "IndexedDbScript/DatabaseNamesAndObjectStoreNamesReflectWhatWasCreated", [] {
    Session session;
    session.Run(
        "globalThis.log = [];"
        "const open = indexedDB.open('reflectDb', 1);"
        "open.onupgradeneeded = () => {"
        "  const db = open.result;"
        "  db.createObjectStore('a');"
        "  db.createObjectStore('b');"
        "};"
        "open.onsuccess = () => {"
        "  const names = open.result.objectStoreNames;"
        "  log.push(names.length + ',' + names.contains('a') + ',' + names.contains('z'));"
        "};");
    ExpectEqString(session.EvaluateLog(), "2,true,false",
                   "objectStoreNames is a real DOMStringList-shaped answer");
  });
}

}  // namespace microbrowser::tests

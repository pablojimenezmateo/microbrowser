// Nested browsing contexts: `<iframe>`, its document, and the `load` that says so. ADR 0027 §1.
//
// Its own file rather than more of EngineTests.cpp, because every test here needs the same three
// things -- a scripted server with two documents on it, a page that appends a frame from script,
// and an assertion about *when* something happened -- and none of them is about the engine's
// loading state machine, which is what that file is about.
//
// **What these tests are really guarding is ordering.** A frame that loads and a frame that loads
// before its handler was attached render identically, produce the same document, and answer
// `contentDocument` the same way. The only thing that tells them apart is whether the event
// arrived, which is why almost every assertion below is on console output rather than on the tree.

#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "engine/Loader.h"
#include "engine/Page.h"
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
    catalog.SetGenericFamily("sans-serif", "Test");
    catalog.SetGenericFamily("monospace", "Test");
  }
};

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};

  void Send(ipc::UiToEngine message) {
    channel.Ui().Send(std::move(message));
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    while (channel.Ui().TryReceive()) {
    }
  }

  // A script turn on a settled page, then the loop. `EvaluateScript` alone is not enough for
  // anything here: appending an `<iframe>` starts a *request*, and the answer arrives on a later
  // turn -- which is the entire difference between this feature and the one that was here before.
  void Run(const std::string& source) {
    engine.EvaluateScript(source);
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
  }
};

std::string NotFoundResponse() {
  return "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 3\r\n\r\nno!";
}

std::string Joined(const std::vector<std::string>& lines) {
  std::string joined;
  for (const std::string& line : lines) {
    if (!joined.empty()) {
      joined += '|';
    }
    joined += line;
  }
  return joined;
}

using ScriptedFactory = ScriptedTransport::Factory;

}  // namespace

void RegisterFrameTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Frames/AParsedFrameLoadsAndFiresLoadAfterTheHandlerIsAttached", [] {
    // The ordering that makes the whole feature usable. The `<iframe>` is in the markup, so its
    // document is asked for during the subresource pass -- which is over before the page's own
    // scripts run. Dispatching `load` where the document was set would fire it before the line
    // that assigns the handler existed, and the page would wait forever for an event that had
    // already happened.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f src='/child.html'></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log('loaded ' + this.contentDocument.title);"
                   "};</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>child</title><p>hi</p>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "loaded child",
                   "load fires at the element, after the handler was attached, with the document");
  });

  AddTest(tests, "Frames/AScriptAppendedFrameLoads", [] {
    // Before this existed, `document.body.appendChild(iframe)` produced an empty box and no
    // request: frames were collected exactly once, during the parse. It is also the shape almost
    // every web-platform-test uses to make a second document.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>later</title>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    Expect(!session.engine.IsLoading(), "the navigation finished");

    session.Run(
        "const f = document.createElement('iframe');"
        "f.onload = function () { console.log('appended ' + f.contentDocument.title); };"
        "f.src = '/later.html';"
        "document.body.appendChild(f);");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "appended later",
                   "a frame a script appended after the load is fetched and fires load");
  });

  AddTest(tests, "Frames/AssigningSrcRenavigatesTheFrame", [] {
    // `iframe.src = other` is an *attribute* write, so it moves the document's mutation version
    // and leaves its structure version alone. The collection pass is gated on structure -- it has
    // to be, or a page writing attributes in a rAF loop re-walks every node at 60Hz -- so this is
    // the case a structure-only gate silently does nothing for.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>first</title>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>second</title>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "f.onload = function () {"
        "  console.log(f.contentDocument.title);"
        "  if (f.getAttribute('src') === '/first.html') { f.src = '/second.html'; }"
        "};"
        "f.src = '/first.html';"
        "document.body.appendChild(f);");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "first|second",
                   "assigning src navigates the existing context and fires load again");
  });

  AddTest(tests, "Frames/AnUnrelatedMutationDoesNotReloadAFrame", [] {
    // The other half of the rule above, and the expensive one to get wrong: a frame that reloads
    // whenever anything on the page changes is a request per mutation, sent to whoever the frame
    // points at, for as long as the page keeps mutating.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>once</title>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "f.onload = function () { console.log('load'); };"
        "f.src = '/once.html';"
        "document.body.appendChild(f);");
    // Attribute writes, class changes and appended elements: everything a page does that is not a
    // navigation. The script exchange list has nothing left in it, so a second request would fail
    // the run rather than quietly succeed.
    session.Run(
        "f.className = 'x';"
        "f.setAttribute('data-n', '1');"
        "document.body.appendChild(document.createElement('div'));");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "load",
                   "the frame loaded once and no mutation reloaded it");
  });

  AddTest(tests, "Frames/SrcdocParsesInlineMarkupWithNoRequest", [] {
    // `srcdoc` is how a test file makes a second document without a second file, and it is the
    // one frame that needs no network at all. The exchange list holds only the top document, so a
    // request here would be a failure rather than a silent extra fetch.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f srcdoc='<title>inline</title><p id=x>text</p>'></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log(this.contentDocument.getElementById('x').textContent);"
                   "};</" "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "text",
                   "srcdoc markup becomes the child document and fires load");
  });

  AddTest(tests, "Frames/AFrameThatFailsToLoadStillFiresLoad", [] {
    // A 404 in an `<iframe>` is a *rendered* error document in every browser, not a failed
    // subresource -- and the difference is not cosmetic. A page that waits on `onload` before
    // reading `contentDocument` hangs forever otherwise, which is a hang that a server the page
    // does not control gets to cause.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(
        ScriptedTransport::Exchange{"page.example", 443, true, NotFoundResponse()});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "f.onload = function () { console.log('load ' + (f.contentDocument !== null)); };"
        "f.src = '/missing.html';"
        "document.body.appendChild(f);");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "load true",
                   "a frame the server refused still fires load, with an empty document");
  });

  AddTest(tests, "Frames/ACrossOriginFrameHasNoContentDocument", [] {
    // ADR 0027 §2, and the reason it is structural rather than a guard: the engine never attaches
    // the child's document to the element, so there is nothing for `contentDocument` to return
    // and no caller who could forget to check.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "other.example", 443, true, OkResponse("text/html", "<title>secret</title>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "f.onload = function () { console.log('load ' + (f.contentDocument === null)); };"
        "f.src = 'https://other.example/x.html';"
        "document.body.appendChild(f);");

    // `load` still fires, and that is deliberate: it is dispatched at the *element*, in the
    // embedder, and carries nothing about what loaded. A page that could tell a cross-origin load
    // from a failure by whether the event arrived would have an oracle; one that gets the event
    // either way has nothing.
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "load true",
                   "a cross-origin child fires load and exposes no document");
  });

  AddTest(tests, "Frames/RemovingAFrameBeforeItsLoadEventFiresNothing", [] {
    // The lifetime edge. The queued event names an element, and a script can take that element out
    // of the document between the response arriving and the turn that dispatches.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>gone</title>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "f.onload = function () { console.log('should not fire'); };"
        "f.src = '/gone.html';"
        "document.body.appendChild(f);"
        "f.remove();");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "",
                   "a detached frame is owed no load event");
  });
}

}  // namespace microbrowser::tests

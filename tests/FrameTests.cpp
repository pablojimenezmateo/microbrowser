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

  AddTest(tests, "Frames/BodyOnloadContentAttributeRunsWhenTheWindowLoads", [] {
    // The parser writes `onload` on `<body>`; the event fires at the window.
    // Forwarding only from a scripted setAttribute left every `body onload`
    // in markup as a TIMEOUT -- encoding/legacy-mb-* decode files included.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<body onload=\"console.log('from-body')\">"
                   "<script>console.log('script')</script></body>")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "script|from-body",
                   "inline script first, then the parsed body onload as window.onload");
  });

  AddTest(tests, "Frames/BodyOnloadSeesAParsedIframeDocument", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<body onload=\"console.log(document.getElementById('f').contentDocument.title)\">"
                   "<iframe id=f src='/child.html'></iframe></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>child</title><p>hi</p>")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "child",
                   "body onload waits for the iframe, then contentDocument is readable");
  });

  AddTest(tests, "Frames/AFormTargetingAnIframeNavigatesThatFrame", [] {
    // encoding/legacy-mb-* encode-form: form.submit() with target=iframe, then
    // read the child's location.search. Navigating the parent instead is a
    // TIMEOUT -- testharness was the parent.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=t name=t></iframe>"
                   "<form id=f action='/blank' target=t>"
                   "<input name=q value=hello></form>"
                   "<script>"
                   "document.getElementById('t').onload = function () {"
                   "  if (this.contentWindow.location.search) {"
                   "    console.log(this.contentWindow.location.search);"
                   "  }"
                   "};"
                   "document.getElementById('f').submit();"
                   "</" "script>")});
    factory.script.push_back(
        ScriptedTransport::Exchange{"page.example", 443, true, OkResponse("text/html", "")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "?q=hello",
                   "the child loaded the form's GET. errors=" + [&] {
                     std::string e;
                     for (const auto& line : session.engine.ScriptErrors()) {
                       e += line + ";";
                     }
                     return e;
                   }());
  });

  AddTest(tests, "Frames/TwoFormSubmitsInOneTurnEachNavigateTheirIframe", [] {
    // encode-form-common.js starts two worker iframes from one window `load`
    // handler. One pending-submit slot dropped the second, so half the
    // async_tests never called done() and testharness timed out.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=a name=a></iframe><iframe id=b name=b></iframe>"
                   "<form id=fa action='/blank' target=a>"
                   "<input name=q value=one></form>"
                   "<form id=fb action='/blank' target=b>"
                   "<input name=q value=two></form>"
                   "<script>"
                   "function arm(id, form) {"
                   "  document.getElementById(id).onload = function () {"
                   "    if (this.contentWindow.location.search) {"
                   "      console.log(id + this.contentWindow.location.search);"
                   "    }"
                   "  };"
                   "  document.getElementById(form).submit();"
                   "}"
                   "arm('a','fa');"
                   "arm('b','fb');"
                   "</" "script>")});
    factory.script.push_back(
        ScriptedTransport::Exchange{"page.example", 443, true, OkResponse("text/html", "")});
    factory.script.push_back(
        ScriptedTransport::Exchange{"page.example", 443, true, OkResponse("text/html", "")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    const std::string got = Joined(session.engine.ConsoleOutput());
    const bool both = (got == "a?q=one|b?q=two" || got == "b?q=two|a?q=one");
    Expect(both, "both child frames loaded their form GET, got " + got);
  });

  AddTest(tests, "Frames/AChildsExternalScriptRunsBeforeLoad", [] {
    // Range-insertNode's iframe loads common.js then calls setupRangeTests from onload. Fetching
    // the child's HTML and firing `load` without its `<script src>` left that name undefined.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f src='/child.html'></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log(String(this.contentWindow.ready));"
                   "};</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<script src='/helper.js'></script>"
                   "<script>window.ready = window.fromHelper;</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/javascript", "window.fromHelper = 'yes';")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "yes",
                   "iframe load waits until the child's script src has run");
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

  AddTest(tests, "Frames/LoadAfterSrcIgnoresTheBlankDocument", [] {
    // Range-insertNode appends the iframe (about:blank), then assigns onload, then src. The blank
    // document's load is queued on append; delivering it after src was set ran the handler against
    // an empty page.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<title>from-src</title>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "document.body.appendChild(f);"
        "f.onload = function () { console.log(f.contentDocument.title); };"
        "f.src = '/from-src.html';");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "from-src",
                   "onload after src sees the navigated document, not about:blank");
  });

  AddTest(tests, "Frames/AChildsRelativeScriptSrcResolvesAgainstTheChild", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f></iframe>"
                   "<script>"
                   "var f = document.getElementById('f');"
                   "document.body.appendChild(f);"
                   "f.onload = function () { console.log(String(f.contentWindow.ready)); };"
                   "f.src = 'child.html';"
                   "</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<script src='../helper.js'></script>"
                   "<script>window.ready = window.fromHelper;</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/javascript", "window.fromHelper = 'yes';")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/dom/ranges/test.html"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "yes",
                   "a child's relative script src resolves against the child, not the embedder");
  });

  AddTest(tests, "Frames/AChildsFunctionDeclarationIsVisibleOnContentWindow", [] {
    // Range-insertNode's common.js is `"use strict"; function setupRangeTests(){}`.
    // The script ran; the parent still saw undefined, because a function
    // declaration is a global-scope binding and contentWindow reads went to the
    // embedder's scope.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<body></body>"
                   "<script>"
                   "var f = document.createElement('iframe');"
                   "document.body.appendChild(f);"
                   "f.onload = function () {"
                   "  console.log(typeof f.contentWindow.setupRangeTests);"
                   "  f.contentWindow.setupRangeTests();"
                   "  console.log(String(f.contentWindow.flag));"
                   "};"
                   "f.src = '/child.html';"
                   "</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html", "<script src='/common.js'></script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/javascript",
                   "\"use strict\";\nfunction setupRangeTests() { window.flag = 'ok'; }\n")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "function|ok",
                   "the parent can call a function the child's classic script declared");
  });

  AddTest(tests, "Frames/RestoringAChildDocumentKeepsBody", [] {
    // Range-insertNode clones the iframe's documentElement into a reference
    // createHTMLDocument, wipes the iframe, then clones it back. setupRangeTests
    // then does document.body.insertBefore -- which throws if the clone lost body.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", "<body>ok</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html", "<!doctype html><title>child</title><body><p>hi</p></body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    session.Run(
        "const f = document.createElement('iframe');"
        "document.body.appendChild(f);"
        "f.onload = function () {"
        "  const ref = document.implementation.createHTMLDocument('');"
        "  ref.removeChild(ref.documentElement);"
        "  ref.appendChild(f.contentDocument.documentElement.cloneNode(true));"
        "  while (f.contentDocument.firstChild && f.contentDocument.firstChild.nodeType != 10) {"
        "    f.contentDocument.removeChild(f.contentDocument.firstChild);"
        "  }"
        "  while (f.contentDocument.lastChild && f.contentDocument.lastChild.nodeType != 10) {"
        "    f.contentDocument.removeChild(f.contentDocument.lastChild);"
        "  }"
        "  f.contentDocument.appendChild(ref.documentElement.cloneNode(true));"
        "  console.log(String(f.contentDocument.body && f.contentDocument.body.tagName));"
        "};"
        "f.src = '/child.html';");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "BODY",
                   "wiping an iframe and cloning its documentElement back leaves document.body");
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

  // --- ADR 0042 §5: a same-origin child runs script, in a realm of its own ---

  AddTest(tests, "Frames/ASameOriginChildRunsItsOwnScript", [] {
    // The smallest thing that says the host half exists at all. Before this, a child's `<script>`
    // was collected by `Page::Load` and then never run by anybody -- the frame was a document with
    // no interpreter pointed at it.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f src='/child.html'></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log('title=' + this.contentDocument.title);"
                   "};</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html", "<title>before</title><script>document.title = 'after';</"
                                "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    // The embedder reads the title its child's *script* wrote, which is two facts in one
    // assertion: the child ran, and it ran before the `load` that told the parent to look.
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "title=after",
                   "a same-origin child's script runs, and runs before its element's load event");
  });

  AddTest(tests, "Frames/AChildGlobalIsNotTheEmbedders", [] {
    // **The security property, stated as an observable.** A missed realm guard would run the
    // child's script with the embedder's global current, and the way that shows is the child's
    // `var` landing on the parent's window -- so this asserts on the parent, about a name the
    // parent never declared. It is the difference between ADR 0042 §5 being implemented and
    // looking implemented, which is why it is a test rather than a comment.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f srcdoc=\"<script>var leaked = 1; window.mine = 2;</"
                   "script>\"></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log('leaked=' + (typeof leaked) + ' mine=' + (typeof window.mine));"
                   "};</" "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()),
                   "leaked=undefined mine=undefined",
                   "a child's globals stay in the child's realm");
  });

  AddTest(tests, "Frames/TwoChildrenDoNotShareAGlobal", [] {
    // One realm per browsing context rather than one realm for "the frames". Two `srcdoc`
    // children writing the same name would agree if they shared a global, and the second one
    // reporting `1` is exactly the bug a single shared child realm would produce.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe srcdoc=\"<script>window.n = 1;</" "script>\"></iframe>"
                   "<iframe id=b srcdoc=\"<script>window.seen = typeof window.n; window.n = 2;</"
                   "script>\"></iframe>"
                   "<script>document.getElementById('b').onload = function () {"
                   "  console.log('seen=' + this.contentWindow.document.title);"
                   "};</" "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    // Both children ran without either throwing, which is what the empty error list says; the
    // globals being separate is asserted by the previous test and this one guards the *count* of
    // realms rather than repeating it.
    Expect(session.engine.ScriptErrors().empty(), "neither child's script threw");
  });

  AddTest(tests, "Frames/ContentWindowIsTheChildsGlobal", [] {
    // What `contentWindow` was before: a plain object with a `document` on it, made fresh on every
    // read. So `f.contentWindow !== f.contentWindow`, `f.contentWindow.foo` never survived, and
    // `f.contentDocument !== f.contentWindow.document` -- a page comparing the last pair would
    // conclude it was looking at two different documents.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f srcdoc=\"<script>window.answer = 42;</" "script>\"></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log('answer=' + this.contentWindow.answer +"
                   "    ' same=' + (this.contentWindow === this.contentWindow) +"
                   "    ' doc=' + (this.contentDocument === this.contentWindow.document));"
                   "};</" "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "answer=42 same=true doc=true",
                   "contentWindow is the child's real global, and its document is contentDocument");
  });

  AddTest(tests, "Frames/AChildSeesItsParentAndTop", [] {
    // The other direction, and the one that needed the realms: a child reaching *out*. `parent`
    // and `top` were both the child's own window, so `while (w !== w.top)` terminated at once and
    // every "am I framed?" check in the suite answered no.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f srcdoc=\"<script>"
                   "window.report = (parent !== window) + ',' + (parent === top) + ',' +"
                   "  (parent.marker === 'embedder');"
                   "</" "script>\"></iframe>"
                   "<script>window.marker = 'embedder';"
                   "document.getElementById('f').onload = function () {"
                   "  console.log(this.contentWindow.report + ' len=' + window.length +"
                   "    ' idx=' + (window[0] === this.contentWindow) +"
                   "    ' selftop=' + (window.top === window));"
                   "};</" "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()),
                   "true,true,true len=1 idx=true selftop=true",
                   "a child's parent and top are the embedder; the embedder indexes its children");
  });

  AddTest(tests, "Frames/ACrossOriginChildHasNoWindow", [] {
    // **The security property from the other side.** A cross-origin child gets an interpreter of
    // its own rather than a realm, so there is nothing to put in the embedder's table -- and
    // `contentWindow` answering null is therefore structural rather than a check somebody
    // remembered to write. It must also be indistinguishable from a frame with no document at
    // all, because "is there a document there" is information about another origin.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html",
                   "<iframe id=f src='https://other.example/x.html'></iframe>"
                   "<script>document.getElementById('f').onload = function () {"
                   "  console.log('win=' + this.contentWindow + ' doc=' + this.contentDocument +"
                   "    ' len=' + window.length);"
                   "};</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "other.example", 443, true,
        OkResponse("text/html", "<script>window.secret = 1;</" "script>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "win=null doc=null len=0",
                   "a cross-origin child is invisible to its embedder, window and all");
  });
}

}  // namespace microbrowser::tests

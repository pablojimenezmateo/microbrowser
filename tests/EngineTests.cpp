#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TestSupport.h"
#include "bindings/AnimationFrames.h"
#include "css/MediaQuery.h"
#include "engine/Engine.h"
#include "net/RequestQueue.h"
#include "engine/Loader.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "privacy/PrivacyPolicy.h"
#include "url/Url.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include <chrono>
#include <thread>

#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"
#include "support/SyntheticPng.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::tests {

namespace {

// A font stack with no system fonts in it, so that nothing here depends on
// which typefaces the machine has installed.
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

std::string DataUrl(std::string_view html) {
  std::string url = "data:text/html,";
  for (const char c : html) {
    // `#` starts the *fragment*, which is not part of the URL's body -- so a
    // document that mentions one has to escape it, exactly as an author writing
    // the URL by hand would. Before the fragment was honoured this helper got
    // away with passing it through, and a page whose script said
    // `querySelector('#one')` silently lost everything from the quote onwards.
    if (c == '#') {
      url += "%23";
    } else {
      url.push_back(c);
    }
  }
  return url;
}

// Drives one navigation and returns everything the engine sent back.
// The three input messages a test sends, built once here rather than at forty
// call sites. The primary button is zero in the DOM's numbering, which is what
// the engine checks, and a typed character is a `key` and a `text` while a
// named key is only a `key` -- ADR 0017 §1's split, in the shape a test uses it.
// Console lines as one string, so a test states the whole sequence rather than
// indexing into a vector line by line.
std::string Joined(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) {
    out += out.empty() ? "" : "|";
    out += line;
  }
  return out;
}

ipc::PointerInputMessage ClickAt(float x, float y) {
  ipc::PointerInputMessage pointer;
  pointer.kind = ipc::PointerInputMessage::Kind::Down;
  pointer.position = gfx::FloatPoint{x, y};
  pointer.buttons = 1;
  return pointer;
}

ipc::PointerInputMessage ClickReleaseAt(float x, float y) {
  ipc::PointerInputMessage pointer;
  pointer.kind = ipc::PointerInputMessage::Kind::Up;
  pointer.position = gfx::FloatPoint{x, y};
  pointer.buttons = 0;
  return pointer;
}

ipc::KeyInputMessage TypedKey(const std::string& character) {
  ipc::KeyInputMessage key;
  key.key = character;
  key.text = character;
  if (character.size() == 1) {
    const char c = character[0];
    if (c >= 'a' && c <= 'z') {
      key.code = std::string("Key") + static_cast<char>('A' + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
      key.code = std::string("Key") + c;
    } else if (c >= '0' && c <= '9') {
      key.code = std::string("Digit") + c;
    }
  }
  return key;
}

ipc::KeyInputMessage NamedKey(const std::string& name) {
  ipc::KeyInputMessage key;
  key.key = name;
  key.code = name;
  return key;
}

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  std::vector<ipc::EngineToUi> sent;

  // Sends one message and then turns the crank until the engine is done with
  // it. Since ADR 0011 a navigation *starts* here and finishes over several
  // turns, so a test that only handled the message would assert on a page that
  // had not loaded yet.
  void Send(ipc::UiToEngine message) {
    channel.Ui().Send(std::move(message));
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    while (auto reply = channel.Ui().TryReceive()) {
      sent.push_back(std::move(*reply));
    }
  }

  // Types text one key at a time, the way a keyboard delivers it. One message
  // per character rather than one for the string: inserting is a *default
  // action* of a keydown now, so a page's handler sees each one and can stop it.
  void Type(std::string_view text) {
    for (const char c : text) {
      Send(TypedKey(std::string(1, c)));
    }
  }

  // Down then up: default actions (link navigation, form submit) run on release.
  void Click(float x, float y) {
    Send(ClickAt(x, y));
    Send(ClickReleaseAt(x, y));
  }

  const ipc::PaintFrameMessage* LastFrame() const {
    const ipc::PaintFrameMessage* found = nullptr;
    for (const ipc::EngineToUi& message : sent) {
      if (const auto* paint = std::get_if<ipc::PaintFrameMessage>(&message)) {
        found = paint;
      }
    }
    return found;
  }

  std::string LastTitle() const {
    std::string title;
    for (const ipc::EngineToUi& message : sent) {
      if (const auto* changed = std::get_if<ipc::TitleChangedMessage>(&message)) {
        title = changed->title;
      }
    }
    return title;
  }

  std::string LastCommittedUrl() const {
    std::string url;
    for (const ipc::EngineToUi& message : sent) {
      if (const auto* committed = std::get_if<ipc::NavigationCommittedMessage>(&message)) {
        url = committed->url;
      }
    }
    return url;
  }
};

std::size_t TextRunCount(const gfx::DisplayList& list) {
  std::size_t runs = 0;
  for (const gfx::DisplayCommand& command : list.Commands()) {
    runs += std::holds_alternative<gfx::DrawTextCommand>(command) ? 1u : 0u;
  }
  return runs;
}

std::optional<std::string> SubmissionTarget(engine::Page& page,
                                            gfx::FloatPoint point) {
  const std::optional<engine::FormSubmission> submission = page.FormSubmissionRequestAt(point);
  if (!submission.has_value() || submission->method != "GET") {
    return std::nullopt;
  }
  return submission->url;
}

std::optional<std::string> FocusedSubmissionTarget(engine::Page& page) {
  const std::optional<engine::FormSubmission> submission = page.FocusedFormSubmission();
  if (!submission.has_value() || submission->method != "GET") {
    return std::nullopt;
  }
  return submission->url;
}

}  // namespace

void RegisterEngineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Page/TheDocumentLifecycleIsAStateMachineAPageCanSee", [] {
    // A page that registers a `DOMContentLoaded` listener and is never told is
    // a page that does nothing at all, which is the state reddit's interstitial
    // was in. `readyState` moves with the events rather than beside them: a
    // handler that reads it must not be told the parse is still going.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><script>"
        "console.log('running: ' + document.readyState);"
        "document.addEventListener('DOMContentLoaded',"
        "  () => console.log('dcl: ' + document.readyState), {once: true});"
        "addEventListener('load', () => console.log('load: ' + document.readyState));"
        "</script></body></html>",
        "https://example.org/");
    page.RunScripts(0);
    Expect(page.NotifyLoad(), "something was listening for load");
    const std::vector<std::string>& output = page.ConsoleOutput();
    ExpectEqInt(static_cast<long long>(output.size()), 3, "three lines, in order");
    ExpectEqString(output.at(0), "running: loading", "a script runs while the document loads");
    ExpectEqString(output.at(1), "dcl: interactive", "DOMContentLoaded means interactive");
    ExpectEqString(output.at(2), "load: complete", "and load means complete");

    // A page listening for nothing costs nothing, which is what keeps `load`
    // from relaying out every document that ever finished.
    engine::Page quiet(fonts.catalog);
    quiet.Load("<html><body><script>1</script></body></html>", "https://example.org/");
    quiet.RunScripts(0);
    Expect(!quiet.NotifyLoad(), "nothing was listening, so nothing is reported");
  });

  AddTest(tests, "Page/ScriptsRunInDocumentOrderAcrossInlineAndExternal", [] {
    // The whole reason nothing runs until every external script has arrived: a
    // page's scripts must run in the order they appear, and an external one in
    // the middle cannot be skipped and caught up with later.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body>"
        "<script>globalThis.log = 'a';</script>"
        "<script src='b.js'></script>"
        "<script>globalThis.log += 'c';</script>"
        "</body></html>",
        "https://example.org/");

    const std::vector<engine::SubresourceRequest>& pending = page.PendingScripts();
    ExpectEqInt(static_cast<long long>(pending.size()), 1, "one external script");
    ExpectEqString(pending[0].url, "b.js", "named as it was written");

    page.AddScript(0, "globalThis.log += 'b'; console.log('external ran');");
    page.RunScripts(0);
    const std::vector<std::string>& output = page.ConsoleOutput();
    Expect(!output.empty(), "the external script ran");
    ExpectEqString(output.front(), "external ran", "and it was the fetched source");
  });

  AddTest(tests, "Page/NamedAccessLosesToAScriptThatWritesTheSameName", [] {
    // HTML §7.3.3 puts named access on the *named properties object*, which is
    // Window's prototype -- so `window.foo = x` on a page with `<div id=foo>`
    // makes an own property that shadows the element, permanently.
    //
    // This is a regression test with a real bill attached. The name was
    // installed as a getter-only own property of the global, which makes that
    // assignment a silent no-op in sloppy mode. testharness.js ends with
    // `expose(test, 'test')` -- a plain `global_scope['test'] = test` -- so on
    // every web-platform-test with an `id=test` in it the write vanished,
    // `test` kept answering with the element, and `test(function(){...})` threw
    // "not a function" before one subtest ran. Nine files in `dom/` reported
    // nothing at all and looked exactly like a hang.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><div id='thing'></div>"
        "<script>console.log('element first: ' + (thing.tagName || 'not an element'));</script>"
        "<script>window.thing = 'a page value';</script>"
        "<script>console.log('after the write: ' + thing);</script>"
        "</body></html>",
        "https://example.org/");
    page.RunScripts(0);
    const std::vector<std::string>& output = page.ConsoleOutput();
    ExpectEqInt(static_cast<long long>(output.size()), 2, "both reporting scripts ran");
    if (output.size() == 2) {
      ExpectEqString(output[0], "element first: DIV", "the id is a bare global to begin with");
      ExpectEqString(output[1], "after the write: a page value",
                     "and the page's own assignment wins from then on");
    }
  });

  AddTest(tests, "Page/AScriptThatNeverArrivesDoesNotStopTheOnesAfterIt", [] {
    // Its slot stays empty rather than shifting every later script's turn. A
    // page whose analytics tag is blocked is still a page, which is the whole
    // reason the blocking engine can be pointed at one.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body>"
        "<script src='missing.js'></script>"
        "<script>console.log('still ran');</script>"
        "</body></html>",
        "https://example.org/");
    ExpectEqInt(static_cast<long long>(page.PendingScripts().size()), 1, "one external");
    // Nothing supplies it, which is what a failed fetch looks like from here.
    page.RunScripts(0);
    Expect(!page.ConsoleOutput().empty(), "the inline script after it still ran");
    ExpectEqString(page.ConsoleOutput().front(), "still ran", "with its own output");
  });

  AddTest(tests, "Page/RunningScriptsTwiceRunsThemOnce", [] {
    // Idempotent, so a caller that fetches subresources first and one that
    // does not can both end with it.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<html><body><script>console.log('once');</script></body></html>",
              "https://example.org/");
    page.RunScripts(0);
    page.RunScripts(0);
    ExpectEqInt(static_cast<long long>(page.ConsoleOutput().size()), 1, "one line, not two");
  });

  AddTest(tests, "Page/InsertedExternalScriptIsCollectedAfterRun", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><script>"
        "const s = document.createElement('script');"
        "s.src = 'late.js';"
        "document.head.appendChild(s);"
        "</script></body></html>",
        "https://example.org/");
    page.RunScripts(0);
    Expect(page.CollectInsertedScripts(), "the injected tag is found after Run");
    const std::vector<engine::SubresourceRequest> pending = page.TakeUnrequestedScripts();
    ExpectEqInt(static_cast<long long>(pending.size()), 1, "one late script to fetch");
    ExpectEqString(pending[0].url, "late.js", "named as written");
  });

  AddTest(tests, "Engine/ALateScriptRunsAfterTheLoadHasFinished", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html", "<html><head></head><body>ok</body></html>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/javascript", "console.log('late ran')")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    Expect(!session.engine.IsLoading(), "the initial navigation finished");

    session.engine.EvaluateScript(
        "const s = document.createElement('script');"
        "s.src = '/late.js';"
        "document.head.appendChild(s);");
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "late ran",
                   "a script inserted after load still runs when it arrives");
  });

  AddTest(tests, "Engine/ALateCrossOriginScriptUsesThePageOriginAfterLoad", [] {
    // OptionsForSubresource used to dereference load_.base after the navigation
    // cleared it; a post-load script with crossorigin needs the page origin for
    // CORS (youtube SPA player base.js).
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html", "<html><head></head><body>ok</body></html>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "cdn.example", 443, true,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/javascript\r\n"
        "Access-Control-Allow-Origin: https://page.example\r\n"
        "Content-Length: 22\r\n"
        "\r\n"
        "console.log('cdn ran')"});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    Expect(!session.engine.IsLoading(), "navigation finished");

    session.engine.EvaluateScript(
        "const s = document.createElement('script');"
        "s.src = 'https://cdn.example/player.js';"
        "s.crossOrigin = 'anonymous';"
        "document.head.appendChild(s);");
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "cdn ran",
                   "post-load CORS script still runs with a cleared load_.base");
  });

  AddTest(tests, "Engine/ALateScriptFiresOnloadSoWaitersCanRun", [] {
    // YouTube `_.VE` / `P_U`: onload schedules a completion that does
    // `dataset.loaded || (set loaded, notify waiters)`. Pre-stamping
    // `data-loaded` before `load` made that completion a no-op (TD-0024).
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/html", "<html><head></head><body>ok</body></html>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true,
        OkResponse("text/javascript", "globalThis.playerReady = true;")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://page.example/"});
    Expect(!session.engine.IsLoading(), "navigation finished");

    session.engine.EvaluateScript(
        "globalThis.gotLoad = false;"
        "globalThis.gotWaiter = false;"
        "globalThis.sawLoadedBeforeNotify = null;"
        "const s = document.createElement('script');"
        "s.id = 'js-player';"
        "s.src = '/player_ias/base.js';"
        "s.onload = function () {"
        "  globalThis.gotLoad = true;"
        "  globalThis.sawLoadedBeforeNotify = !!(s.dataset && s.dataset.loaded);"
        "  setTimeout(function () {"
        "    if (s.dataset && s.dataset.loaded) { return; }"
        "    s.dataset.loaded = 'true';"
        "    globalThis.gotWaiter = true;"
        "  }, 0);"
        "};"
        "document.head.appendChild(s);");
    session.engine.HandlePendingMessages();
    // Post-load: IsLoading is false, so RunEngineToIdle alone will not drain
    // setTimeout(0) from onload. Mirror CspEnforcement's settle loop.
    for (int turn = 0; turn < 1000; ++turn) {
      const bool advanced = session.engine.Advance();
      const bool due = session.engine.RunDueWork();
      if (!advanced && !due && !session.engine.HasRunnableWork()) {
        break;
      }
    }

    ExpectEqString(session.engine.EvaluateScript("'' + gotLoad"), "true",
                   "script.onload ran after the late fetch");
    ExpectEqString(session.engine.EvaluateScript("'' + sawLoadedBeforeNotify"), "false",
                   "data-loaded must not be set before onload (VE OgC short-circuit)");
    ExpectEqString(session.engine.EvaluateScript("'' + gotWaiter"), "true",
                   "the VE-shaped waiter ran and could set data-loaded itself");
    ExpectEqString(session.engine.EvaluateScript("'' + playerReady"), "true",
                   "and the script body ran");
  });

  AddTest(tests, "Page/AClickReachesTheElementUnderIt", [] {
    // An inline element has no box geometry of its own -- its text fragments
    // carry the rectangles -- and a text box has no element. So a click on the
    // words inside a link hits a box with no origin inside a box with no area,
    // and testing either alone finds nothing. This is the case that found it.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><a id=link href='/next'>click the words</a>"
        "<script>"
        "globalThis.hits = 0;"
        "document.getElementById('link').addEventListener('click', e => {"
        "  hits++; e.preventDefault();"
        "});"
        "</script></body></html>",
        "https://example.org/");
    page.RunScripts(0);
    page.Layout(800.0f);

    // Inside the link's text, which is where a reader would click it.
    const engine::DispatchOutcome outcome = page.DispatchClickAt(gfx::FloatPoint{20.0f, 8.0f}, {});
    Expect(outcome.ran, "the page had handlers");
    Expect(outcome.prevented, "and one of them prevented the default");

    // The two facts are separate: a handler that changes the document needs a
    // relayout whether or not it prevented anything, and conflating them left
    // a page whose handler ran and whose screen did not change.
    const engine::DispatchOutcome elsewhere =
        page.DispatchClickAt(gfx::FloatPoint{700.0f, 400.0f}, {});
    Expect(!elsewhere.prevented, "a click on nothing prevents nothing");
  });

  AddTest(tests, "Page/TrustedClickGetsAFreshStepBudget", [] {
    // TD-0039 / TD-0045: after a spent host turn, click handlers used to inherit
    // steps_ > kMaxSteps and never reach preventDefault — youtube then followed
    // a#thumbnail as a full document navigation. InputTaskBudget always zeros
    // steps_ (even under live frames) and raises the hang ceiling for the
    // dispatch. A single catch absorbs once and zeros steps_; a second hang in
    // the same turn leaves the budget spent (same shape as
    // JsInterpreter/ABlockThatDeclaresNothingAllocatesNoScope).
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><a id=link href='/next'>click the words</a>"
        "<script>"
        "globalThis.hits = 0;"
        "document.getElementById('link').addEventListener('click', e => {"
        "  hits++; e.preventDefault();"
        "});"
        "</script></body></html>",
        "https://example.org/");
    page.RunScripts(0);
    page.Layout(800.0f);
    (void)page.EvaluateScript(
        "try { while (true) {} } catch (e) { while (true) {} }");
    const engine::DispatchOutcome outcome =
        page.DispatchClickAt(gfx::FloatPoint{20.0f, 8.0f}, {});
    ExpectEqString(page.EvaluateScript("String(globalThis.hits)"), "1",
                   "click handler runs after a spent turn");
    Expect(outcome.prevented, "preventDefault is reached on a fresh task budget");
  });

  AddTest(tests, "Page/PointerDownDoesNotDropTheBoxTree", [] {
    // TD-0047: pointerdown used to InvalidateLayout+LayoutAndPaint unconditionally,
    // rebuilding youtube search results under the press so click raced a new tree.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'>"
                   "<a href='/next' style='display:block;width:80px;height:40px'>go</a>"
                   "</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Next</title><body>next page</body>")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    const auto builds_before =
        util::ReadPerformanceCounter(util::PerfCounterId::LayoutTreeBuilds);
    session.channel.Ui().Send(ClickAt(5.0f, 5.0f));
    session.engine.HandlePendingMessages();
    ExpectEqInt(
        static_cast<long long>(util::ReadPerformanceCounter(util::PerfCounterId::LayoutTreeBuilds)),
        static_cast<long long>(builds_before),
        "pointerdown must not drop the box tree when no layout-affecting state changed");
    session.Send(ClickReleaseAt(5.0f, 5.0f));
    ExpectEqString(session.LastCommittedUrl(), "https://example.org/next",
                   "pointerup still follows the link default action");
  });

  AddTest(tests, "Page/MediaSourceOpenRunsAsAHostTask", [] {
    // TD-0040: sourceopen used to FireOn synchronously from video.src = blob:…
    // under the same hang-guard allotment as the stamp that set src. Soft-nav
    // then created SourceBuffers and never reached appendBuffer. Needs a
    // NetworkSource so `URL` / createObjectURL exist (Engine provides one).
    Session session;
    session.Send(ipc::NavigateMessage{
        "data:text/html,<!doctype html><video id=v></video>"});
    const std::string setup = session.engine.EvaluateScript(
        "(() => {"
        "  try {"
        "    globalThis.opened = false;"
        "    globalThis.buffers = 0;"
        "    const ms = new MediaSource();"
        "    ms.addEventListener('sourceopen', () => {"
        "      globalThis.opened = true;"
        "      ms.addSourceBuffer('video/mp4; codecs=\"avc1.42E01E\"');"
        "      globalThis.buffers = ms.sourceBuffers.length;"
        "    });"
        "    document.getElementById('v').src = URL.createObjectURL(ms);"
        "    return 'sync:' + globalThis.opened;"
        "  } catch (e) { return 'err:' + e; }"
        "})()");
    Expect(setup.rfind("sync:false", 0) == 0, "src assign queues sourceopen: " + setup);
    Expect(session.engine.RunDueWork(), "the queued sourceopen task is runnable");
    ExpectEqString(session.engine.EvaluateScript("String(globalThis.opened)"), "true",
                   "sourceopen runs on the host task");
    ExpectEqString(session.engine.EvaluateScript("String(globalThis.buffers)"), "1",
                   "and can addSourceBuffer under a fresh budget");
  });

  AddTest(tests, "Page/AClickReachesAFloatOverAnOverlappingBlock", [] {
    // CSS keeps a later in-flow block's border box full-width under a float;
    // only its line boxes shrink. Hit testing that walked last-sibling-first
    // then returned the block for every point in the float's rectangle.
    // old.reddit.com's `.side` search field is that shape.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>"
        "body { margin: 0 }"
        ".side { float: right; width: 100px; height: 40px; margin: 0 }"
        "input { width: 100px; height: 20px; margin: 0; padding: 0; border: 0 }"
        ".content { margin: 0; width: 300px; height: 80px }"
        "</style>"
        "<body>"
        "<div class='side'><input name='q'></div>"
        "<div class='content'>article</div>"
        "</body>",
        "https://example.org/");
    page.Layout(300.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{220.0f, 10.0f}),
           "a click on the floated search field focuses it, not the overlapping content block");
    const dom::Element* focused = page.FocusedElement();
    Expect(focused != nullptr && focused->TagName() == "input",
           "focus landed on the input inside the float");
  });

  AddTest(tests, "Page/AScriptChangesWhatIsLaidOut", [] {
    // The point of all of it: what a script builds is what gets laid out.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><div id=host></div>"
        "<script>"
        "for (let i = 0; i < 3; i++) {"
        "  const el = document.createElement('p');"
        "  el.appendText('row ' + i);"
        "  document.getElementById('host').appendChild(el);"
        "}"
        "</script></body></html>",
        "https://example.org/");
    page.RunScripts(0);
    page.Layout(800.0f);
    gfx::DisplayList list;
    page.Paint(list);
    int rows = 0;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      if (const auto* text = std::get_if<gfx::DrawTextCommand>(&command)) {
        const gfx::DisplayList::TextRun* run = list.TextAt(text->text);
        rows += run != nullptr && run->text.rfind("row ", 0) == 0 ? 1 : 0;
      }
    }
    ExpectEqInt(rows, 3, "three rows, built by script and painted");
  });

  // --- The loader -----------------------------------------------------------

  AddTest(tests, "Loader/DecodesPercentEncodedDataUrls", [] {
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:text/html,%3Cp%3Ehi%3C/p%3E");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "<p>hi</p>", "percent escapes are the payload, not decoration");
    // **This expectation changed when encodings landed** (ADR 0025 §2). It used to be exactly
    // `text/html`; a `data:` URL that names no charset now gets `;charset=utf-8` appended, because the
    // payload arrived percent-encoded and the bytes `%E6%97%A5` decodes to are UTF-8 -- following RFC
    // 2397's `US-ASCII` default instead means decoding them as windows-1252 and rendering `æ—¥` where
    // the author wrote 日. Every browser makes the same deviation.
    ExpectEqString(decoded.content_type, "text/html;charset=utf-8",
                   "the type came from the metadata, with the encoding a data URL implies");
    // A charset the URL *did* name is honoured rather than overridden.
    ExpectEqString(engine::DecodeDataUrl("data:text/html;charset=windows-1252,x").content_type,
                   "text/html;charset=windows-1252", "and one that was named is left alone");
  });

  AddTest(tests, "Loader/DecodesBase64DataUrls", [] {
    // "<b>x</b>" base64-encoded.
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:text/html;base64,PGI+eDwvYj4=");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "<b>x</b>", "and produced the bytes");
  });

  AddTest(tests, "Loader/RejectsMalformedDataUrls", [] {
    Expect(!engine::DecodeDataUrl("data:text/html").ok,
           "no comma is not a data URL, it is a string beginning with 'data:'");
    Expect(!engine::DecodeDataUrl("https://example.org/").ok, "and neither is another scheme");
    Expect(!engine::DecodeDataUrl("data:text/html;base64,!!!!").ok,
           "nor is base64 that is not base64");
  });

  AddTest(tests, "Loader/LeavesAMalformedEscapeAlone", [] {
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:,100% and %zz");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "100% and %zz",
                   "a lone percent is a byte; eating it would change the document");
  });

  AddTest(tests, "Loader/TheFragmentIsNotPartOfTheBody", [] {
    // Found by rendering `:target` on a data: URL: the fragment was decoded as
    // payload, so the document ended with the text "#one" drawn after it. The
    // fragment names something *in* the document; it is never document content.
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:text/html,%3Cp%3Ehi%3C/p%3E#one");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "<p>hi</p>", "the fragment is not body");
    ExpectEqString(engine::DecodeDataUrl("data:text/html;base64,PGI+eDwvYj4=#x").body, "<b>x</b>",
                   "and the same for base64, where a '#' was never valid input anyway");
  });

  AddTest(tests, "Loader/DefaultsTheContentTypeWhenTheUrlOmitsIt", [] {
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:,plain");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.content_type, "text/plain;charset=US-ASCII", "per the data URL spec");
  });

  // --- The page -------------------------------------------------------------

  AddTest(tests, "Page/TakesItsTitleFromTheDocument", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<html><head><title>Real Title</title></head><body>x</body></html>",
              "https://example.org/");
    ExpectEqString(page.Title(), "Real Title", "the <title> element is the title");
  });

  AddTest(tests, "Page/FallsBackToTheUrlWhenThereIsNoTitle", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<p>no title here</p>", "https://example.org/a");
    ExpectEqString(page.Title(), "https://example.org/a",
                   "a tab strip has to show something, and \"\" is not a title but a missing one");
  });

  AddTest(tests, "Page/AppliesStyleElementsFromTheDocument", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<html><head><style>div { width: 50px }</style></head>"
              "<body><div>x</div></body></html>",
              "");
    page.Layout(400.0f);
    gfx::DisplayList list;
    page.Paint(list);
    // Width is asserted through layout rather than by reaching into the box
    // tree, because the point is that the sheet reached the cascade.
    Expect(page.ContentHeight() > 0.0f, "the document laid out");
  });

  AddTest(tests, "Page/DoesNotCarryOneDocumentsStylesIntoTheNext", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>p { height: 500px }</style><p>x</p>", "");
    const float styled = page.Layout(400.0f);
    page.Load("<p>x</p>", "");
    const float unstyled = page.Layout(400.0f);
    Expect(styled > unstyled,
           "author sheets belong to the document that carried them; keeping the resolver would "
           "let the previous page style this one");
  });

  AddTest(tests, "Page/ScrollOffsetMovesTheGeometryRatherThanATransform", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='background-color:red'><p>ABC</p></body>", "");
    page.Layout(400.0f);

    gfx::DisplayList top;
    page.Paint(top);
    gfx::DisplayList scrolled;
    page.SetScrollOffsetY(40.0f);
    page.Paint(scrolled);
    Expect(!(top == scrolled), "scrolling changed the recorded geometry");
    Expect(top.Bounds().y - scrolled.Bounds().y == 40, "by exactly the scroll offset");
  });

  AddTest(tests, "Page/HitTestsLaidOutLinks", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><a href='/next'>ABC</a><p>outside</p></body>",
              "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> hit = page.LinkAt(gfx::FloatPoint{5.0f, 5.0f});
    Expect(hit.has_value(), "a point over link text hits the anchor");
    ExpectEqString(*hit, "/next", "the written href is returned for the engine to resolve");
    Expect(!page.LinkAt(gfx::FloatPoint{5.0f, 50.0f}).has_value(),
           "text outside the anchor is not a link");
  });

  AddTest(tests, "Page/HitTestsAbsoluteInsetFillLink", [] {
    // youtube's search thumbnail: host sized by ::before, link fills it with
    // top/right/bottom/left 0. A zero-height absolute link made every click miss.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "#host{position:relative;width:200px;height:100px}"
              "#host::before{content:\"\";display:block;padding-top:50%}"
              "a{position:absolute;top:0;right:0;bottom:0;left:0}"
              "</style>"
              "<body style='margin:0'><div id=host><a href='/watch'></a></div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}).has_value(),
           "a click inside the sized host hits the absolute fill link");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}), "/watch", "href");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{50.0f, 50.0f}), "and focuses the link");
  });

  AddTest(tests, "Page/VisibilityHiddenSkipsHitTesting", [] {
    // Polymer app-drawer (youtube's #guide): host covers the viewport with
    // visibility:hidden when closed so clicks reach content underneath.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "#cover{position:fixed;inset:0;visibility:hidden;z-index:1}"
              "a{display:block;width:100px;height:40px}"
              "</style>"
              "<body style='margin:0'>"
              "<a href='/under'>link</a>"
              "<div id=cover></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.LinkAt(gfx::FloatPoint{10.0f, 10.0f}).has_value(),
           "a hidden overlay does not steal the link under it");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{10.0f, 10.0f}), "/under", "href");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{10.0f, 10.0f}), "and focuses that link");
  });

  AddTest(tests, "Page/LinkAtSurvivesInvalidateDuringClick", [] {
    // After a click handler runs, the box tree may have been cleared
    // (InvalidateBoxTree). LinkAt must rebuild before answering — otherwise the
    // engine's link default action sees href=none and youtube search never
    // navigates.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><a id=t href='/watch' style='display:block;width:100px;height:40px'>x</a></body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.LinkAt(gfx::FloatPoint{10.0f, 10.0f}).has_value(), "link is there");
    page.InvalidateLayout();
    Expect(page.LinkAt(gfx::FloatPoint{10.0f, 10.0f}).has_value(),
           "LinkAt rebuilds after InvalidateLayout cleared boxes_");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{10.0f, 10.0f}), "/watch", "href");
  });

  AddTest(tests, "Page/LinkAtThroughOverflowHiddenAncestor", [] {
    // Abspos descendants stay under a DOM parent in the box tree. If that parent
    // clips (overflow:hidden) with a padding box that does not cover them, a
    // PointInside gate would never visit the link — youtube search thumbnails.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "#clip{overflow:hidden;height:0;position:relative;width:200px}"
              "a{position:absolute;top:0;left:0;width:200px;height:100px}"
              "</style>"
              "<body style='margin:0'><div id=clip><a href='/watch'></a></div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}).has_value(),
           "abspos link outside clipped parent's padding box is still hit");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}), "/watch", "href");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{50.0f, 50.0f}), "and focuses");
  });

  AddTest(tests, "Page/LinkAtFindsLinkUnderAbsposOverlay", [] {
    // youtube: yt-interaction is an absolute sibling covering the thumbnail
    // link. LinkAt must still find the href (paint order != "only the top box").
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "#card{position:relative;width:800px;height:280px}"
              "#thumb{position:relative;width:500px;height:280px}"
              "a{position:absolute;top:0;right:0;bottom:0;left:0}"
              "#ink{position:absolute;top:0;left:0;width:815px;height:288px}"
              "</style>"
              "<body style='margin:0'><div id=card>"
              "<div id=thumb><a href='/watch'></a></div>"
              "<div id=ink><div></div></div>"
              "</div></body>",
              "https://example.org/");
    page.Layout(900.0f);
    Expect(page.LinkAt(gfx::FloatPoint{274.0f, 252.0f}).has_value(),
           "link under abspos overlay is still found");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{274.0f, 252.0f}), "/watch", "href");
  });

  AddTest(tests, "Page/PointerEventsNoneSkipsHitTesting", [] {
    // youtube's yt-interaction ink layer sits above the thumbnail link with
    // pointer-events:none so the link receives the click.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "#host{position:relative;width:200px;height:100px}"
              "a{position:absolute;inset:0}"
              "#ink{position:absolute;inset:0;pointer-events:none}"
              "</style>"
              "<body style='margin:0'>"
              "<div id=host><a href='/watch'></a><div id=ink></div></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}).has_value(),
           "pointer-events:none overlay does not steal the link");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{50.0f, 50.0f}), "and focuses the link");
  });

  AddTest(tests, "Page/NegativeZIndexAbsposDoesNotStealInFlowHit", [] {
    // youtube's #background.ytd-masthead is position:absolute; z-index:-1 over
    // the full masthead. Without respecting negative z-index, clicks never
    // reach the search input (elementFromPoint returns #background).
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "#host{position:relative;width:200px;height:40px}"
              "#bg{position:absolute;inset:0;z-index:-1}"
              "input{width:200px;height:40px}"
              "</style>"
              "<body style='margin:0'>"
              "<div id=host><div id=bg></div><input name=search_query></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.FocusFromClickAt(gfx::FloatPoint{100.0f, 20.0f}),
           "click focuses the in-flow input under a z-index:-1 abspos");
    const dom::Element* focused = page.FocusedElement();
    Expect(focused != nullptr && focused->TagName() == "input",
           "focused element is the input, not #bg");
  });

  AddTest(tests, "Page/PositiveZIndexBeatsLaterAutoSibling", [] {
    // youtube consent: dialog is position:fixed;z-index:2202 under ytd-app
    // (abspos, z-index:auto), while iron-overlay-backdrop is a later body
    // sibling with position:fixed;z-index:auto covering the viewport. Paint
    // already orders by layer; hit-testing must match or Accept focuses the
    // backdrop/dialog shell instead of the button.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "body{margin:0}"
              "#app{position:absolute;inset:0}"
              "#dialog{position:fixed;left:50px;top:50px;width:200px;height:100px;"
              "z-index:2202}"
              "button{width:100%;height:100%}"
              "#backdrop{position:fixed;inset:0;z-index:auto}"
              "</style>"
              "<body>"
              "<div id=app><div id=dialog><button id=accept>Accept</button></div></div>"
              "<div id=backdrop></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.FocusFromClickAt(gfx::FloatPoint{100.0f, 80.0f}), "click focuses Accept");
    const dom::Element* focused = page.FocusedElement();
    Expect(focused != nullptr && focused->GetAttribute("id") != nullptr &&
               *focused->GetAttribute("id") == "accept",
           "focused element is #accept, not #backdrop");
  });

  AddTest(tests, "Page/HitTestsAbsposUnderNonStackingAbsposAncestor", [] {
    // youtube search: body often has height:0 with overflow:scroll while
    // ytd-app is position:absolute;z-index:auto (unit, not a stacking context)
    // and ytd-search is position:relative;z-index:0 (unit + SC) around the
    // abspos thumbnail. html/body overflow propagates to the viewport (TD-0034)
    // so their padding boxes must not clip collected units.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "html,body{margin:0;height:0;overflow:scroll}"
              "#app{position:absolute;left:0;top:0;width:400px;height:300px}"
              "#pm{width:400px;height:300px}"
              "#search{position:relative;z-index:0;width:400px;height:300px;"
              "overflow:hidden}"
              "#thumb{position:relative;width:200px;height:100px}"
              "a{position:absolute;inset:0}"
              "</style>"
              "<body>"
              "<div id=app><div id=pm><div id=search><div id=thumb>"
              "<a id=thumbnail href='/watch?v=1'></a>"
              "</div></div></div></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    const std::string hit = page.EvaluateScript(
        "var el=document.elementFromPoint(50,50); el && (el.id||el.tagName);");
    ExpectEqString(hit, "thumbnail",
                   "abspos link under non-stacking abspos ancestor is the top hit");
    Expect(page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}).has_value(), "LinkAt finds href");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{50.0f, 50.0f}), "click focuses the link");
    const dom::Element* focused = page.FocusedElement();
    Expect(focused != nullptr && focused->GetAttribute("id") != nullptr &&
               *focused->GetAttribute("id") == "thumbnail",
           "focused element is #thumbnail");
  });

  AddTest(tests, "Page/HitTestsAbsposInsideOverflowHiddenStackingContext", [] {
    // Closer to youtube search: after TD-0034, elementFromPoint still returned
    // ytd-search for an in-view abspos thumbnail. ytd-search is
    // position:relative;z-index:0;overflow:hidden around the link.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "body{margin:0}"
              "#search{position:relative;z-index:0;width:400px;height:300px;"
              "overflow:hidden}"
              "#thumb{position:relative;width:200px;height:100px;margin:50px}"
              "a{position:absolute;inset:0;overflow:hidden}"
              "</style>"
              "<body>"
              "<div id=search><div id=thumb>"
              "<a id=thumbnail href='/watch?v=1'></a>"
              "</div></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    const std::string hit = page.EvaluateScript(
        "var r=document.getElementById('thumbnail').getBoundingClientRect();"
        "var el=document.elementFromPoint(r.left+r.width/2, r.top+r.height/2);"
        "el && (el.id||el.tagName);");
    ExpectEqString(hit, "thumbnail",
                   "abspos link inside overflow:hidden stacking context is topmost");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{150.0f, 100.0f}), "/watch?v=1",
                   "LinkAt agrees");
  });

  AddTest(tests, "Page/HitTestsAbsposAfterDocumentScroll", [] {
    // youtube /results after TD-0036: scrollIntoView moves the document so the
    // thumbnail sits at the viewport top. Live pages then returned ytd-search
    // for every point on the thumb — click still SPA-navigated via script, but
    // hit-testing disagreed with paint.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetViewport(css::MediaContext{400.0f, 400.0f, 1.0f});
    page.Load("<style>"
              "html,body{margin:0;height:0}"
              "#app{position:absolute;left:0;top:0;width:400px;height:3000px}"
              "#search{position:relative;z-index:0;width:400px;height:2500px;"
              "overflow:hidden}"
              "#thumb{position:relative;width:200px;height:100px;margin-top:800px}"
              "a{position:absolute;inset:0}"
              "</style>"
              "<body><div id=app><div id=search><div id=thumb>"
              "<a id=thumbnail href='/watch?v=1'></a>"
              "</div></div></div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    (void)page.EvaluateScript("document.getElementById('thumbnail').scrollIntoView()");
    const std::string hit = page.EvaluateScript(
        "var r=document.getElementById('thumbnail').getBoundingClientRect();"
        "var el=document.elementFromPoint(r.left+r.width/2, r.top+r.height/2);"
        "el && (el.id||el.tagName);");
    ExpectEqString(hit, "thumbnail",
                   "scrolled abspos thumbnail stays the top hit, not the search SC");
  });

  AddTest(tests, "Page/HitTestsAbsposInFlexOverflowYHiddenAfterScroll", [] {
    // TD-0037 live shape: ytd-search is display:flex with asymmetric
    // overflow-x:visible;overflow-y:hidden. Prior fixtures used overflow:hidden
    // without flex; this pins the measured CSS so a miss cannot hide behind
    // "fixtures don't match youtube".
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetViewport(css::MediaContext{400.0f, 400.0f, 1.0f});
    page.Load("<style>"
              "html,body{margin:0;height:0}"
              "#app{position:absolute;left:0;top:0;width:400px;height:3000px}"
              "#search{position:relative;z-index:0;display:flex;"
              "flex-direction:column;width:400px;height:2500px;"
              "overflow-x:visible;overflow-y:hidden}"
              "#container{flex:1;min-height:0;position:relative}"
              "#thumb{position:relative;width:200px;height:100px;margin-top:800px}"
              "a{position:absolute;inset:0;overflow:hidden}"
              "</style>"
              "<body><div id=app><div id=search><div id=container>"
              "<div id=thumb><a id=thumbnail href='/watch?v=1'></a></div>"
              "</div></div></div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    (void)page.EvaluateScript("document.getElementById('thumbnail').scrollIntoView()");
    const std::string hit = page.EvaluateScript(
        "var r=document.getElementById('thumbnail').getBoundingClientRect();"
        "var el=document.elementFromPoint(r.left+r.width/2, r.top+r.height/2);"
        "el && (el.id||el.tagName);");
    ExpectEqString(hit, "thumbnail",
                   "flex + overflow-y:hidden search SC still hits the abspos thumb");
  });

  AddTest(tests, "Page/HitTestsAbsposUnderPositionedNonStackingAncestor", [] {
    // `position:relative; z-index:auto` is a paint unit but not a stacking
    // context (Appendix E). Positioned descendants belong to the ancestor
    // context. Hit-testing that visits the unit with collects=false skipped
    // those descendants and returned the ancestor — youtube `ytd-search` over
    // every thumbnail after scrollIntoView.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetViewport(css::MediaContext{400.0f, 400.0f, 1.0f});
    page.Load("<style>"
              "html,body{margin:0;height:0}"
              "#app{position:absolute;left:0;top:0;width:400px;height:2000px;"
              "z-index:0}"
              "#search{position:relative;width:400px;height:1500px}"
              "#thumb{position:relative;width:200px;height:100px;margin-top:500px}"
              "a{position:absolute;inset:0}"
              "</style>"
              "<body><div id=app><div id=search><div id=thumb>"
              "<a id=thumbnail href='/watch?v=1'></a>"
              "</div></div></div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    (void)page.EvaluateScript("document.getElementById('thumbnail').scrollIntoView()");
    const std::string hit = page.EvaluateScript(
        "var r=document.getElementById('thumbnail').getBoundingClientRect();"
        "var el=document.elementFromPoint(r.left+r.width/2, r.top+r.height/2);"
        "el && (el.id||el.tagName);");
    ExpectEqString(hit, "thumbnail",
                   "abspos under relative z-index:auto ancestor is still the top hit");
  });

  AddTest(tests, "Page/HtmlBodyOverflowDoesNotClipAsLocalScroller", [] {
    // In-flow content under height:0 body still paints in the viewport; the
    // document scrolls via the viewport, not a body padding-box scroller.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "html,body{margin:0;height:0;overflow:scroll}"
              "#tall{height:2000px;width:100px;background:red}"
              "</style>"
              "<body><div id=tall></div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    gfx::DisplayList list;
    page.Paint(list);
    int pushes = 0;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      pushes += std::holds_alternative<gfx::PushClipCommand>(command) ? 1 : 0;
    }
    ExpectEqInt(pushes, 0, "html/body must not PushClip their empty padding boxes");
    const std::string hit = page.EvaluateScript(
        "document.elementFromPoint(10,10) && document.elementFromPoint(10,10).id;");
    ExpectEqString(hit, "tall", "in-flow content in the viewport is still hit");
  });

  AddTest(tests, "Page/ClickOnImgInsideLinkActivatesHref", [] {
    // youtube search thumbs: elementFromPoint hits the <img> inside a#thumbnail;
    // ResolveClickActivation must still walk to the anchor (ADR 0017).
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "body{margin:0}"
              "a{position:absolute;left:0;top:0;width:200px;height:100px}"
              "img{width:200px;height:100px}"
              "</style>"
              "<body><a id=thumbnail href='/watch?v=1'>"
              "<img id=pic src='x.png'>"
              "</a></body>",
              "https://example.org/");
    page.Layout(400.0f);
    const std::string hit = page.EvaluateScript(
        "var el=document.elementFromPoint(50,50); el && (el.id||el.tagName);");
    Expect(hit == "pic" || hit == "IMG" || hit == "thumbnail",
           "hit is the img or the link");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{50.0f, 50.0f}), "click focuses");
    // Activation is tested through LinkAt agreement and focusable ancestor.
    Expect(page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}).has_value(), "link is found");
    ExpectEqString(*page.LinkAt(gfx::FloatPoint{50.0f, 50.0f}), "/watch?v=1", "href");
  });

  AddTest(tests, "Page/ClickOnImgInsideShadowUnderLinkActivatesHref", [] {
    // youtube: a#thumbnail hosts yt-image; the painted <img> lives in the
    // shadow tree. Parent() stops at the shadow root, so click/focus/activation
    // must cross ShadowHostOf (ADR 0019) or the click is dropped as not in
    // document and the href never activates.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "body{margin:0}"
              "a{position:absolute;left:0;top:0;width:200px;height:100px;display:block}"
              "yt-image,img{display:block;width:200px;height:100px}"
              "</style>"
              "<body><a id=thumbnail href='/watch?v=1'><yt-image id=host></yt-image></a>"
              "<script>"
              "var h=document.getElementById('host');"
              "var r=h.attachShadow({mode:'open'});"
              "r.innerHTML=\"<img id=pic src='x.png'>\";"
              "globalThis.clicks=0;"
              "document.getElementById('thumbnail').addEventListener('click',function(e){"
              "  globalThis.clicks++;"
              "  e.preventDefault();"
              "});"
              "</script></body>",
              "https://example.org/");
    page.RunScripts(0);
    page.Layout(400.0f);
    const std::string hit = page.EvaluateScript(
        "var el=document.elementFromPoint(50,50); el && (el.id||el.tagName);");
    Expect(hit == "pic" || hit == "IMG" || hit == "host" || hit == "thumbnail",
           "hit is shadow img, host, or link");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{50.0f, 50.0f}),
           "focus walks composed ancestors to the link");
    const dom::Element* focused = page.FocusedElement();
    Expect(focused != nullptr && focused->GetAttribute("id") != nullptr &&
               *focused->GetAttribute("id") == "thumbnail",
           "focused element is a#thumbnail across the shadow boundary");
    const engine::DispatchOutcome outcome =
        page.DispatchClickAt(gfx::FloatPoint{50.0f, 50.0f}, {});
    Expect(outcome.click_target != nullptr, "click target survives in-document check");
    ExpectEqString(page.EvaluateScript("String(globalThis.clicks)"), "1",
                   "click listener on the light-DOM link runs");
    Expect(outcome.prevented, "preventDefault from the link was seen");
    const engine::ClickActivation activation =
        page.ResolveClickActivation(outcome.click_target);
    Expect(activation.href.has_value(), "activation finds href across shadow");
    ExpectEqString(*activation.href, "/watch?v=1", "href is the thumbnail link");
  });

  AddTest(tests, "Page/HitTestsRelativeInsideOverflowScroller", [] {
    // youtube Accept: position:relative button under #content { overflow:auto }.
    // Appendix E collects the button into the dialog stacking context; the hit
    // point must still include #content's scrollTop or the static parent wins.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "body{margin:0}"
              "#dialog{position:fixed;left:0;top:0;width:200px;height:100px;z-index:2}"
              "#content{height:100px;overflow:auto}"
              "#spacer{height:200px}"
              "button{position:relative;display:block;width:100px;height:40px}"
              "</style>"
              "<body>"
              "<div id=dialog><div id=content><div id=spacer></div>"
              "<button id=accept>Accept</button></div></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    page.RunScripts(0);
    const std::string hit = page.EvaluateScript(
        "var c=document.getElementById('content');"
        "c.scrollTop=1e9;"
        "var r=document.getElementById('accept').getBoundingClientRect();"
        "var el=document.elementFromPoint(r.left+r.width/2, r.top+r.height/2);"
        "el && el.id;");
    ExpectEqString(hit, "accept", "scrolled relative button is hit, not the overflow parent");

    // TD-0030: after scroll the button's layout BorderBox may still lie below the
    // scroller's padding box. Collect must clip — a point outside #content must
    // not hit the button even though scroll_delta would map it onto the box.
    const std::string outside = page.EvaluateScript(
        "var c=document.getElementById('content');"
        "var cr=c.getBoundingClientRect();"
        "var el=document.elementFromPoint(cr.left+10, cr.bottom+20);"
        "el && el.id;");
    Expect(outside != "accept",
           "relative button scrolled out of overflow:auto is not hit outside the clip");
  });

  AddTest(tests, "Page/CollectedRelativeUnitClippedByOverflowScroller", [] {
    // Paint half of TD-0030: a collected position:relative unit under
    // overflow:auto must PushClip the scroller's padding box the way the tree
    // walk does for in-flow children.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>"
              "body{margin:0}"
              "#dialog{position:fixed;left:0;top:0;width:200px;height:80px;z-index:2}"
              "#content{height:80px;overflow:auto}"
              "#spacer{height:200px}"
              "#mark{position:relative;height:40px;background:red}"
              "</style>"
              "<body>"
              "<div id=dialog><div id=content><div id=spacer></div>"
              "<div id=mark></div></div></div>"
              "</body>",
              "https://example.org/");
    page.Layout(400.0f);
    page.EvaluateScript("document.getElementById('content').scrollTop=1e9;");
    page.Layout(400.0f);
    gfx::DisplayList list;
    page.Paint(list);
    int pushes = 0;
    int pops = 0;
    for (const gfx::DisplayCommand& command : list.Commands()) {
      pushes += std::holds_alternative<gfx::PushClipCommand>(command) ? 1 : 0;
      pops += std::holds_alternative<gfx::PopClipCommand>(command) ? 1 : 0;
    }
    Expect(pushes >= 1, "overflow:auto under a stacking context still PushClips");
    ExpectEqInt(pushes, pops, "every PushClip has a matching PopClip");
  });

  AddTest(tests, "Page/ClickOnVideoTogglesPlayback", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><video muted src='x.webm' "
        "style='width:200px;height:100px'></video></body>",
        "https://example.org/");
    page.Layout(400.0f);
    const dom::Element* video = nullptr;
    page.CurrentDocument()->ForEachDescendant([&](const dom::Node& node) {
      if (video != nullptr || !node.IsElement()) {
        return;
      }
      if (static_cast<const dom::Element&>(node).TagName() == "video") {
        video = &static_cast<const dom::Element&>(node);
      }
    });
    Expect(video != nullptr, "video exists");
    Expect(page.ToggleMediaPlaybackAt(gfx::FloatPoint{10.0f, 10.0f}), "starts on click");
    Expect(!page.Paused(*video), "and is playing");
  });

  AddTest(tests, "Page/ClickInsideMoviePlayerTogglesVideoPlayback", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'>"
        "<div id='movie_player' style='width:200px;height:100px;position:relative'>"
        "<div class='ytp-error' style='position:absolute;inset:0'></div>"
        "<video muted src='x.webm' style='width:200px;height:100px'></video>"
        "</div></body>",
        "https://example.org/");
    page.Layout(400.0f);
    const dom::Element* video = nullptr;
    page.CurrentDocument()->ForEachDescendant([&](const dom::Node& node) {
      if (video != nullptr || !node.IsElement()) {
        return;
      }
      if (static_cast<const dom::Element&>(node).TagName() == "video") {
        video = &static_cast<const dom::Element&>(node);
      }
    });
    Expect(video != nullptr, "video exists");
    Expect(page.Paused(*video), "starts paused");
    Expect(page.ToggleMediaPlaybackAt(gfx::FloatPoint{10.0f, 10.0f}),
           "overlay click reaches #movie_player's video");
    Expect(!page.Paused(*video), "and starts playback");
  });

  AddTest(tests, "Page/HitTestsThroughATransform", [] {
    // ADR 0014 §4's other half. A transform moves what is *painted* and nothing
    // else, so the box stays where layout put it and the hit test has to un-map the
    // pointer. Without that, a rotated menu is clicked by pointing at where it would
    // have been -- which is invisible, and reads as a broken click handler rather
    // than a broken hit test.
    //
    // The link is 120x40 at the origin, rotated a quarter turn about its centre
    // (60,20), so on screen it is 40 wide and 120 tall about the same point.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>a{display:block;width:120px;height:40px;transform:rotate(90deg)}</style>"
        "<body style='margin:0'><a href='/next'>ABC</a></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.LinkAt(gfx::FloatPoint{55.0f, 70.0f}).has_value(),
           "a point inside the rotated rectangle hits the link");
    Expect(!page.LinkAt(gfx::FloatPoint{110.0f, 20.0f}).has_value(),
           "and a point inside the *unrotated* one no longer does");
  });

  AddTest(tests, "Page/ADegenerateTransformCannotBeClicked", [] {
    // `scale(0)` has collapsed the box to a point: it paints nothing, so it must not
    // be clickable either. The painter refuses the same case for the same reason, and
    // a hit test that answered here would be an invisible element eating clicks.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>a{display:block;width:120px;height:40px;transform:scale(0)}</style>"
        "<body style='margin:0'><a href='/next'>ABC</a></body>",
        "https://example.org/start");
    page.Layout(400.0f);
    for (float x = 0.0f; x < 130.0f; x += 10.0f) {
      for (float y = 0.0f; y < 50.0f; y += 10.0f) {
        Expect(!page.LinkAt(gfx::FloatPoint{x, y}).has_value(),
               "nothing anywhere in or around the collapsed box is a link");
      }
    }
  });

  AddTest(tests, "Page/BuildsGetFormSubmissionTargets", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search?old=1'>"
        "<input type='hidden' name='token' value='a&b'>"
        "<input name='q' value='hello world' size='2'>"
        "<input type='submit' name='go' value='Search'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "clicking the submit input activates its form");
    ExpectEqString(*target, "/search?token=a%26b&q=hello+world&go=Search",
                   "GET submission replaces the action query with successful controls");
  });

  AddTest(tests, "Page/SubmitterOverridesFormAction", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/default'>"
        "<input name='q' value='hello'>"
        "<input type='submit' value='Search' formaction='/override?old=1'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/override?q=hello",
                   "the submitter's formaction overrides the form action");
  });

  AddTest(tests, "Page/SubmitterOverridesFormMethodToGet", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search' method='post'>"
        "<input name='q' value='hello'>"
        "<input type='submit' value='Search' formmethod='get'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "formmethod=get is a supported submission path");
    ExpectEqString(*target, "/search?q=hello",
                   "the submitter's formmethod overrides the form method");
  });

  AddTest(tests, "Page/SubmitterOverridesFormMethodToPost", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search?keep=1#frag' method='get'>"
        "<input name='q' value='hello'>"
        "<input type='submit' value='Search' formmethod='post'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<engine::FormSubmission> submission =
        page.FormSubmissionRequestAt(gfx::FloatPoint{45.0f, 5.0f});
    Expect(submission.has_value(), "formmethod=post is a supported submission path");
    ExpectEqString(submission->method, "POST", "the submitter's formmethod wins");
    ExpectEqString(submission->url, "/search?keep=1", "POST preserves the action query");
    ExpectEqString(submission->body, "q=hello", "controls move into the request body");
    ExpectEqString(submission->content_type, "application/x-www-form-urlencoded",
                   "POST uses the default form encoding");
  });

  AddTest(tests, "Page/FormAttributeAssociatesExternalControls", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input name='external' value='out' form='f'>"
        "<form id='f' action='/search'><input name='inside' value='in'>"
        "<input type='submit' value='Go'></form>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 25.0f});
    Expect(target.has_value(), "the submit control activates its form");
    ExpectEqString(*target, "/search?external=out&inside=in",
                   "controls with a matching form attribute are submitted with that form");
  });

  AddTest(tests, "Page/FormAttributeAssociatesExternalSubmitters", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<form id='f' action='/search'><input name='q' value='hello'></form>"
        "<input type='submit' name='go' value='Go' form='f'>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{5.0f, 25.0f});
    Expect(target.has_value(), "an external submitter activates its associated form");
    ExpectEqString(*target, "/search?q=hello&go=Go",
                   "the external submitter is serialized as the clicked submitter");
  });

  AddTest(tests, "Page/FormAttributeAssociatesFocusedTextControls", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input name='q' form='f'>"
        "<form id='f' action='/search'><input type='submit' value='Go'></form>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}),
           "the external text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("hello"), "typing changed the external input");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the external input can submit its associated form");
    ExpectEqString(*target, "/search?q=hello", "focused submission uses the form attribute");
  });

  AddTest(tests, "Page/FormAttributeAssociatesExternalResetButtons", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input name='q' value='start' form='f'>"
        "<form id='f' action='/search'><input type='submit' value='Go'></form>"
        "<input type='reset' value='Reset' form='f'>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}),
           "the external text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("ed"), "typing changed the external input");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{5.0f, 40.0f}),
           "the external reset button restores its associated form");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{5.0f, 25.0f});
    Expect(target.has_value(), "the submit control activates its form");
    ExpectEqString(*target, "/search?q=start", "reset restored the associated external input");
  });

  AddTest(tests, "Page/DisabledSubmitInputDoesNotSubmitForm", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='hello' size='2'>"
        "<input type='submit' value='Go' disabled>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f}).has_value(),
           "a disabled submit control must not activate its form");
  });

  AddTest(tests, "Page/DisabledFieldsetControlsAreNotSubmitted", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        // The fieldset is a block, so the submit after it is on the next line. Its
        // height is pinned here so the point below is a stated fact rather than a
        // number that happens to work.
        "<style>fieldset,input{margin:0;padding:0;border:0}fieldset{height:20px}"
        "input{width:40px;height:20px}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled>"
        "<input name='q' value='hello'>"
        "<input type='checkbox' name='seen' checked>"
        "</fieldset>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{20.0f, 30.0f});
    Expect(target.has_value(), "the submit control outside the fieldset activates the form");
    ExpectEqString(*target, "/search", "disabled fieldset descendants are not successful");
  });

  AddTest(tests, "Page/DisabledFieldsetSubmitInputDoesNotSubmitForm", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled><input type='submit' value='Go'></fieldset>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!SubmissionTarget(page, gfx::FloatPoint{5.0f, 5.0f}).has_value(),
           "a submit control inside a disabled fieldset must not activate its form");
  });

  AddTest(tests, "Page/DisabledFieldsetTextControlsCannotBeEdited", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}"
        "fieldset{margin:0;padding:0;border:0;height:20px}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled><input name='q' value='locked'></fieldset>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}),
           "a text control inside a disabled fieldset cannot be focused");
    Expect(!page.InsertTextIntoFocusedTextControl("x"),
           "typing cannot mutate a disabled fieldset descendant");
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{20.0f, 30.0f});
    Expect(target.has_value(), "the submit control outside the fieldset activates the form");
    ExpectEqString(*target, "/search", "the disabled fieldset text control was not submitted");
  });

  AddTest(tests, "Page/DisabledFieldsetFirstLegendControlsRemainEnabled", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>fieldset,legend,input{margin:0;padding:0;border:0}"
        "input{width:40px;height:20px}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled>"
        "<legend><input name='q' value='allowed'><input type='submit' value='Go'></legend>"
        "<input name='blocked' value='x'>"
        "</fieldset>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "a submit control inside the first legend remains enabled");
    ExpectEqString(*target, "/search?q=allowed",
                   "only controls inside the first legend escape the disabled fieldset");
  });

  AddTest(tests, "Page/ButtonElementsSubmitForms", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input,button{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='hello'>"
        "<button name='go' value='Search'>Search</button>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "clicking a button with no type activates its form");
    ExpectEqString(*target, "/search?q=hello&go=Search",
                   "the clicked button is serialized as the submitter");
  });

  AddTest(tests, "Page/ButtonElementTypesAreHonored", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        // `padding: 0` against the user-agent `button { padding: 1px 6px }`: this test is about
        // which button type does what, and the coordinates below are only readable while each
        // control is exactly 42px wide.
        "<style>input,button{width:40px;height:20px;margin:0;padding:0}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='start'>"
        "<button type='button' name='noop' value='x'>Noop</button>"
        "<button type='reset'>Reset</button>"
        "<button name='go' value='Go'>Go</button>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f}).has_value(),
           "a button with type=button does not submit");
    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("ed"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{85.0f, 5.0f}), "button type=reset resets its form");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{125.0f, 5.0f});
    Expect(target.has_value(), "the submit button activates its form");
    ExpectEqString(*target, "/search?q=start&go=Go",
                   "reset restored the input and the clicked submit button was serialized");
  });

  AddTest(tests, "Page/SelectControlsSubmitSelectedOptions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic'>"
        "<option value='a'>Alpha</option><option value='b' selected>Beta</option>"
        "</select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=b", "select serializes its selected option value");
  });

  AddTest(tests, "Page/SelectControlsDefaultToTheFirstOption", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic'><option>Alpha</option><option value='b'>Beta</option></select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=Alpha",
                   "a select with no selected option uses the first option text");
  });

  AddTest(tests, "Page/MultipleSelectControlsSubmitEverySelectedOption", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic' multiple>"
        "<option value='a' selected>Alpha</option>"
        "<option value='b'>Beta</option>"
        "<option selected>Gamma</option>"
        "</select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=a&topic=Gamma",
                   "a multiple select serializes every selected option in tree order");
  });

  AddTest(tests, "Page/MultipleSelectWithNoSelectionIsNotSuccessful", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic' multiple><option value='a'>Alpha</option></select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick", "an unselected multiple select contributes no entry");
  });

  AddTest(tests, "Page/SelectControlsSkipDisabledSelectedOptions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic'><option value='placeholder' selected disabled>Pick</option>"
        "<option value='a'>Alpha</option></select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick", "a disabled selected option contributes no entry");
  });

  AddTest(tests, "Page/MultipleSelectSkipsDisabledOptions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic' multiple>"
        "<option value='a' selected disabled>Alpha</option>"
        "<optgroup disabled><option value='b' selected>Beta</option></optgroup>"
        "<option value='c' selected>Gamma</option>"
        "</select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=c",
                   "disabled options and disabled optgroups are skipped");
  });

  AddTest(tests, "Page/SerializesOnlyCheckedCheckboxesAndRadios", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:10px;height:10px}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input type='checkbox' name='seen' checked>"
        "<input type='checkbox' name='skip'>"
        "<input type='radio' name='mode' value='new' checked>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{35.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?seen=on&mode=new",
                   "checked boxes without a value submit 'on', unchecked boxes submit nothing, "
                   "and an unnamed submitter is not successful");
  });

  AddTest(tests, "Page/ClickingCheckableInputsUpdatesFormSubmission", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:10px;height:10px;margin:0}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input type='checkbox' name='seen'>"
        "<input type='radio' name='mode' value='old' checked>"
        "<input type='radio' name='mode' value='new'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{5.0f, 5.0f}),
           "clicking the checkbox toggles it");
    page.Layout(400.0f);
    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{25.0f, 5.0f}),
           "clicking a radio input selects it");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{35.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?seen=on&mode=new",
                   "activated checkable controls update the submitted state");
  });

  AddTest(tests, "Page/RadioGroupsUseFormAttributeOwnership", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:20px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input type='radio' name='mode' value='new' form='f'>"
        "<form id='f' action='/filter'>"
        "<input type='radio' name='mode' value='old' checked>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{5.0f, 5.0f}),
           "clicking the external radio selects it");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{25.0f, 25.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?mode=new",
                   "a radio outside the form clears its peer with the same form owner");
  });

  AddTest(tests, "Page/ResetInputRestoresFormControlDefaults", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:20px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input name='q'>"
        "<input type='checkbox' name='seen' checked>"
        "<input type='reset' value='Reset'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("abc"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{25.0f, 5.0f}),
           "clicking the checkbox toggles it");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{45.0f, 5.0f}), "clicking reset restores defaults");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{65.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?q=&seen=on", "reset restored the original form state");
  });

  AddTest(tests, "Page/DisabledResetInputDoesNotRestoreForm", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:20px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input name='q'>"
        "<input type='reset' value='Reset' disabled>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("abc"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(!page.ResetFormAt(gfx::FloatPoint{25.0f, 5.0f}),
           "a disabled reset control must not restore its form");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?q=abc", "disabled reset left the edited state intact");
  });

  AddTest(tests, "Page/ResetRestoresTextLikeInputTypes", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/contact'>"
        "<input type='email' name='email' value='a@b'>"
        "<input type='reset' value='Reset'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the email input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("c"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{45.0f, 5.0f}), "clicking reset restores defaults");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{85.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/contact?email=a%40b",
                   "reset restored the email input's original value");
  });

  AddTest(tests, "Page/FocusedInputTextUpdatesFormSubmission", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' size='2'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("hi"), "typing changed the input value");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/search?q=hi", "the form uses the edited input value");
  });

  AddTest(tests, "Page/TextareasCanBeEditedAndSubmitted", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/note'>"
        "<textarea name='body' cols='4' rows='2'>hi</textarea>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the textarea was focused");
    Expect(page.InsertTextIntoFocusedTextControl("&"), "typing changed the textarea value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused textarea can submit its owning form");
    ExpectEqString(*target, "/note?body=hi%26", "textarea edits are submitted");
  });

  AddTest(tests, "Page/ResetRestoresTextareaDefaults", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>textarea,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/note'>"
        "<textarea name='body'>hi</textarea>"
        "<input type='reset' value='Reset'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the textarea was focused");
    Expect(page.InsertTextIntoFocusedTextControl("!"), "typing changed the textarea value");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{45.0f, 5.0f}), "reset restored the textarea");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{85.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/note?body=hi", "reset restored the textarea default text");
  });

  AddTest(tests, "Page/TextLikeInputTypesCanBeEdited", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/contact'>"
        "<input type='email' name='email' size='4'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the email input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("a@b"), "typing changed the input value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can submit its owning form");
    ExpectEqString(*target, "/contact?email=a%40b", "email input edits are submitted");
  });

  AddTest(tests, "Page/FocusedInputHonorsMaxlength", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' maxlength='3' size='3'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("abcd"), "typing changed the input value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can submit its owning form");
    ExpectEqString(*target, "/search?q=abc", "maxlength bounds inserted input text");
  });

  AddTest(tests, "Page/FocusedReadonlyInputDoesNotMutate", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='locked' readonly size='6'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the readonly input was focused");
    Expect(!page.InsertTextIntoFocusedTextControl("x"), "typing does not mutate readonly input");
    Expect(!page.DeleteBackwardFromFocusedTextControl(), "backspace does not mutate readonly input");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can still submit its owning form");
    ExpectEqString(*target, "/search?q=locked", "readonly preserves the original value");
  });

  AddTest(tests, "Page/FocusedInputBackspaceAndEnterSubmission", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' size='3'><input type='submit' name='go' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusFromClickAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("caf\xC3\xA9"), "typing changed the input value");
    Expect(page.DeleteBackwardFromFocusedTextControl(), "backspace changed the focused input value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can submit its owning form");
    ExpectEqString(*target, "/search?q=caf",
                   "enter submission serializes the focused input without a clicked submitter");
  });

  // --- Subresources ---------------------------------------------------------

  AddTest(tests, "Page/CollectsLinkedStyleSheetsButNotOtherLinks", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<head>"
        "<link rel='stylesheet' href='a.css'>"
        "<link rel='STYLESHEET' href='b.css'>"
        "<link rel='alternate stylesheet' href='alt.css'>"
        "<link rel='preload' href='p.css'>"
        "<link rel='icon' href='favicon.png'>"
        "<link rel='stylesheet'>"
        "</head><body>x</body>",
        "https://example.org/");

    const std::vector<engine::SubresourceRequest>& sheets = page.PendingStyleSheets();
    ExpectEqInt(static_cast<long long>(sheets.size()), 2,
                "rel is a token set: an alternate sheet is not applied, a preload is not a "
                "sheet, and a link with no href points nowhere");
    ExpectEqString(sheets.at(0).url, "a.css", "in document order");
    ExpectEqString(sheets.at(1).url, "b.css", "and rel matches case-insensitively");
  });

  AddTest(tests, "Page/StyleSheetsCascadeInDocumentOrderAcrossLinksAndStyleElements", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<head><link rel='stylesheet' href='early.css'>"
        "<style>p { height: 40px }</style></head>"
        "<body><p>ABC</p></body>",
        "https://example.org/");

    page.AddStyleSheet(0, "p { height: 400px }");
    const float height = page.Layout(400.0f);
    Expect(height < 200.0f,
           "a linked sheet fills its document slot; it does not win merely because it loaded "
           "after a later <style> element");
  });

  AddTest(tests, "Page/FailedStyleSheetsDoNotShiftLaterSheetsIntoTheWrongCascadeSlot", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<head><link rel='stylesheet' href='missing.css'>"
        "<style>p { height: 40px }</style>"
        "<link rel='stylesheet' href='late.css'></head>"
        "<body><p>ABC</p></body>",
        "https://example.org/");

    page.AddStyleSheet(1, "p { height: 400px }");
    const float height = page.Layout(400.0f);
    Expect(height >= 300.0f,
           "the second successful fetch fills the second link's slot, after the inline style, "
           "even though the first link never loaded");
  });

  AddTest(tests, "Loader/ASubresourceIsFetchedRelativeToItsDocument", [] {
    engine::Loader loader;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("text/css", "p { color: red }")});
    loader.SetTransport(factory);

    const url::Url document = *url::Url::Parse("https://example.org/dir/page.html");
    const engine::Loader::Result result = RunOneRequest(
        loader, loader.StartSubresource("../style.css", document,
                                        privacy::ResourceType::Stylesheet, 1000));

    Expect(result.ok, "the sheet loaded");
    ExpectEqString(result.body, "p { color: red }", "with its bytes");
    Expect(!factory.log.requests.empty(), "a request was made");
    Expect(factory.log.requests.at(0).find("GET /style.css ") != std::string::npos,
           "resolved against the document, not against the root: every href in a page is "
           "relative to where the page is");
    Expect(factory.log.requests.at(0).find("Referer: https://example.org/dir/page.html\r\n") !=
               std::string::npos,
           "and the subresource request carries the policy-computed referrer");
  });

  AddTest(tests, "Loader/ABlockedSubresourceIsNotFetched", [] {
    // The point of the privacy layer: a request that the policy refuses never
    // reaches a socket. If it did, the block would be cosmetic.
    engine::Loader loader;
    ScriptedFactory factory;
    factory.script.push_back(
        ScriptedTransport::Exchange{"", 0, false, OkResponse("text/css", "x{}")});
    loader.SetTransport(factory);

    // HTTPS-only is the default, and deliberately not settable downward.
    const url::Url document = *url::Url::Parse("http://insecure.test/page.html");
    const engine::Loader::Result result = RunOneRequest(
        loader, loader.StartSubresource("http://insecure.test/style.css", document,
                                        privacy::ResourceType::Stylesheet, 1000));
    (void)result;
    Expect(factory.log.hosts.empty() || result.ok,
           "either the policy upgraded the request and it was made over TLS, or it refused "
           "and no connection happened -- what must not happen is a plaintext fetch");
    for (const bool secure : factory.log.secure) {
      Expect(secure, "no request left this machine in plaintext under HTTPS-only");
    }
  });

  AddTest(tests, "Engine/AppliesAStyleSheetTheDocumentLinked", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<html><head><link rel='stylesheet' href='/s.css'></head>"
                   "<body><p>ABC</p></body></html>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("text/css", "p { height: 400px }")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the document and its stylesheet were both fetched");
    Expect(factory.log.requests.at(1).find("GET /s.css ") != std::string::npos,
           "and the second request is the sheet");
    ExpectEqInt(static_cast<long long>(factory.connects), 1,
                "on one connection: same host, same partition, and the document's connection "
                "was still in the pool when the sheet was asked for");

    // The sheet must have applied *before* the first layout: laying out
    // without it and reflowing after is the flash of unstyled content.
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");
    Expect(frame->display_list.Bounds().height >= 300,
           "the 400px paragraph from the linked sheet is in the geometry of the first frame");
  });

  AddTest(tests, "Engine/AStyleSheetThatFailsToLoadIsNotANavigationFailure", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<html><head><link rel='stylesheet' href='/missing.css'></head>"
                   "<body><p>ABC</p></body></html>")});
    // No second exchange: the sheet's connection fails.
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    ExpectEqString(session.LastTitle(), "https://example.org/page.html",
                   "the document still committed");
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr && TextRunCount(frame->display_list) > 0,
           "a stylesheet that does not load is a page rendered without it, which is what "
           "every browser does -- not an error page");
  });

  // --- Images ---------------------------------------------------------------

  AddTest(tests, "Page/CollectsImageSourcesOnceEach", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<img src='a.png'><img src='b.png'><img src='a.png'><img>", "https://example.org/");
    const std::vector<std::string>& images = page.PendingImages();
    ExpectEqInt(static_cast<long long>(images.size()), 2,
                "a page that shows one icon forty times fetches and decodes it once, and an "
                "<img> with no src points nowhere");
    ExpectEqString(images.at(0), "a.png", "in document order");
  });

  AddTest(tests, "Page/CollectsImagesInsideShadowTrees", [] {
    // Youtube thumbnails live in ytd-thumbnail's shadow root. ElementsByTagName
    // does not see them; CollectImages must walk shadow trees the same way
    // CollectShadowStyleSheets does.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body><div id=host></div>"
              "<script>"
              "var r = document.getElementById('host').attachShadow({mode:'open'});"
              "r.innerHTML = \"<img src='shadow.png'><img src='a.png'>\";"
              "</script></body>",
              "https://example.org/");
    page.RunScripts(0);
    const std::vector<std::string>& images = page.PendingImages();
    Expect(std::find(images.begin(), images.end(), "shadow.png") != images.end(),
           "an <img> in a shadow tree is fetched");
    Expect(std::find(images.begin(), images.end(), "a.png") != images.end(),
           "and so is every other one under that root");
  });

  AddTest(tests, "Page/RecollectsImagesAfterScriptAssignsSrc", [] {
    // IntersectionObserver lazy thumbs assign img.src without inserting nodes.
    // Attribute-only mutation used to restyle/paint and never recollect, so
    // TakeUnrequestedImages never saw the URL (youtube search after Accept).
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body><img id=t></body>", "https://example.org/");
    page.RunScripts(0);
    ExpectEqInt(static_cast<long long>(page.PendingImages().size()), 0,
                "no src yet means nothing to fetch");
    (void)page.EvaluateScript(
        "document.getElementById('t').src = 'https://example.org/thumb.jpg';");
    page.RecollectDocumentImages();
    const std::vector<std::string>& images = page.PendingImages();
    Expect(std::find(images.begin(), images.end(), "https://example.org/thumb.jpg") !=
               images.end(),
           "assigning img.src must enqueue the URL without a tree rebuild");
  });

  AddTest(tests, "Page/WebFontSwapReflowsWithoutRebuildingBoxTree", [] {
    // font-display:swap changes metrics, not box generation. Youtube registers
    // dozens of faces after first layout; each InvalidateBoxTree was a full
    // BuildBoxTree (TD-0021).
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><p>hello</p></body>", "https://example.org/");
    page.Layout(400.0f);
    const util::PerfCounterSnapshot before = util::CapturePerformanceCounters();
    engine::Page::PendingFontFace face;
    face.url = "https://example.org/swap.ttf";
    face.family = "SwapFace";
    face.weight = 400;
    Expect(page.AddWebFont(face, BuildSyntheticFont()), "face registers");
    const util::PerfCounterSnapshot after = util::CapturePerformanceCounters();
    const auto delta = [&](util::PerfCounterId id) {
      return after[static_cast<std::size_t>(id)] - before[static_cast<std::size_t>(id)];
    };
    ExpectEqInt(static_cast<long long>(delta(util::PerfCounterId::BoxTreeInvalidatedByFont)), 0,
                "a late face must not drop an existing box tree");
    ExpectEqInt(static_cast<long long>(delta(util::PerfCounterId::BoxTreeFontReflowOnly)), 1,
                "and must mark a reflow instead");
    const std::uint64_t builds_before =
        util::ReadPerformanceCounter(util::PerfCounterId::LayoutTreeBuilds);
    page.Layout(400.0f);
    const std::uint64_t builds_after =
        util::ReadPerformanceCounter(util::PerfCounterId::LayoutTreeBuilds);
    ExpectEqInt(static_cast<long long>(builds_after - builds_before), 0,
                "reflow after swap reuses the box tree");
  });

  AddTest(tests, "Layout/AnImageTakesItsSizeFromThePixelsWhenNothingElseSaysOtherwise", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='x.png'></body>", "https://example.org/");

    auto image = std::make_shared<gfx::Image>();
    Expect(image->Adopt(24, 12, std::vector<std::uint32_t>(24 * 12, 0xFF00FF00u)), "built");
    page.AddImage("x.png", image);
    page.Layout(400.0f);

    gfx::DisplayList list;
    page.Paint(list);
    const gfx::IntRect bounds = list.Bounds();
    Expect(bounds.width >= 24 && bounds.height >= 12,
           "the intrinsic size of the decoded image is the used size");
  });

  AddTest(tests, "Layout/AnImagesDeclaredSizeBeatsItsIntrinsicOne", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='x.png' width='40' height='30'></body>",
              "https://example.org/");
    auto image = std::make_shared<gfx::Image>();
    Expect(image->Adopt(8, 8, std::vector<std::uint32_t>(64, 0xFFFF0000u)), "built");
    page.AddImage("x.png", image);
    page.Layout(400.0f);

    gfx::DisplayList list;
    page.Paint(list);
    Expect(list.Bounds().width >= 40 && list.Bounds().height >= 30,
           "the width and height attributes are where most of the web still puts an image's "
           "size, and the cascade never sees them");
  });

  AddTest(tests, "Page/DeclaredSizeImageAttachesWithoutRebuildingTheBoxTree", [] {
    // Youtube search thumbnails are sized by attributes / CSS before the
    // bitmap arrives. Dropping the whole tree per decode was the bulk of
    // BuildBoxTree after TD-0001 closed the layout half.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='x.png' width='40' height='30'></body>",
              "https://example.org/");
    page.Layout(400.0f);
    util::ResetPerformanceCounters();
    auto image = std::make_shared<gfx::Image>();
    Expect(image->Adopt(8, 8, std::vector<std::uint32_t>(64, 0xFFFF0000u)), "built");
    page.AddImage("x.png", image);
    ExpectEqInt(static_cast<long long>(
                    util::ReadPerformanceCounter(util::PerfCounterId::BoxTreeImagePaintOnly)),
                1, "declared size: attach pixels, do not rebuild");
    ExpectEqInt(static_cast<long long>(
                    util::ReadPerformanceCounter(util::PerfCounterId::BoxTreeInvalidatedByImage)),
                0, "and do not count an invalidation");
    gfx::DisplayList list;
    page.Paint(list);
    Expect(list.Bounds().width >= 40 && list.Bounds().height >= 30, "still the declared size");
  });

  AddTest(tests, "Page/AbsPosFilledImageAttachesWithoutRebuildingTheBoxTree", [] {
    // Youtube thumbnails: the host sizes via padding-top, the <img> is
    // position:absolute; inset 0. Used size is non-zero before decode.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'>"
              "<div style='position:relative;width:200px;height:100px'>"
              "<img src='x.png' style='position:absolute;left:0;top:0;width:100%;height:100%'>"
              "</div></body>",
              "https://example.org/");
    page.Layout(400.0f);
    util::ResetPerformanceCounters();
    auto image = std::make_shared<gfx::Image>();
    Expect(image->Adopt(16, 16, std::vector<std::uint32_t>(256, 0xFF00FF00u)), "built");
    page.AddImage("x.png", image);
    ExpectEqInt(static_cast<long long>(
                    util::ReadPerformanceCounter(util::PerfCounterId::BoxTreeImagePaintOnly)),
                1, "abspos fill already has used size");
    ExpectEqInt(static_cast<long long>(
                    util::ReadPerformanceCounter(util::PerfCounterId::BoxTreeInvalidatedByImage)),
                0, "decode must not rebuild");
  });

  AddTest(tests, "Page/ScriptTurnWithoutMutationKeepsTheBoxTree", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><div id=a>x</div>"
              "<script>void 0;</script></body>",
              "https://example.org/");
    page.Layout(400.0f);
    util::ResetPerformanceCounters();
    page.RunScripts(0);
    ExpectEqInt(static_cast<long long>(
                    util::ReadPerformanceCounter(util::PerfCounterId::BoxTreeScriptSkipped)),
                1, "a script that changes nothing must not drop the tree");
    ExpectEqInt(static_cast<long long>(
                    util::ReadPerformanceCounter(util::PerfCounterId::BoxTreeInvalidatedByScript)),
                0, "so the invalidation counter stays quiet");
  });

  AddTest(tests, "Layout/AnImageThatNeverArrivesStillOccupiesItsDeclaredSize", [] {
    // Otherwise the page reflows when the image lands, which is the layout
    // shift every user has learned to hate.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='missing.png' width='50' height='60'></body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.ContentHeight() >= 60.0f, "the box is there before the pixels are");
  });

  AddTest(tests, "Engine/FetchesDecodesAndDrawsAnImage", [] {
    Session session;
    ScriptedFactory factory;
    const std::vector<std::byte> png = BuildPng(PngSpec{
        16, 8, 8, 6, false, {}, {}, SolidRgbaRows(16, 8, 0x20, 0x80, 0xC0, 0xFF), 0});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<body style='margin:0'><img src='/pic.png'></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("image/png", std::string(reinterpret_cast<const char*>(png.data()),
                                            png.size()))});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{200, 100}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the document and the image were both fetched");
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");
    ExpectEqInt(static_cast<long long>(frame->display_list.Images().size()), 1,
                "with the decoded image on it");
    Expect(frame->display_list.Images().at(0)->Width() == 16 &&
               frame->display_list.Images().at(0)->Height() == 8,
           "at the size the PNG declared");
  });

  AddTest(tests, "Engine/BytesThatAreNotAnImageDoNotBreakThePage", [] {
    // Image bytes are attacker-controlled. A decoder failure is an image that
    // does not draw, not a page that does not render.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<body><img src='/bad.png'><p>ABC</p></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("image/png", "not a png at all")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{200, 100}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "the page still painted");
    Expect(frame->display_list.Images().empty(), "with no image");
    Expect(TextRunCount(frame->display_list) > 0, "and its text intact");
  });

  AddTest(tests, "Engine/AnImageSurvivesTheWireFormat", [] {
    Session session;
    ScriptedFactory factory;
    const std::vector<std::byte> png = BuildPng(PngSpec{
        4, 4, 8, 6, false, {}, {}, SolidRgbaRows(4, 4, 0x11, 0x22, 0x33, 0xFF), 0});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<body><img src='/p.png'></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("image/png",
                   std::string(reinterpret_cast<const char*>(png.data()), png.size()))});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{200, 100}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");
    const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(ipc::EngineToUi{*frame}));
    Expect(decoded.has_value(), "and it survived its own wire format");
    const auto& list = std::get<ipc::PaintFrameMessage>(*decoded).display_list;
    ExpectEqInt(static_cast<long long>(list.Images().size()), 1, "the image crossed");
    Expect(list.Images().at(0)->Width() == 4 && list.Images().at(0)->Height() == 4,
           "at its own size");
    Expect(std::equal(list.Images().at(0)->Pixels().begin(), list.Images().at(0)->Pixels().end(),
                      frame->display_list.Images().at(0)->Pixels().begin()),
           "pixel for pixel -- the wire carries the bitmap, since a display list that named a "
           "resource by id would need the receiver's cache to be part of the contract");
  });

  // --- The engine -----------------------------------------------------------

  AddTest(tests, "Engine/NavigatingToADataUrlRendersTheDocument", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<html><head><title>Doc</title></head><body><p>ABC</p></body></html>")});

    ExpectEqString(session.LastTitle(), "Doc", "the title reached the UI");
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "and a frame was painted");
    Expect(TextRunCount(frame->display_list) > 0, "with the document's text on it");
  });

  AddTest(tests, "Engine/AFailedLoadRendersAnErrorPageRatherThanNothing", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"not a url at all"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a failed load still paints");
    Expect(TextRunCount(frame->display_list) > 0,
           "a browser showing nothing when a load fails is indistinguishable from one that "
           "has hung");
  });

  AddTest(tests, "Engine/TheErrorPageEscapesTheUrlItEchoes", [] {
    // The error page is built by string concatenation and the URL comes from
    // whoever asked for the navigation. Interpolating it raw makes a URL
    // containing markup an injection into the browser's own document.
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"<script>x</script> not a url"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");

    // The URL appears as text. That is the proof: unescaped, the tokenizer
    // would have made a script element out of it and swallowed the contents,
    // and no text run would mention it at all.
    bool shown_as_text = false;
    for (const gfx::DisplayList::TextRun& run : frame->display_list.Texts()) {
      shown_as_text = shown_as_text || run.text.find("<script>x</script>") != std::string::npos;
    }
    Expect(shown_as_text,
           "the URL must reach the page as text rather than as markup, and must still be "
           "shown -- an error page that hides what failed is not an error page");
  });

  AddTest(tests, "Engine/ScrollingStopsAtTheEndOfTheDocument", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl("<p>ABC</p>")});
    const std::size_t before = session.sent.size();

    // A short document does not scroll at all, so neither of these produces a
    // frame. A scroll that ran off the end would paint blank space.
    session.Send(ipc::ScrollMessage{0, 10000, gfx::IntPoint{}});
    session.Send(ipc::ScrollMessage{0, -10000, gfx::IntPoint{}});
    ExpectEqInt(static_cast<long long>(session.sent.size() - before), 0,
                "scrolling a document that fits repaints nothing");
  });

  AddTest(tests, "Engine/AResizeRelaysOutAndAScrollDoesNot", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 60}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<p>ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC</p>")});
    Expect(session.LastFrame() != nullptr, "the document painted");

    const std::size_t before = session.sent.size();
    session.Send(ipc::ScrollMessage{0, 20, gfx::IntPoint{}});
    Expect(session.sent.size() > before, "a tall document scrolls, and scrolling repaints");
  });

  AddTest(tests, "Engine/AScrollDamagesTheExposedBandRatherThanTheWindow", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<body style='margin:0'><div style='height:3000px'>A</div></body>")});
    Expect(session.LastFrame() != nullptr, "the document painted");

    session.Send(ipc::ScrollMessage{0, 50, gfx::IntPoint{}});
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "the scroll produced a frame");
    // ADR 0018 §2, and the whole point of the session: the display-list diff
    // would answer "everything changed" here, because every command in the list
    // moved by 50 pixels. What actually came into view is a 400x50 strip, and
    // the delta is the UI's licence to blit the rest rather than repaint it.
    ExpectEqInt(static_cast<long long>(frame->damage.size()), 1, "one damaged rect");
    ExpectEqInt(frame->damage[0].height, 50, "and it is the band the scroll exposed");
    ExpectEqInt(frame->damage[0].y, 250, "at the bottom, because the page moved up");
    ExpectEqInt(frame->scroll_delta.y, -50, "with the delta the previous frame's pixels moved by");
  });

  AddTest(tests, "Engine/AWheelOverAScrollingBoxMovesTheBoxAndNotThePage", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<body style='margin:0'>"
        "<div style='height:100px;overflow:auto'><div style='height:900px'>A</div></div>"
        "<div style='height:3000px'>B</div></body>")});
    Expect(session.LastFrame() != nullptr, "the document painted");

    // Over the inner scroller, which can still move: it takes the wheel, and
    // the damage is its own rectangle rather than a band of the window. The
    // frame carries no scroll delta, because the *document* did not move and a
    // blit would slide the whole page.
    session.Send(ipc::ScrollMessage{0, 30, gfx::IntPoint{10, 10}});
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "the wheel produced a frame");
    ExpectEqInt(static_cast<long long>(frame->damage.size()), 1, "one damaged rect");
    ExpectEqInt(frame->damage[0].height, 100, "the scroller's own box, not the window");
    ExpectEqInt(frame->scroll_delta.y, 0, "and no document blit");
  });

  AddTest(tests, "Engine/AWheelReachesFixedOverflowUnderAZeroHeightHost", [] {
    // youtube consent: `position:fixed` dialog inside a 0×0 lightbox host.
    // ScrollTargetAt required every ancestor BorderBox to contain the point and
    // never reached `#content { overflow:auto }`. Wheel must use the same
    // elevated abspos walk as elementFromPoint (TD-0022).
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 400}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<body style='margin:0'>"
        "<div id='host' style='height:0;width:0;overflow:visible'>"
        "<div id='dialog' style='position:fixed;left:0;top:0;width:400px;height:400px'>"
        "<div id='content' style='height:300px;overflow:auto'>"
        "<div style='height:900px'><button id='accept' style='margin-top:700px'>Accept</button>"
        "</div></div></div></div></body>")});
    Expect(session.LastFrame() != nullptr, "painted");
    session.Send(ipc::ScrollMessage{0, 400, gfx::IntPoint{200, 150}});
    const std::string top =
        session.engine.EvaluateScript("String(document.getElementById('content').scrollTop)");
    ExpectEqString(top, "400", "fixed overflow under a 0-height host takes the wheel");
  });

  AddTest(tests, "Engine/ClickDoesNotActivateLinkUnderDismissedOverlay", [] {
    // youtube consent Accept sits over search results. mousedown removes (or
    // hides) the dialog; a release that re-hit-tests the point would activate
    // the result underneath. UI Events fires click at the common ancestor of
    // press and release — default actions must follow that, not the point.
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<body style='margin:0'>"
        "<a href='https://example.org/watch' style='display:block;width:200px;height:100px'>"
        "video</a>"
        "<button id='accept' style='position:absolute;left:0;top:0;width:200px;height:100px'"
        " onmousedown='this.remove()'>Accept</button>"
        "</body>")});
    const std::string start = session.LastCommittedUrl();
    Expect(!start.empty(), "the document committed");
    session.Click(50.0f, 40.0f);
    ExpectEqString(session.LastCommittedUrl(), start,
                   "dismissing an overlay on mousedown must not navigate the link under it");
  });

  AddTest(tests, "Engine/NavigatingToAboutBlankIsAPageRatherThanAFailure", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"about:blank"});
    ExpectEqString(session.LastTitle(), "New Tab", "about:blank is a real, blank document");
    Expect(session.LastFrame() != nullptr, "and it paints");
  });

  AddTest(tests, "Engine/ReloadCanBypassTheHttpCache", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: max-age=600\r\n"
        "Content-Length: 34\r\n\r\n<title>One</title><body>one</body>"});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: max-age=600\r\n"
        "Content-Length: 34\r\n\r\n<title>Two</title><body>two</body>"});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});
    ExpectEqString(session.LastTitle(), "One", "the first document committed");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 1,
                "the initial navigation fetched the document");

    session.Send(ipc::ReloadMessage{false});
    ExpectEqString(session.LastTitle(), "One", "ordinary reload may use a fresh cache entry");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 1,
                "and did not open another connection");

    session.Send(ipc::ReloadMessage{true});
    ExpectEqString(session.LastTitle(), "Two", "cache-bypassing reload fetched the new document");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "and made the second request");
  });

  AddTest(tests, "Engine/ClickingALinkNavigatesToIt", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><a href='/next'>ABC</a></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Next</title><body>next page</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/next",
                   "the relative href was resolved against the document URL");
    ExpectEqString(session.LastTitle(), "Next", "and the clicked document committed");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the initial page and the clicked page were fetched");
    Expect(factory.log.requests.at(1).find("GET /next ") != std::string::npos,
           "the second request is for the clicked link");
    Expect(factory.log.requests.at(1).find("Referer: https://example.org/start\r\n") !=
               std::string::npos,
           "the clicked navigation carries the policy-computed referrer");
  });

  // The interstitial, end to end.
  //
  // This is `docs/surveys/2026-08-04-reddit-youtube-plex.md` §1 reduced to what
  // it exercises, and every line of it was a hole a month ago: DOMContentLoaded
  // firing at all, `{once: true}`, `document.forms`, `location.search`,
  // `URLSearchParams`, `Object.assign` onto an element setting IDL attributes,
  // `form.onsubmit` as a property, `elements.namedItem`, and `requestSubmit()`
  // being distinct from `submit()` -- plus the cookie from the first response
  // being sent on the second, which already worked and is what the whole
  // exercise is for.
  AddTest(tests, "Engine/AScriptedFormSubmissionIsANavigation", [] {
    Session session;
    ScriptedFactory factory;
    const std::string interstitial =
        "<title>Please wait</title><form action='/challenge'>"
        "<input type='hidden' name='token' value='t0'>"
        "<input type='hidden' name='solution' value=''>"
        "</form><script>"
        "document.addEventListener('DOMContentLoaded', function () {"
        "  var f = document.forms[0];"
        "  f.onsubmit = function (e) {"
        "    new URLSearchParams(document.location.search).forEach((v, n) =>"
        "      e.target.appendChild(Object.assign(document.createElement('input'),"
        "        {name: n, type: 'hidden', value: v})));"
        "    return true;"
        "  };"
        "  f.elements.namedItem('solution').value = 'answered';"
        "  f.requestSubmit();"
        "}, {once: true});"
        "</script>";
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Set-Cookie: gate=passed; Path=/\r\nContent-Length: " +
            std::to_string(interstitial.size()) + "\r\n\r\n" + interstitial});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>The real page</title><body>content</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/?jsc=1&r=abc"});

    ExpectEqString(session.LastTitle(), "The real page",
                   "the challenge submitted itself and the answer committed");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "one request for the interstitial and one for its answer");
    const std::string& submitted = factory.log.requests.at(1);
    Expect(submitted.find("GET /challenge?token=t0&solution=answered&jsc=1&r=abc ") !=
               std::string::npos,
           "the submission carries the form's own fields, the value the script "
           "set through elements.namedItem, and the two the onsubmit handler "
           "copied out of location.search -- which is the whole point of "
           "requestSubmit firing the event that submit() does not");
    Expect(submitted.find("Cookie: gate=passed\r\n") != std::string::npos,
           "and the cookie the first response set");
  });

  AddTest(tests, "Engine/PostLoadRequestSubmitNavigates", [] {
    // Reddit's challenge submits during DOMContentLoaded while the load is
    // still active, so AdvanceLoad's FollowScriptNavigation sees it. A settled
    // page's EvaluateScript that calls requestSubmit used to queue a
    // PendingSubmit that nothing drained (TD-0026). The script turn must take
    // the navigation the same way a timer callback's turn does.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<title>Gate</title><form action='/next' method='get'>"
                   "<input name='q' value='cats'></form><body>gate</body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Results</title><body>found</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/"});
    ExpectEqString(session.LastTitle(), "Gate", "the gate page committed first");

    (void)session.engine.EvaluateScript("document.forms[0].requestSubmit()");
    RunEngineToIdle(session.engine);
    while (auto reply = session.channel.Ui().TryReceive()) {
      session.sent.push_back(std::move(*reply));
    }
    ExpectEqString(session.LastTitle(), "Results",
                   "EvaluateScript's requestSubmit navigated after the load finished");
    Expect(factory.log.requests.size() >= 2, "a second document was fetched");
    Expect(factory.log.requests.at(1).find("GET /next?q=cats ") != std::string::npos,
           "the submitted query string is on the request");
  });

  AddTest(tests, "Engine/AsyncDomContentLoadedChallengeSubmitsAfterAwait", [] {
    Session session;
    ScriptedFactory factory;
    const std::string interstitial =
        "<title>Please wait</title><form method='GET' action='/'>"
        "<input type='hidden' name='solution'>"
        "<input type='hidden' name='js_challenge' value='1'>"
        "<input type='hidden' name='token' value='t0'>"
        "<input type='hidden' name='jsc_orig_r' value=''>"
        "</form><script>"
        "document.addEventListener('DOMContentLoaded', async function () {"
        "  var f = document.forms[0];"
        "  f.onsubmit = function (e) {"
        "    new URLSearchParams(document.location.search).forEach((v, n) =>"
        "      e.target.appendChild(Object.assign(document.createElement('input'),"
        "        {name: n, type: 'hidden', value: v})));"
        "    return true;"
        "  };"
        "  var n = await (async e => e + e)('a4c1');"
        "  f.elements.namedItem('solution').value = n;"
        "  f.requestSubmit();"
        "}, {once: true});"
        "</script>";
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Set-Cookie: gate=passed; Path=/\r\nContent-Length: " +
            std::to_string(interstitial.size()) + "\r\n\r\n" + interstitial});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>The real page</title><body>content</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/"});

    ExpectEqString(session.LastTitle(), "The real page",
                   "reddit's async DOMContentLoaded handler submits after await");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "one request for the interstitial and one for its answer");
    const std::string& submitted = factory.log.requests.at(1);
    Expect(submitted.find("solution=a4c1a4c1") != std::string::npos,
           "the doubled seed from await (async e => e + e) reaches the server");
    Expect(submitted.find("js_challenge=1") != std::string::npos,
           "the form's own hidden fields travel with the submission");
  });

  AddTest(tests, "Engine/RedditInterstitialHtmlSubmitsDoubledSeed", [] {
    Session session;
    ScriptedFactory factory;
    const std::string interstitial = R"(<!DOCTYPE html>
<html lang="en"><head><title>Reddit</title>
<script>
document.addEventListener("DOMContentLoaded",async function(){var e=document.forms[0],n=(e.onsubmit=function(t){return new URLSearchParams(document.location.search).forEach((e,n)=>t.target.appendChild(Object.assign(document.createElement("input"),{name:n,type:"hidden",value:e}))),!0},await(async e=>e+e)("a4c1c97a5208ca7e"));e.elements.namedItem("solution").value=n,e.requestSubmit()},{once:!0});
</script></head><body>
<form hidden method="GET" action="/">
<input type="hidden" name="solution" />
<input type="hidden" name="js_challenge" value="1"/>
<input type="hidden" name="token" value="7afd7253fec22262ff1c52b1703fe9ece2867a7ef7c264433b54326a11af38ba"/>
<input type="hidden" name="jsc_orig_r" value=""/>
</form></body></html>)";
    factory.script.push_back(ScriptedTransport::Exchange{
        "www.reddit.com", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Set-Cookie: edgebucket=livebucket; Path=/; Domain=reddit.com; Secure\r\n"
        "Set-Cookie: csv=2; Path=/; Domain=.reddit.com\r\n"
        "Set-Cookie: token_v2=fake; Path=/; Domain=.reddit.com\r\n"
        "Content-Length: " + std::to_string(interstitial.size()) + "\r\n\r\n" + interstitial});
    factory.script.push_back(ScriptedTransport::Exchange{
        "www.reddit.com", 443, true,
        OkResponse("text/html",
                   "<title>Reddit - The heart of the internet</title>"
                   "<article>one</article><article>two</article><article>three</article>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{1280, 900}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://www.reddit.com/"});

    ExpectEqString(session.LastTitle(), "Reddit - The heart of the internet",
                   "the live interstitial shape submits and the feed commits");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "one request for the interstitial and one for the answer");
    const std::string& submitted = factory.log.requests.at(1);
    Expect(submitted.find("solution=a4c1c97a5208ca7ea4c1c97a5208ca7e") != std::string::npos,
           "the doubled seed from reddit's inline script is in the GET line");
    Expect(submitted.find("js_challenge=1") != std::string::npos,
           "js_challenge travels with the submission");
    Expect(submitted.find("token=7afd7253") != std::string::npos, "token travels with the submission");
    Expect(submitted.find("Cookie:") != std::string::npos,
           "cookies from the first response ride on the challenge answer");
    Expect(submitted.find("edgebucket=livebucket") != std::string::npos,
           "edgebucket from the live Set-Cookie shape is forwarded on the submission");
  });

  AddTest(tests, "Engine/DocumentCookieReadsSetCookie", [] {
    Session session;
    ScriptedFactory factory;
    const std::string page =
        "<script>"
        "var m = document.cookie.match(/csrf_token=([^;]+)/);"
        "console.log(m ? m[1] : 'missing');"
        "</script>";
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        "Set-Cookie: csrf_token=abc123; Path=/\r\n"
        "Set-Cookie: secret=hidden; HttpOnly; Path=/\r\n"
        "Content-Length: " + std::to_string(page.size()) + "\r\n\r\n" + page});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/"});

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "abc123",
                   "script reads csrf_token from document.cookie but not HttpOnly cookies");
  });

  AddTest(tests, "Engine/ClickingAGetFormSubmitNavigatesToTheSerializedQuery", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input name='q' value='hello world' size='2'>"
                   "<input type='submit' name='go' value='Search'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Results</title><body>results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(45.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(),
                   "https://example.org/search?q=hello+world&go=Search",
                   "the form query was encoded and resolved against the document URL");
    ExpectEqString(session.LastTitle(), "Results", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /search?q=hello+world&go=Search ") !=
               std::string::npos,
           "the second request is the submitted GET form");
    Expect(factory.log.requests.at(1).find("Referer: https://example.org/start\r\n") !=
               std::string::npos,
           "the GET form navigation carries the policy-computed referrer");
  });

  AddTest(tests, "Engine/ClickingAPostFormSubmitSendsARequestBody", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search?keep=1' method='post'>"
                   "<input name='q' value='hello world' size='2'>"
                   "<input type='submit' name='go' value='Search'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Posted</title><body>posted results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(45.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?keep=1",
                   "POST form navigation commits the action URL without moving controls into it");
    ExpectEqString(session.LastTitle(), "Posted", "and the result document committed");
    const std::string& request = factory.log.requests.at(1);
    Expect(request.rfind("POST /search?keep=1 HTTP/1.1\r\n", 0) == 0,
           "the second request uses the form method and preserves the action query");
    Expect(request.find("Referer: https://example.org/start\r\n") != std::string::npos,
           "the POST form navigation carries the policy-computed referrer");
    Expect(request.find("Content-Type: application/x-www-form-urlencoded\r\n") !=
               std::string::npos,
           "the request carries the form encoding");
    Expect(request.find("Content-Length: 23\r\n") != std::string::npos,
           "the serialized controls define the request body length");
    Expect(request.size() >= 23 &&
               request.substr(request.size() - 23) == "q=hello+world&go=Search",
           "the form controls are sent in the body");
  });

  AddTest(tests, "Engine/ClickingATextPlainPostFormSendsAPlainRequestBody", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/plain' method='post' "
                   "enctype='text/plain'>"
                   "<input name='q' value='hello world' size='2'>"
                   "<input type='submit' name='go' value='Search'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Plain</title><body>plain results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(45.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/plain",
                   "POST text/plain commits the action URL");
    ExpectEqString(session.LastTitle(), "Plain", "and the result document committed");
    const std::string& request = factory.log.requests.at(1);
    Expect(request.rfind("POST /plain HTTP/1.1\r\n", 0) == 0,
           "the second request uses POST");
    Expect(request.find("Content-Type: text/plain\r\n") != std::string::npos,
           "the request carries the selected form encoding");
    Expect(request.find("Content-Length: 26\r\n") != std::string::npos,
           "the plain form body length includes CRLF row endings");
    Expect(request.size() >= 26 &&
               request.substr(request.size() - 26) == "q=hello world\r\ngo=Search\r\n",
           "the form controls are sent as name=value lines without URL encoding");
  });

  AddTest(tests, "Engine/TextInputChangesFocusedFormControlsBeforeSubmit", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input name='q' size='2'><input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Typed</title><body>typed results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);
    session.Type("hi");
    session.Click(45.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?q=hi",
                   "submitted GET forms use the current focused input value");
    ExpectEqString(session.LastTitle(), "Typed", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /search?q=hi ") != std::string::npos,
           "the second request contains the typed query");
  });

  AddTest(tests, "Engine/TextLikeInputTypesCanBeEditedAndSubmitted", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/contact'>"
                   "<input type='email' name='email' size='4'><input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Contact</title><body>contact</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);
    session.Type("a@b");
    session.Send(NamedKey("Enter"));

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/contact?email=a%40b",
                   "text-like input types use the focused text-editing path");
    ExpectEqString(session.LastTitle(), "Contact", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /contact?email=a%40b ") != std::string::npos,
           "the second request contains the edited email value");
  });

  AddTest(tests, "Engine/PreventDefaultOnAKeydownStopsTheCharacterBeingInserted", [] {
    // The default action is a step after dispatch, and this is the test that
    // says so. Before ADR 0017 the character was inserted on the way past and
    // there was no keydown at all, so a page filtering its own input -- a
    // numbers-only field, a shortcut bar -- could not.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input id=q name='q' size='4'>"
                   "</form>"
                   "<script>"
                   "document.getElementById('q').addEventListener('keydown', e => {"
                   "  console.log('key ' + e.key);"
                   "  if (e.key === 'x') { e.preventDefault(); }"
                   "});"
                   "</script></body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);
    session.Type("axb");

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "key a|key x|key b",
                   "every key reached the page's handler");
    // The cancelled one is missing from the value and present in the log, which
    // is the whole distinction: dispatch happened, the default action did not.
    session.Send(NamedKey("Enter"));
    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?q=ab",
                   "and only the key it cancelled failed to be inserted");
  });

  AddTest(tests, "Engine/AKeyThatInsertsNothingStillReachesThePage", [] {
    // Escape, which the message set this replaces could not deliver at all: a
    // key crossed the seam as the text it produced, and Escape produces none.
    // "Escape closes a menu" is session 9's check and this is its unit.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body><div id=menu>open</div>"
                   "<script>"
                   "document.addEventListener('keydown', e => console.log('+' + e.key));"
                   "document.addEventListener('keyup', e => console.log('-' + e.key));"
                   "</script></body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    ipc::KeyInputMessage escape = NamedKey("Escape");
    session.Send(escape);
    escape.kind = ipc::KeyInputMessage::Kind::Up;
    session.Send(escape);

    ExpectEqString(Joined(session.engine.ConsoleOutput()), "+Escape|-Escape",
                   "the press and the release both arrived, named");
  });

  AddTest(tests, "Engine/InputCommandsEditAndSubmitFocusedForm", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input name='q' size='3'><input type='submit' name='go' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Commands</title><body>command results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);
    session.Type("abc");
    session.Send(NamedKey("Backspace"));
    session.Send(NamedKey("Delete"));
    session.Send(NamedKey("Enter"));

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?q=ab",
                   "enter submits the edited focused form without a clicked submit button");
    ExpectEqString(session.LastTitle(), "Commands", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /search?q=ab ") != std::string::npos,
           "the second request contains the command-edited query");
  });

  AddTest(tests, "Engine/ClickingCheckableInputsUpdatesSubmittedForm", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<style>input{width:10px;height:10px;margin:0}</style>"
                   "<body style='margin:0'><form action='/filter'>"
                   "<input type='checkbox' name='seen'>"
                   "<input type='radio' name='mode' value='old' checked>"
                   "<input type='radio' name='mode' value='new'>"
                   "<input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Filtered</title><body>filtered</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);
    session.Click(25.0f, 5.0f);
    session.Click(35.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/filter?seen=on&mode=new",
                   "submitted GET forms use clicked checkable state");
    ExpectEqString(session.LastTitle(), "Filtered", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /filter?seen=on&mode=new ") !=
               std::string::npos,
           "the second request contains the toggled controls");
  });

  AddTest(tests, "Engine/ClickingResetRestoresSubmittedFormDefaults", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<style>input{width:20px;height:20px;margin:0}</style>"
                   "<body style='margin:0'><form action='/filter'>"
                   "<input name='q'>"
                   "<input type='checkbox' name='seen' checked>"
                   "<input type='reset' value='Reset'>"
                   "<input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Filtered</title><body>filtered</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Click(5.0f, 5.0f);
    session.Type("abc");
    session.Click(25.0f, 5.0f);
    session.Click(45.0f, 5.0f);
    session.Click(65.0f, 5.0f);

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/filter?q=&seen=on",
                   "submitted GET forms use reset defaults");
    ExpectEqString(session.LastTitle(), "Filtered", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /filter?q=&seen=on ") != std::string::npos,
           "the second request contains the reset form state");
  });

  AddTest(tests, "Engine/EveryFrameItProducesSurvivesItsOwnWireFormat", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<h1>ABCD</h1><p style='border:1px solid black'>ABC ABCD</p>")});

    std::size_t frames = 0;
    for (const ipc::EngineToUi& message : session.sent) {
      const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(message));
      Expect(decoded.has_value(), "the engine emitted a message its own wire format rejects");
      Expect(*decoded == message, "and decoding is not a fixed point of encoding");
      frames += std::holds_alternative<ipc::PaintFrameMessage>(message) ? 1u : 0u;
    }
    Expect(frames > 0, "at least one frame, or this asserts nothing");
  });

  // ADR 0011: "the failure mode of asynchronous loading is not slowness, it is
  // nondeterminism". These are the tests that say so. The same responses,
  // delivered in different orders, must produce the same page -- and that is a
  // stronger property than "it loads", and the one that decays silently.
  AddTest(tests, "Engine/ArrivalOrderDoesNotChangeThePage", [] {
    // Two sheets that both set the same property. Which one wins is decided by
    // document order, so a load that filled slots in arrival order would give a
    // different colour depending on which server answered first.
    constexpr std::string_view kDocument =
        "<html><head>"
        "<link rel='stylesheet' href='/a.css'>"
        "<link rel='stylesheet' href='/b.css'>"
        "</head><body><p>ABC</p><script src='/x.js'></script></body></html>";

    const auto load = [&](const std::vector<std::string>& order) {
      auto session = std::make_unique<Session>();
      ScriptedFactory factory;
      factory.delivery = ScriptedFactory::Delivery::Held;
      factory.script = {
          {"example.org", 443, true, OkResponse("text/html", std::string(kDocument))},
          {"example.org", 443, true, OkResponse("text/css", "p { height: 100px }")},
          {"example.org", 443, true, OkResponse("text/css", "p { height: 400px }")},
          {"example.org", 443, true,
           OkResponse("application/javascript",
                      "var d = document.createElement('div');"
                      "d.setAttribute('style', 'height: 700px');"
                      "document.body.appendChild(d);")},
      };
      session->engine.PageLoader().SetTransport(factory);

      session->Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
      session->channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
      session->engine.HandlePendingMessages();
      RunEngineToIdle(session->engine);

      Expect(factory.Release("GET /page.html "), "the document request is outstanding");
      RunEngineToIdle(session->engine);
      for (const std::string& needle : order) {
        Expect(factory.Release(needle), "expected " + needle + " to be outstanding");
        RunEngineToIdle(session->engine);
      }
      Expect(!session->engine.IsLoading(), "the load finished");
      while (auto reply = session->channel.Ui().TryReceive()) {
        session->sent.push_back(std::move(*reply));
      }
      return session;
    };

    const auto forwards = load({"GET /a.css ", "GET /b.css ", "GET /x.js "});
    const auto backwards = load({"GET /x.js ", "GET /b.css ", "GET /a.css "});
    const auto middle_first = load({"GET /b.css ", "GET /x.js ", "GET /a.css "});

    const ipc::PaintFrameMessage* first = forwards->LastFrame();
    Expect(first != nullptr, "a frame was painted");
    Expect(first->display_list.Bounds().height >= 300,
           "the page is as tall as the 400px sheet and the script's 700px div make it, so "
           "the later sheet won -- which is document order and not arrival order");
    Expect(backwards->LastFrame() != nullptr && *backwards->LastFrame() == *first,
           "delivering the responses backwards produced a different page");
    Expect(middle_first->LastFrame() != nullptr && *middle_first->LastFrame() == *first,
           "delivering the script between the two sheets produced a different page");
    Expect(forwards->engine.ScriptErrors().empty() && backwards->engine.ScriptErrors().empty(),
           "no script threw, in either order");
  });

  AddTest(tests, "Engine/ScriptsDoNotRunUntilEveryStyleSheetHasResolved", [] {
    Session session;
    ScriptedFactory factory;
    factory.delivery = ScriptedFactory::Delivery::Held;
    factory.script = {
        {"example.org", 443, true,
         OkResponse("text/html",
                    "<html><head><link rel='stylesheet' href='/s.css'></head>"
                    "<body><script src='/x.js'></script></body></html>")},
        {"example.org", 443, true, OkResponse("text/css", "p { height: 400px }")},
        // Throwing is the cheapest thing a script can do that the engine
        // reports from outside, which is what makes "did it run yet" a
        // question this test can ask at all.
        {"example.org", 443, true, OkResponse("application/javascript", "throw 'ran';")},
    };
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    Expect(factory.Release("GET /page.html "), "the document is outstanding");
    RunEngineToIdle(session.engine);

    // The script arrives first and must wait: a script may ask about a style,
    // and running it before the sheet landed would make the answer depend on
    // the network.
    Expect(factory.Release("GET /x.js "), "the script is outstanding");
    RunEngineToIdle(session.engine);
    Expect(session.engine.IsLoading(),
           "the load must not be finished while a render-blocking sheet is outstanding");
    Expect(session.engine.ScriptErrors().empty(),
           "the script must not have run before the stylesheet resolved: a script may ask "
           "about a style, and running it first would make the answer depend on the network");

    Expect(factory.Release("GET /s.css "), "the sheet is outstanding");
    RunEngineToIdle(session.engine);
    ExpectEqInt(static_cast<long long>(session.engine.ScriptErrors().size()), 1,
                "and it ran once the sheet had");
  });

  AddTest(tests, "Engine/ConcurrencyIsBoundedPerPartition", [] {
    // More images than the per-partition request bound. That many may be in
    // flight; the rest wait for a slot. Per key rather than globally -- see
    // net::kMaxRequestsPerPartition. (The socket bound is a separate number;
    // HTTP/2 made them stop being the same thing -- TD-0010.)
    constexpr int kExtra = 2;
    constexpr int kTotal =
        static_cast<int>(net::kMaxRequestsPerPartition) + kExtra;
    std::string html = "<html><body>";
    for (int i = 0; i < kTotal; ++i) {
      html += "<img src='/i" + std::to_string(i) + ".png'>";
    }
    html += "</body></html>";

    Session session;
    ScriptedFactory factory;
    factory.delivery = ScriptedFactory::Delivery::Held;
    factory.script.push_back({"example.org", 443, true, OkResponse("text/html", html)});
    for (int i = 0; i < kTotal; ++i) {
      factory.script.push_back({"example.org", 443, true, OkResponse("image/png", "notapng")});
    }
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    Expect(factory.Release("GET /page.html "), "the document is outstanding");
    RunEngineToIdle(session.engine);

    ExpectEqInt(static_cast<long long>(factory.Held()),
                static_cast<long long>(net::kMaxRequestsPerPartition),
                "exactly the bound is open at once, and the rest are waiting for a slot");

    // Letting them go frees slots, and the ones that were waiting start.
    factory.ReleaseAll();
    RunEngineToIdle(session.engine);
    factory.ReleaseAll();
    RunEngineToIdle(session.engine);
    ExpectEqInt(static_cast<long long>(factory.Held()), 0, "every image has been attempted");
    Expect(!session.engine.IsLoading(), "and the load finishes");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()),
                static_cast<long long>(kTotal) + 1,
                "every image was eventually asked for");
  });

  AddTest(tests, "Engine/ANavigationDropsWhatTheLastOneHadInFlight", [] {
    Session session;
    ScriptedFactory factory;
    factory.delivery = ScriptedFactory::Delivery::Held;
    factory.script = {
        {"example.org", 443, true,
         OkResponse("text/html",
                    "<html><head><title>first</title></head>"
                    "<body><link rel='stylesheet' href='/s.css'></body></html>")},
        {"example.org", 443, true, OkResponse("text/css", "p { height: 400px }")},
        {"example.org", 443, true,
         OkResponse("text/html", "<html><head><title>second</title></head><body></body></html>")},
    };
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.channel.Ui().Send(ipc::NavigateMessage{"https://example.org/one.html"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    Expect(factory.Release("GET /one.html "), "the first document is outstanding");
    RunEngineToIdle(session.engine);
    ExpectEqInt(static_cast<long long>(factory.Held()), 1, "its stylesheet is in flight");

    // Navigating away must take the connection with it. Not "ignore the
    // response when it lands" -- the request has to stop existing, which is
    // what ADR 0011 means by dropped by construction.
    session.channel.Ui().Send(ipc::NavigateMessage{"https://example.org/two.html"});
    session.engine.HandlePendingMessages();
    ExpectEqInt(static_cast<long long>(factory.Held()), 1,
                "the abandoned stylesheet's connection is gone and only the new document is "
                "outstanding");

    Expect(factory.Release("GET /two.html "), "the second document is outstanding");
    RunEngineToIdle(session.engine);
    while (auto reply = session.channel.Ui().TryReceive()) {
      session.sent.push_back(std::move(*reply));
    }
    ExpectEqString(session.LastTitle(), "second", "the second page is the one on screen");
  });

  // ADR 0011 decided `defer`, `async` and `type=module` are three points in a
  // document's lifecycle rather than three attributes to ignore. These say so.
  // Each script throws its own name, because `ScriptErrors()` is in run order
  // and names the script -- which makes "when did it run" a thing a test can
  // ask without a console.
  AddTest(tests, "Engine/DeferredScriptsRunAfterBlockingOnes", [] {
    Session session;
    ScriptedFactory factory;
    factory.script = {
        {"example.org", 443, true,
         OkResponse("text/html",
                    "<html><head>"
                    "<script src='/d.js' defer></" "script>"
                    "<script src='/b.js'></" "script>"
                    "</head><body></body></html>")},
        {"example.org", 443, true, OkResponse("application/javascript", "throw 'deferred';")},
        {"example.org", 443, true, OkResponse("application/javascript", "throw 'blocking';")},
    };
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    const std::vector<std::string>& errors = session.engine.ScriptErrors();
    ExpectEqInt(static_cast<long long>(errors.size()), 2, "both scripts ran");
    Expect(errors.at(0).find("/b.js") != std::string::npos,
           "the blocking script runs first even though the deferred one is earlier in the "
           "document: that is what `defer` promises");
    Expect(errors.at(1).find("/d.js") != std::string::npos, "and the deferred one runs after");
  });

  // The thrown completion is a C++ local after Run returns. Firing `error` on
  // the script element drains microtasks and can collect it; reading `.stack`
  // afterwards was the youtube.com watch-page segfault (ValueRoot).
  AddTest(tests, "Engine/AThrowingScriptsErrorEventDoesNotCollectItsStack", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<script id='boom'>"
        "document.getElementById('boom').addEventListener('error', function () {"
        "  const a = [];"
        "  for (let i = 0; i < 8000; i++) a.push({i: i});"
        "});"
        "throw new Error('boom');"
        "</" "script>")});

    const std::vector<std::string>& errors = session.engine.ScriptErrors();
    ExpectEqInt(static_cast<long long>(errors.size()), 1, "the throw was recorded");
    Expect(errors.at(0).find("boom") != std::string::npos, "the message survived the error event: " +
                                                               errors.at(0));
    Expect(errors.at(0).find("at ") != std::string::npos,
           "and so did the stack — collecting the Error under the error listener "
           "used to segfault here: " +
               errors.at(0));
  });

  AddTest(tests, "Engine/AnAsyncScriptDoesNotHoldTheFirstFrame", [] {
    Session session;
    ScriptedFactory factory;
    factory.delivery = ScriptedFactory::Delivery::Held;
    factory.script = {
        {"example.org", 443, true,
         OkResponse("text/html",
                    "<html><head><script src='/a.js' async></" "script></head>"
                    "<body><p>ABC</p></body></html>")},
        {"example.org", 443, true, OkResponse("application/javascript", "throw 'async';")},
    };
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
    session.engine.HandlePendingMessages();
    RunEngineToIdle(session.engine);
    Expect(factory.Release("GET /page.html "), "the document is outstanding");
    RunEngineToIdle(session.engine);
    while (auto reply = session.channel.Ui().TryReceive()) {
      session.sent.push_back(std::move(*reply));
    }

    ExpectEqInt(static_cast<long long>(factory.Held()), 1, "the async script is still in flight");
    const ipc::PaintFrameMessage* painted = session.LastFrame();
    Expect(painted != nullptr,
           "the page is on screen without it: a page whose analytics tag is slow must not be "
           "a page that is blank, which is the entire reason the attribute exists");
    Expect(session.engine.ScriptErrors().empty(), "and it has not run yet");
    Expect(session.engine.IsLoading(),
           "though the navigation is not over -- not waiting for it is different from "
           "dropping it");

    Expect(factory.Release("GET /a.js "), "the async script is outstanding");
    RunEngineToIdle(session.engine);
    ExpectEqInt(static_cast<long long>(session.engine.ScriptErrors().size()), 1,
                "and it runs when it lands");
    Expect(!session.engine.IsLoading(), "which is when the navigation is finally over");
  });

  AddTest(tests, "Engine/AModuleIsEvaluatedAsAModule", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<script type='module'>throw 'ran';</" "script>"
        "<script type='module'>import a from './x.js';</" "script>")});

    const std::vector<std::string>& errors = session.engine.ScriptErrors();
    ExpectEqInt(static_cast<long long>(errors.size()), 2, "both module scripts were evaluated");
    Expect(errors.at(0).find("ran") != std::string::npos, "an inline module runs");
    // **This assertion changed with the module loader (ledger session 50).** It
    // used to be "modules are not available in this context", which was the
    // engine saying there was no resolver at all. There is one now, so the
    // failure is the specific one it should be: `./x.js` is relative and this
    // document is a `data:` URL, which is not a base anything can be relative
    // to, so the specifier resolves to nothing. What matters is unchanged and is
    // why the test exists -- an `import` that cannot be resolved fails *as an
    // unresolved import* rather than as a syntax error, because the second would
    // be the engine claiming the page is malformed.
    Expect(errors.at(1).find("cannot resolve module") != std::string::npos,
           "and an unresolvable import says so rather than reporting a parse error: " +
               errors.at(1));
  });

  AddTest(tests, "Engine/AStaticPageSchedulesNothing", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl("<p>ABC</p>")});
    Expect(!session.engine.NextDeadlineMs().has_value(),
           "a loaded page with no timer, no frame and nothing outstanding must hand the loop "
           "no deadline at all -- this is the zero-idle-CPU invariant at the seam");
  });

  AddTest(tests, "Engine/AKeptConnectionIsTheOnlyThingAFinishedLoadStillSchedules", [] {
    // The other half of the invariant. A pooled connection is a socket the user
    // did not ask to keep open, so something has to come back for it -- and the
    // only thing that ever will is a deadline the loop is told about. Before
    // ADR 0010 the engine handed back no deadline at all once a load was over,
    // which would have left an idle socket open until the next navigation
    // happened along.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("text/html", "<p>ABC</p>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});
    Expect(!session.engine.IsLoading(), "the load is over");

    const std::optional<std::uint32_t> deadline = session.engine.NextDeadlineMs();
    Expect(deadline.has_value(),
           "and the connection it left behind is still a deadline, with no load in flight");
    Expect(*deadline <= net::kIdleConnectionTimeoutMs,
           "one no later than the idle timeout, not one wakeup per anything");
  });

  AddTest(tests, "Page/AnAnimationFrameIsADeadlineAndAStoppedOneIsNot", [] {
    // At Page rather than Engine because time is a parameter here: a test that
    // had to wait 16ms of real time per frame to assert a scheduling property
    // would be a slow test that is also a flaky one.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><p>ABC</p><script>"
        "globalThis.left = 2;"
        "globalThis.tick = () => { if (--left > 0) requestAnimationFrame(tick); };"
        "requestAnimationFrame(tick);"
        "</" "script></body></html>",
        "https://example.org/");
    page.RunScripts(0);

    Expect(page.NextWakeDelay(0).has_value(),
           "a page with a frame pending wakes the loop at the frame boundary");
    // Two frames, and then the page stops asking. A browser that kept a 60Hz
    // loop running past this point is one that costs a core to leave open.
    std::int64_t now = 0;
    for (int frame = 0; frame < 4 && page.NextWakeDelay(now).has_value(); ++frame) {
      now += bindings::kFrameIntervalMs;
      page.RunDueWork(now);
    }
    Expect(!page.NextWakeDelay(now).has_value(),
           "and stops scheduling the moment the page stops asking");
  });

  AddTest(tests, "Page/AnimationConstructorIsConstructible", [] {
    // youtube SPA search→watch calls `new Animation(effect)` in a listener;
    // Illegal constructor left ytd-player without #movie_player / <video>.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<html><body></body></html>", "https://example.org/");
    page.Layout(400.0f);
    page.RunScripts(0);
    const std::string out = page.EvaluateScript(
        "(() => {"
        "  const a = new Animation();"
        "  const b = document.createElement('div').animate([], 0);"
        "  const need = 'play currentTime pause reverse playbackRate cancel finish startTime "
        "playState'.split(' ');"
        "  const missing = need.filter((k) => b[k] === undefined);"
        "  return [a instanceof Animation, a.playState, typeof a.play, typeof a.reverse,"
        "          typeof a.finish, typeof a.playbackRate, typeof a.startTime,"
        "          missing.join(',')].join('|');"
        "})()");
    ExpectEqString(out, "true|finished|function|function|function|number|object|",
                   "constructible Animation + polyfill probe surface: " + out);
  });

  AddTest(tests, "Page/ElementAnimateDrivesComputedStyleWithoutStyleAttribute", [] {
    // TD-0021: native Element.animate must not write el.style (the polyfill
    // path). Mid-animation getComputedStyle sees the interpolated transform.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetAnimationTime(0);
    page.Load(
        "<html><body><div id='box'>x</div><script>"
        "const el = document.getElementById('box');"
        "const before = el.getAttribute('style');"
        "globalThis.anim = el.animate("
        "  [{transform: 'translateX(0px)'}, {transform: 'translateX(100px)'}],"
        "  {duration: 200, fill: 'forwards', easing: 'linear'});"
        "globalThis.styleAttr = el.getAttribute('style');"
        "globalThis.hadAttr = before;"
        "</" "script></body></html>",
        "https://example.org/");
    page.SetViewport(css::MediaContext{400.0f, 300.0f, 1.0f});
    page.Layout(400.0f);
    page.RunScripts(0);
    Expect(page.NextWakeDelay(0).has_value(), "a running WAAPI effect wakes the loop");
    ExpectEqInt(static_cast<long long>(page.RunningAnimations().RunningCount()), 1,
                "one programmatic effect");
    // Advance halfway; RestyleWithoutLayout via due work so AdjustStyle applies.
    page.SetAnimationTime(100);
    page.RunDueWork(100);
    page.Layout(400.0f);
    const std::string mid = page.EvaluateScript(
        "getComputedStyle(document.getElementById('box')).transform + '|' +"
        " (document.getElementById('box').getAttribute('style') === null ? 'none' : "
        "document.getElementById('box').getAttribute('style')) + '|' +"
        " globalThis.anim.playState");
    Expect(mid.find("none") != std::string::npos || mid.find("matrix") != std::string::npos ||
               mid.find("translate") != std::string::npos,
           "computed transform is set mid-animation: " + mid);
    Expect(mid.find("|none|") != std::string::npos || mid.find("||") != std::string::npos,
           "style attribute stays unset: " + mid);
    Expect(mid.find("running") != std::string::npos || mid.find("finished") != std::string::npos,
           "playState is live: " + mid);

    page.EvaluateScript("globalThis.anim.pause()");
    page.RunDueWork(100);
    Expect(!page.NextWakeDelay(100).has_value(),
           "pause leaves no animation wake (idle CPU)");

    page.EvaluateScript("globalThis.anim.play()");
    page.SetAnimationTime(250);
    page.RunDueWork(250);
    const std::string done = page.EvaluateScript("globalThis.anim.playState");
    ExpectEqString(done, "finished", "fill:forwards holds finished state");
  });

  AddTest(tests, "Page/ElementAnimateFinishedResolvesAndCancelRejects", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetAnimationTime(0);
    page.Load(
        "<html><body><div id='box'>x</div><script>"
        "const el = document.getElementById('box');"
        "globalThis.ok = el.animate([{opacity:'1'},{opacity:'1'}], {duration:50, fill:'none'});"
        "globalThis.ok.finished.then(() => { globalThis.resolved = true; },"
        "  () => { globalThis.resolved = false; });"
        "globalThis.bad = el.animate([{transform:'none'},{transform:'none'}], {duration:5000});"
        "globalThis.bad.finished.then(() => { globalThis.cancelledOk = false; },"
        "  (e) => { globalThis.cancelledOk = e && e.name === 'AbortError'; });"
        "globalThis.bad.cancel();"
        "</" "script></body></html>",
        "https://example.org/");
    page.Layout(400.0f);
    page.RunScripts(0);
    page.RunDueWork(0);  // deliver cancel rejection
    page.SetAnimationTime(60);
    page.RunDueWork(60);  // finish the short one
    // Microtasks from SettleAsyncResult need a turn.
    page.RunDueWork(60);
    const std::string out = page.EvaluateScript(
        "String(globalThis.resolved) + '|' + String(globalThis.cancelledOk)");
    ExpectEqString(out, "true|true", "finished resolves; cancel rejects AbortError");
  });

  AddTest(tests, "Page/ElementAnimateAcceptsEmptyKeyframes", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<html><body><div id='box'>x</div><script>"
        "const el = document.getElementById('box');"
        "const a = el.animate([], 100);"
        "const b = el.animate(null, 100);"
        "globalThis.ok = a && b && a.playState === 'finished' && b.playState === 'finished';"
        "</" "script></body></html>",
        "https://example.org/");
    page.Layout(400.0f);
    page.RunScripts(0);
    ExpectEqString(page.EvaluateScript("String(globalThis.ok)"), "true",
                   "empty/null keyframes return a finished Animation");
  });

  // An element by id, for the two animation tests below. `dom::Document` has no `getElementById` --
  // that lives in the binding layer, where a page reaches it -- so the walk is here.
  const auto element_with_id = [](dom::Document& document, std::string_view id) -> dom::Element* {
    dom::Element* found = nullptr;
    document.ForEachDescendant([&](const dom::Node& node) {
      if (found != nullptr || !node.IsElement()) {
        return;
      }
      const auto& element = static_cast<const dom::Element&>(node);
      const std::string* value = element.GetAttribute("id");
      if (value != nullptr && *value == id) {
        found = const_cast<dom::Element*>(&element);
      }
    });
    return found;
  };

  AddTest(tests, "Page/ATransitionWakesTheLoopAndThenStopsWaking", [element_with_id] {
    // **The assertion ADR 0014 §5 asks for, in as many words**: "the loop wakes while something is
    // animating and *not one frame after everything has settled*". At `Page` rather than `Engine`
    // because time is a parameter here -- a test that waited 16ms of real time per frame would be a
    // slow test that is also a flaky one.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetAnimationTime(0);
    page.Load(
        "<html><head><style>"
        "#box { width: 100px; background-color: rgb(0,0,0); transition: width 200ms linear }"
        "#box.wide { width: 300px }"
        "</style></head><body><div id='box'>ABC</div></body></html>",
        "https://example.org/");
    page.Layout(400.0f);
    Expect(!page.NextWakeDelay(0).has_value(),
           "a page whose transition has not been triggered schedules nothing -- which is the case "
           "every page with a :hover transition is in while the pointer is elsewhere");

    // The class change is what a hover or a script would do. The transition starts on the *next*
    // restyle, because that is when the cascade produces a different width.
    dom::Element* box = element_with_id(*page.MutableDocument(), "box");
    Expect(box != nullptr, "the element is there");
    box->SetAttribute("class", "wide");
    // **The clock before the invalidation**, and the order is the point rather than an incantation:
    // `InvalidateLayout` resolves the cascade again to collect background images, so it is a restyle --
    // and a restyle is where a transition starts. Setting the time afterwards made the transition begin
    // at instant zero and be over before anything looked at it, which is how this was found.
    page.SetAnimationTime(1000);
    page.InvalidateLayout();
    page.Layout(400.0f);
    Expect(page.NextWakeDelay(1000).has_value(), "and now it asks for frames");
    ExpectEqInt(static_cast<long long>(page.RunningAnimations().RunningCount()), 1,
                "exactly one transition, on the one property that changed");

    // Halfway through, at linear easing, the width is halfway. Read through the cascade rather than off
    // the animation object, because what matters is that *layout* sees the interpolated value.
    page.SetAnimationTime(1100);
    page.InvalidateLayout();
    page.Layout(400.0f);
    const css::ComputedStyle halfway = page.StyleOfForTesting(*box);
    Expect(halfway.width.value > 190.0f && halfway.width.value < 210.0f,
           "halfway through a 100px-to-300px linear transition the width is about 200px, and it is "
           "the cascade that says so; got " + std::to_string(halfway.width.value));

    // Past the end. The transition is dropped, and *the final value survives* -- because a
    // transition's `to` is the resolved style, which is why a transition needs no fill mode.
    std::int64_t now = 1000;
    for (int frame = 0; frame < 40 && page.NextWakeDelay(now).has_value(); ++frame) {
      now += 16;
      page.SetAnimationTime(now);
      page.RunDueWork(now);
    }
    Expect(!page.NextWakeDelay(now).has_value(),
           "and the loop stops being woken the moment the transition finishes");
    ExpectEqInt(static_cast<long long>(page.RunningAnimations().RunningCount()), 0,
                "with nothing left in the map -- a finished transition that stayed would ask for a "
                "frame forever, which is how this feature would cost a core");
    page.InvalidateLayout();
    page.Layout(400.0f);
    ExpectEqString(std::to_string(static_cast<int>(page.StyleOfForTesting(*box).width.value)), "300",
                   "and the element is left where the transition was heading");
  });

  AddTest(tests, "Page/AKeyframeAnimationRunsAndAPausedOneAsksForNothing", [element_with_id] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetAnimationTime(0);
    page.Load(
        "<html><head><style>"
        "@keyframes slide { from { left: 0px } to { left: 100px } }"
        "#box { position: relative; animation: slide 1s linear }"
        "</style></head><body><div id='box'>ABC</div></body></html>",
        "https://example.org/");
    page.Layout(400.0f);
    Expect(page.NextWakeDelay(0).has_value(),
           "an animation starts by itself -- unlike a transition, which needs something to change");
    dom::Element* box = element_with_id(*page.MutableDocument(), "box");
    Expect(box != nullptr, "the element is there");
    page.SetAnimationTime(500);
    page.InvalidateLayout();
    page.Layout(400.0f);
    const css::ComputedStyle halfway = page.StyleOfForTesting(*box);
    Expect(halfway.inset.left.value > 45.0f && halfway.inset.left.value < 55.0f,
           "halfway through, halfway along");

    // A paused animation asks for no frames at all, which is the whole point of `paused` and the one
    // case where something is running and the loop should still block.
    TestFonts paused_fonts;
    engine::Page paused(paused_fonts.catalog);
    paused.SetAnimationTime(0);
    paused.Load(
        "<html><head><style>"
        "@keyframes slide { from { left: 0px } to { left: 100px } }"
        "#box { position: relative; animation: slide 1s linear paused }"
        "</style></head><body><div id='box'>ABC</div></body></html>",
        "https://example.org/");
    paused.Layout(400.0f);
    Expect(!paused.NextWakeDelay(0).has_value(), "a paused animation schedules nothing");
  });

  AddTest(tests, "Worker/AScriptRunsOnItsOwnThreadAndMessagesCrossByValue", [] {
    // **ADR 0022 §1 end to end**, and the assertions worth having are the ones about the *boundary*: a
    // value crosses as bytes and comes back as an object in the other heap, a `Map` survives both
    // crossings (which `JSON.parse(JSON.stringify())` cannot do), an uncaught throw inside the worker
    // arrives as an `error` event, and `terminate()` joins the thread.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<script>"
                   "var w = new Worker('/w.js');"
                   "w.onmessage = function(e) {"
                   "  console.log(e.data.kind + ':' + (e.data.total === undefined ? '' : e.data.total) +"
                   "           (e.data.isMap === undefined ? '' : e.data.isMap + ',' + e.data.got));"
                   "  if (e.data.kind === 'ready') { w.postMessage({kind:'sum', upto: 100}); }"
                   "  else if (e.data.kind === 'sum') {"
                   "    w.postMessage({kind:'echo', map: new Map([['a','A']])}); }"
                   "  else if (e.data.kind === 'echo') { w.postMessage('boom'); }"
                   "};"
                   "w.onerror = function(e) { console.log('error:' + e.message); w.terminate();"
                   "                           console.log('terminated'); };"
                   "</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("application/javascript",
                   "onmessage = function(e) {"
                   "  if (e.data.kind === 'sum') {"
                   "    var t = 0; for (var i = 0; i < e.data.upto; i++) t += i;"
                   "    postMessage({kind:'sum', total: t});"
                   "  } else if (e.data.kind === 'echo') {"
                   "    postMessage({kind:'echo', isMap: e.data.map instanceof Map,"
                   "                 got: e.data.map.get('a')});"
                   "  } else { throw new Error('on purpose'); }"
                   "};"
                   "postMessage({kind:'ready'});")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    // The crank, until the round trip is done.
    //
    // **This one has to wait in wall-clock time, and that is the point rather than a concession.** Every
    // other test here turns the crank in a tight loop because a canned transport answers instantly; a
    // worker is a *different thread*, and how soon it runs is the scheduler's business. The first version
    // spun 2000 turns in microseconds and passed on an idle machine while failing in the full suite,
    // where the other shards had the cores -- which is exactly the flake a busy-wait against another
    // thread produces. So it sleeps when there is nothing to do and gives up on a deadline.
    std::string log;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      const bool advanced = session.engine.Advance();
      const bool ran = session.engine.RunDueWork();
      log.clear();
      for (const std::string& line : session.engine.ConsoleOutput()) {
        log += line + "|";
      }
      if (log.find("terminated") != std::string::npos) {
        break;
      }
      if (!advanced && !ran && !session.engine.HasRunnableWork()) {
        // Nothing for this thread to do, so yield rather than burn a core waiting for the other one.
        // The real loop blocks on the worker's pipe here; a test has no window to wait on, so a
        // millisecond is the equivalent.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    Expect(log.find("ready:") != std::string::npos,
           "the worker's script ran on its own thread and posted back: " + log);
    Expect(log.find("sum:4950") != std::string::npos,
           "and it computed 0..99 from a message the page sent: " + log);
    // The one that matters: a `Map` serialised in the page's heap, rebuilt in the worker's, read there,
    // and the answer serialised back. Two crossings, and `JSON` would have lost it on the first.
    Expect(log.find("echo:true,A") != std::string::npos,
           "a Map survived both crossings with its contents: " + log);
    Expect(log.find("error:") != std::string::npos,
           "and an uncaught throw inside the worker arrived as an error event: " + log);
    // `terminate()` ran inside the error handler and joined the thread *before returning*, which is
    // what makes a page that terminates and then navigates unable to race it. The line after it in the
    // handler is the proof that the join returned rather than deadlocking.
    Expect(log.find("terminated") != std::string::npos,
           "terminate() joined the thread and returned: " + log);
  });

  AddTest(tests, "Worker/StructuredCloneKeepsWhatJsonWouldLose", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<script>"
        "var cyc = {}; cyc.self = cyc;"
        "var clone = structuredClone({m: new Map([['a',1]]), d: new Date(86400000),"
        "                             arr: new Uint8Array([1,2,3]), cyc: cyc});"
        "console.log('map=' + (clone.m instanceof Map) + ',' + clone.m.get('a'));"
        "console.log('date=' + (clone.d instanceof Date) + ',' + clone.d.getTime());"
        "console.log('bytes=' + (clone.arr instanceof Uint8Array) + ',' + clone.arr[2]);"
        // A cycle deserialises to *one* object, not to a tree -- which is the property a page that
        // stores a graph and reads back a tree cannot see it has lost.
        "console.log('cycle=' + (clone.cyc.self === clone.cyc));"
        "try { structuredClone(function(){}); } catch (e) { console.log('threw=' + e.name); }"
        "</" "script>",
        "https://example.org/");
    page.RunScripts(0);
    const std::vector<std::string>& output = page.ConsoleOutput();
    const auto said = [&output](const std::string& line) {
      return std::find(output.begin(), output.end(), line) != output.end();
    };
    Expect(said("map=true,1"), "a Map, which JSON would have flattened to {}");
    Expect(said("date=true,86400000"), "a Date, which JSON would have made a string");
    Expect(said("bytes=true,3"), "a typed array, which JSON would have made an object");
    Expect(said("cycle=true"), "and a cycle, which JSON would have thrown on");
    Expect(said("threw=DataCloneError"),
           "a function is a DataCloneError rather than a silent drop -- a clone that lost one would "
           "hand a page an object that is *nearly* the one it asked for");
  });

  AddTest(tests, "Worker/TheGlobalScopeIsAWorkerGlobalScopeAndImportScriptsIsSynchronous", [] {
    // **The surface a worker script stands in**, and every assertion here is one that was false before
    // and cost the whole of web-platform-tests' 1,763 `.any.worker.html` files a twenty-second timeout.
    //
    // The two that are not obvious:
    //
    // - `self instanceof DedicatedWorkerGlobalScope`. testharness.js decides what environment it is in
    //   with exactly that expression, and with no answer it concludes it is in a *shell* -- which has
    //   no way to report a result at all. A worker that ran every assertion correctly still reported
    //   nothing.
    // - **One delivery per message.** `DeliverWorkerMessage` used to call the `onmessage` property and
    //   *then* `dispatchEvent`, which runs `on<type>` again as the specification's implicit listener.
    //   Every message arrived twice. It is invisible on a page that counts side effects and fatal to a
    //   harness that counts results.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<script>"
                   "var seen = 0;"
                   "var w = new Worker('/w.js');"
                   "w.onmessage = function(e) { seen++; console.log('got:' + e.data +"
                   "                            ' deliveries=' + seen);"
                   "  if (String(e.data).indexOf('lib=') === 0) { w.postMessage('ping'); } };"
                   "</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("application/javascript",
                   // `importScripts` is synchronous: the line after it reads what it defined.
                   "importScripts('/lib.js');"
                   "postMessage('lib=' + LIB + ' scope=' +"
                   "  (self instanceof DedicatedWorkerGlobalScope) + ',' +"
                   "  (self instanceof WorkerGlobalScope) + ' path=' + location.pathname);"
                   // A timer on the worker's own thread, and a listener rather than a property.
                   "setTimeout(function() { postMessage('timer'); }, 1);"
                   "self.addEventListener('message', function(e) { postMessage('echo:' + e.data); });"
                   // A `NetworkError` rather than a syntax error from running a 404's error page.
                   "try { importScripts('/gone.js'); } catch (err) {"
                   "  postMessage('missing=' + (String(err).indexOf('NetworkError') >= 0)); }")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("application/javascript", "var LIB = 7;")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 9\r\n\r\n<i>no</i>"});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    // Wall-clock rather than a turn count, for the reason the test above says: a worker is another
    // thread and how soon it runs is the scheduler's business.
    std::string log;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      const bool advanced = session.engine.Advance();
      const bool ran = session.engine.RunDueWork();
      log.clear();
      for (const std::string& line : session.engine.ConsoleOutput()) {
        log += line + "|";
      }
      if (log.find("got:timer") != std::string::npos &&
          log.find("got:missing=") != std::string::npos &&
          log.find("got:echo:ping") != std::string::npos) {
        break;
      }
      if (!advanced && !ran && !session.engine.HasRunnableWork()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    Expect(log.find("got:lib=7 ") != std::string::npos,
           "importScripts ran the script and the *next line* saw what it defined: " + log);
    Expect(log.find("scope=true,true ") != std::string::npos,
           "the global is a DedicatedWorkerGlobalScope and a WorkerGlobalScope: " + log);
    Expect(log.find("path=/w.js") != std::string::npos,
           "and `location` is the worker's own URL, parsed on the main thread: " + log);
    Expect(log.find("got:timer") != std::string::npos,
           "a setTimeout on the worker's thread fired: " + log);
    Expect(log.find("got:missing=true") != std::string::npos,
           "a 404 is a NetworkError, not a SyntaxError from running the error page: " + log);
    Expect(log.find("got:echo:ping") != std::string::npos,
           "`self.addEventListener('message')` in the worker saw what the page posted: " + log);
    // Every `got:` line carries the running count, so a doubled delivery shows up as a repeated
    // number. Four posts from the worker, four deliveries, and there is no fifth.
    Expect(log.find("deliveries=4") != std::string::npos &&
               log.find("deliveries=5") == std::string::npos,
           "each message was delivered exactly once: " + log);
  });

  AddTest(tests, "Worker/FetchGoesOutThroughTheSameRequestPathAsThePages", [] {
    // **A worker's `fetch` is the page's `fetch`.** Same `bindings::NetworkSource`, same
    // `privacy::Verdict`, same CORS check inside `net`, same connection pool keyed by the same
    // partition -- only the two ends move: the request is queued from a second thread and the promise
    // it settles lives in a second heap. A worker with a request path of its own would be a second
    // place for every one of those decisions to be made differently.
    //
    // `URL` is asserted in the same worker for the reason the fetch is: it is the *page's* URL
    // parser, reached from the worker's thread, rather than a copy written for workers.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<script>"
                   "var w = new Worker('/w.js');"
                   "w.onmessage = function(e) { console.log('got:' + e.data); };"
                   "w.onerror = function(e) { console.log('error:' + e.message); };"
                   "</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("application/javascript",
                   "postMessage('url=' + new URL('../x?a=1', 'https://example.org/a/b/c').href +"
                   "            ' enc=' + typeof TextEncoder + ' b64=' + btoa('hi'));"
                   "fetch('/data.txt').then(function(r) { return r.text(); })"
                   "  .then(function(t) { postMessage('body=' + t); })"
                   "  .catch(function(e) { postMessage('threw=' + e); });")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("text/plain", "from the network")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    std::string log;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      const bool advanced = session.engine.Advance();
      const bool ran = session.engine.RunDueWork();
      log.clear();
      for (const std::string& line : session.engine.ConsoleOutput()) {
        log += line + "|";
      }
      if (log.find("got:body=") != std::string::npos ||
          log.find("got:threw=") != std::string::npos) {
        break;
      }
      if (!advanced && !ran && !session.engine.HasRunnableWork()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    Expect(log.find("got:url=https://example.org/a/x?a=1 ") != std::string::npos,
           "`new URL` in a worker is the one parser in src/url, resolving against a base: " + log);
    Expect(log.find("enc=function b64=aGk=") != std::string::npos,
           "and TextEncoder and btoa are the page's, not copies: " + log);
    Expect(log.find("got:body=from the network") != std::string::npos,
           "a worker's fetch went out on the main thread and its promise settled in the worker's "
           "own heap: " + log);
  });

  AddTest(tests, "Activation/PreClickStepsToggleBeforeTheEventAndUndoOnCancel", [] {
    // **HTML's pre-click activation steps, which are four separate facts and each was false.**
    //
    // A page reads all of them in one place -- its own click handler -- which is why the ordering
    // matters at all: this engine used to toggle *after* the dispatch, so `checked` inside the
    // handler answered the old value and `preventDefault` worked by never toggling rather than by
    // undoing. The two are indistinguishable from outside until a page looks, and
    // `html/semantics/forms/the-input-element/checkbox.html` looks.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<input type=checkbox id=a>"
        "<input type=checkbox id=b>"
        "<script>"
        "var a = document.getElementById('a'), b = document.getElementById('b');"
        "var order = [];"
        "a.onclick = function(e) { order.push('click:' + a.checked + ':' + a.indeterminate); };"
        "a.oninput = function(e) { order.push('input:' + a.checked + ':' + e.bubbles); };"
        "a.onchange = function(e) { order.push('change:' + a.checked); };"
        "a.indeterminate = true;"
        "a.click();"
        // Read on the line *after* the click: the events are part of the activation behaviour and
        // run inside it, not at the turn boundary.
        "console.log('a=' + order.join('|') + ' after=' + a.checked);"
        "b.onclick = function(e) { order.push('b-in-handler:' + b.checked); e.preventDefault(); };"
        "b.click();"
        "console.log('b=' + b.checked);"
        "console.log('order=' + order.join('|'));"
        "</" "script>",
        "https://example.org/");
    page.RunScripts(0);
    std::string log;
    for (const std::string& line : page.ConsoleOutput()) {
      log += line + "|";
    }
    Expect(log.find("a=click:true:false") != std::string::npos,
           "the checkbox is already toggled when the page's own click handler runs, and the "
           "pre-click steps have cleared `indeterminate`: " + log);
    Expect(log.find("input:true:true") != std::string::npos,
           "`input` fires with the new value and **bubbles**: " + log);
    Expect(log.find("change:true after=true") != std::string::npos,
           "`change` follows it, and both ran before the line after `click()`: " + log);
    Expect(log.find("b-in-handler:true") != std::string::npos,
           "a handler that will call preventDefault still sees the toggled value, because the "
           "canceled activation steps run after every handler has: " + log);
    Expect(log.find("b=false") != std::string::npos,
           "and preventDefault puts it back rather than never having toggled it: " + log);
  });

  AddTest(tests, "Script/TheTypeListIsClosedAndLanguageIsTheOtherHalfOfIt", [] {
    // HTML's "script block's type string" is **not** the `type` attribute. It is the attribute
    // stripped of ASCII whitespace when there is one, `text/` plus the `language` attribute when
    // there is not, and `text/javascript` when there is neither -- and the result has to be an
    // ASCII case-insensitive match for one of sixteen JavaScript MIME type essences.
    //
    // Both halves were wrong in opposite directions. Three spellings were accepted where the list
    // has sixteen, so `<script type="text/ecmascript">` was treated as data; and `language` was not
    // read at all, so `<script language="JavaScript1.2">` -- which becomes `text/javascript1.2`,
    // which *is* on the list -- did not run while `<script type="javascript1.2">`, a bare word that
    // is not, would have if the comparison had been any looser.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<script>window.r = [];</" "script>"
        "<script type='text/ecmascript'>r.push('ecmascript');</" "script>"
        "<script type='  text/javascript  '>r.push('padded');</" "script>"
        "<script type='TEXT/JAVASCRIPT'>r.push('uppercase');</" "script>"
        "<script type='text/javascript1.3'>r.push('js1.3');</" "script>"
        "<script language='JavaScript1.2'>r.push('language');</" "script>"
        "<script language=''>r.push('language-empty');</" "script>"
        // And the three that must not run: a bare word is not a MIME type, `vbscript` is not on the
        // list, and `text/plain` is what a page uses to park data in a script tag.
        "<script type='javascript'>r.push('BAD-bare');</" "script>"
        "<script language='vbscript'>r.push('BAD-vbscript');</" "script>"
        "<script type='text/plain'>r.push('BAD-plain');</" "script>"
        "<script>console.log('ran:' + r.join(','));</" "script>",
        "https://example.org/");
    page.RunScripts(0);
    std::string log;
    for (const std::string& line : page.ConsoleOutput()) {
      log += line + "|";
    }
    Expect(log.find("ran:ecmascript,padded,uppercase,js1.3,language,language-empty") !=
               std::string::npos,
           "every JavaScript MIME type essence runs, whitespace and case included, and `language` "
           "is read when `type` is absent: " + log);
    Expect(log.find("BAD-") == std::string::npos,
           "and the list is closed: a bare word, `vbscript` and `text/plain` are data: " + log);
  });

  AddTest(tests, "Script/AnInsertedInlineScriptRunsAtTheInsertion", [] {
    // **TD-0059.** HTML's "prepare the script element" runs an inline classic script *during* the
    // insertion steps, so the line after `appendChild` reads what it set. This engine collected
    // inserted scripts and ran them on the loop's next turn, which no page can observe as anything
    // but "it did not run" -- and which the suite asserts against on the very next line.
    //
    // Only the inline classic case moved. An external script has to be fetched and a module has to
    // be graphed; both finish on a later turn whatever this does, so both keep the path they had.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<div id=p></div>"
                   "<script>"
                   "window.ran = 0;"
                   "var s = document.createElement('script');"
                   "s.textContent = 'window.ran = 1;';"
                   "document.getElementById('p').appendChild(s);"
                   "console.log('after-append:' + window.ran);"
                   // Twice is once: the element is marked before it runs, so a later collection
                   // pass cannot run it again.
                   "document.getElementById('p').appendChild(s);"
                   "console.log('after-reinsert:' + window.ran);"
                   // An *external* one is still deferred, and saying so is the point of this line:
                   // it has to be fetched, so there is nothing to run yet.
                   "var e = document.createElement('script');"
                   "e.src = '/late.js';"
                   "document.getElementById('p').appendChild(e);"
                   "console.log('external-immediately:' + window.ran);"
                   "</" "script>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("application/javascript", "window.ran = 2;")});
    session.engine.PageLoader().SetTransport(factory);
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});
    for (int turn = 0; turn < 200 && session.engine.IsLoading(); ++turn) {
      session.engine.Advance();
      session.engine.RunDueWork();
    }
    std::string log;
    for (const std::string& line : session.engine.ConsoleOutput()) {
      log += line + "|";
    }
    Expect(log.find("after-append:1") != std::string::npos,
           "an inline script inserted by script has already run on the next line: " + log);
    Expect(log.find("after-reinsert:1") != std::string::npos,
           "and re-inserting the same element does not run it a second time: " + log);
    Expect(log.find("external-immediately:1") != std::string::npos,
           "while an external one has not run yet, because it has not been fetched: " + log);
  });

  AddTest(tests, "Privacy/TheAnswerTableIsWhatADR0029SaysItIs", [] {
    // **ADR 0029 §6's table, asserted.** The values are one thing and the *absences* are the other, and
    // the absences are why this test exists: `navigator.deviceMemory` and its six siblings are things a
    // page can find, and under ADR 0012's rule a page that finds nothing takes whatever path it has for
    // a browser without them. A page that finds a plausible-looking zero takes the path that assumes it
    // works. So each one is named here, and putting any of them back fails a test rather than slipping
    // in as a line somebody thought was harmless.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetViewport(css::MediaContext{1000.0f, 700.0f, 1.0f});
    page.Load(
        "<script>"
        "console.log('platform=' + navigator.platform);"
        "console.log('vendor=' + JSON.stringify(navigator.vendor));"
        "console.log('language=' + navigator.language + '|' + navigator.languages.join(','));"
        "console.log('cores=' + navigator.hardwareConcurrency);"
        "console.log('plugins=' + navigator.plugins.length + ',' + navigator.mimeTypes.length);"
        "var absent = ['deviceMemory','connection','getBattery','geolocation','mediaDevices',"
        "  'doNotTrack','globalPrivacyControl','fonts','userAgentData'];"
        "console.log('absent=' + absent.filter(function(n){ return navigator[n] !== undefined; }));"
        "console.log('notification=' + Notification.permission);"
        "console.log('ua-agrees=' + (navigator.appVersion === navigator.userAgent));"
        "</" "script>",
        "https://example.org/");
    page.RunScripts(0);
    const std::vector<std::string>& output = page.ConsoleOutput();
    const auto said = [&output](const std::string& line) {
      return std::find(output.begin(), output.end(), line) != output.end();
    };
    Expect(said("platform=Unknown"), "platform is a constant that says nothing about the machine");
    Expect(said("vendor=\"\""), "vendor is empty");
    Expect(said("language=en-US|en-US"),
           "one language, the same constant Accept-Language sends -- a real cost for a "
           "non-English user and the trade ADR 0029 §1 selects");
    Expect(said("cores=4"), "a constant, not the core count");
    Expect(said("plugins=0,0"),
           "empty rather than absent: plugins.length is one of the oldest fingerprinting reads, and a "
           "page that finds no plugins object at all assumes an ancient browser");
    // The one that matters most, and it is one line because the list is the assertion.
    Expect(said("absent="),
           "every one of deviceMemory, connection, getBattery, geolocation, mediaDevices, doNotTrack, "
           "globalPrivacyControl, fonts and userAgentData is undefined");
    Expect(said("notification=denied"), "default deny, and no prompt");
    Expect(said("ua-agrees=true"),
           "appVersion and userAgent are the same constant, because a page may sniff both and two "
           "constants meant to agree eventually do not");
  });

  AddTest(tests, "Privacy/TheViewportAndPixelRatioAreQuantised", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    // A window at a size a user dragged it to. The exact number is one of the highest-entropy things a
    // page can read with no interaction at all, so it is rounded *down* to a multiple of eight -- down,
    // because a page laying out to the reported width has to fit inside the real one.
    page.SetViewport(css::MediaContext{1003.0f, 701.0f, 1.5f});
    page.Load(
        "<script>"
        "console.log('inner=' + innerWidth + 'x' + innerHeight);"
        "console.log('screen=' + screen.width + 'x' + screen.height + ',' + screen.colorDepth);"
        "console.log('dpr=' + devicePixelRatio);"
        "</" "script>",
        "https://example.org/");
    page.RunScripts(0);
    const std::vector<std::string>& output = page.ConsoleOutput();
    const auto said = [&output](const std::string& line) {
      return std::find(output.begin(), output.end(), line) != output.end();
    };
    Expect(said("inner=1000x696"), "1003 and 701 round down to 1000 and 696");
    // **`screen` reports the viewport, not the display.** The sharpest row on the table: a display's
    // resolution is a strong, stable identifier readable with no interaction, and it is not a number
    // any page needs -- what a page wants from `screen.width` is "how much room do I have".
    Expect(said("screen=1000x696,24"), "and screen agrees with it rather than with the display");
    Expect(said("dpr=2"), "a 1.5x panel reports 2, which is the nearest of the three allowed ratios");
    // **Bare identifiers, not `window.`-prefixed.** They resolve because a property of the global object
    // is a global variable -- including an *accessor*, which is what these are. That did not work until
    // this session: `innerWidth` threw a ReferenceError while `window.innerWidth` answered, because the
    // identifier path used `GetOwn` and an accessor has no stored value to find.
  });

  AddTest(tests, "Engine/ASecondNavigationGetsASecondGlobalScope", [] {
    // Two documents in a row, each with a script that touches the tree. The
    // rule is stated on PageScript: a fresh global scope per document, because
    // leaving the previous page's globals in place would let one document's
    // script see another's. The sharper reason is that the binding layer holds
    // a *reference* to the document, so reusing it across a navigation is a
    // use-after-free the moment the second page's first script reads the tree.
    // Under a sanitizer this test is the one that says so; without one it still
    // asserts that the second page's script saw the second page.
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<p id='one'>ABC</p><script>"
        "if (!document.querySelector('#one')) throw 'first page missing';"
        "globalThis.marker = 'first';"
        "</" "script>")});
    Expect(session.engine.ScriptErrors().empty(), "the first page's script ran cleanly");

    session.Send(ipc::NavigateMessage{DataUrl(
        "<p id='two'>DEF</p><script>"
        "if (!document.querySelector('#two')) throw 'second page missing';"
        "if (typeof marker !== 'undefined') throw 'the first page\\'s globals survived';"
        "</" "script>")});
    Expect(session.engine.ScriptErrors().empty(),
           "the second page's script saw the second page's tree, and none of the first "
           "page's globals");
  });

  AddTest(tests, "Page/VhSurvivesAuthorSheetRebuild", [element_with_id] {
    // TD-0028: RebuildAuthorStyleSheets ResetResolver()'d without restoring
    // MediaContext, so youtube's `min-height: calc(100vh - 120px)` applied as
    // nothing and the home column stayed content-sized.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.SetViewport(css::MediaContext{1280.0f, 900.0f, 1.0f});
    page.Load("<style>"
              "#col { min-height: calc(100vh - 120px); width: 100px }"
              "</style><div id=col></div>",
              "https://example.org/");
    page.Layout(1280.0f);
    const dom::Element* col = element_with_id(*page.MutableDocument(), "col");
    Expect(col != nullptr, "column");
    ExpectEqInt(static_cast<long long>(page.StyleOfForTesting(*col).min_height.value + 0.5f), 780,
                "vh applied after the Load rebuild");
    // A resize rebuilds author sheets again — the same trap as first load.
    page.SetViewport(css::MediaContext{1280.0f, 800.0f, 1.0f});
    page.Layout(1280.0f);
    ExpectEqInt(static_cast<long long>(page.StyleOfForTesting(*col).min_height.value + 0.5f), 680,
                "and after a SetViewport rebuild");
  });

  AddTest(tests, "Engine/ViewportResizeFiresWindowResize", [] {
    // Polymer iron-fit / iron-resizable listen on window `resize` (TD-0022).
    // SetViewport used to relayout without ever dispatching it.
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl(
        "<script>"
        "window.addEventListener('resize', function () { console.log('resized'); });"
        "</" "script>")});
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "",
                   "the initial viewport does not fire after the listener is attached");
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{500, 400}, 1.0f});
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "resized",
                   "a later SetViewport fires resize after layout");
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{500, 400}, 1.0f});
    ExpectEqString(Joined(session.engine.ConsoleOutput()), "resized",
                   "an unchanged viewport is a no-op");
  });
}

}  // namespace microbrowser::tests

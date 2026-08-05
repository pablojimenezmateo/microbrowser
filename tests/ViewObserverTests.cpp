// ADR 0018 §5: `IntersectionObserver`, `ResizeObserver`, and `loading="lazy"`.
//
// The ADR's rule for this session is the one these tests are shaped around:
// *neither observer is shipped in a form that fires approximately*. An
// observer that exists and never fires is worse than one that does not exist,
// because a feed feature-detects the name and walks into the wall behind it.
// So there is a test per observable fact -- it fires, it fires once, it stops
// firing when nothing changed, it fires again when something did -- rather than
// one test that constructs an observer and asserts nothing threw.
//
// The lazy-image tests are at the other end and are the session's check: an
// image below the fold is *not fetched*, and the way to see that is to count
// the requests a scripted transport received.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/MediaQuery.h"
#include "engine/Engine.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"
#include "support/SyntheticPng.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::tests {

namespace {

using util::PerfCounterId;
using util::ReadPerformanceCounter;

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

// A page laid out in an 800x600 viewport, with its script run.
//
// Everything an observer reports comes back through `console.log`, which is
// the one channel a page has to say something out loud -- and reading it as
// text pins the *values* rather than just the fact that a callback ran.
struct Observed {
  TestFonts fonts;
  engine::Page page{fonts.catalog};

  void Load(std::string_view html) {
    page.Load(html, "https://example.org/");
    page.SetViewport(css::MediaContext{800.0f, 600.0f, 1.0f});
    page.Layout(800.0f);
    page.RunScripts(0);
  }

  // One frame's worth of observation. The engine does this inside its paint;
  // a test does it directly, because there is no window here to paint into.
  bool Frame(std::int64_t now_ms = 0) { return page.DeliverObservations(now_ms); }

  std::string Log() const {
    std::string out;
    for (const std::string& line : page.ConsoleOutput()) {
      out += out.empty() ? "" : "|";
      out += line;
    }
    return out;
  }

  std::string Errors() const {
    std::string out;
    for (const std::string& line : page.ScriptErrors()) {
      out += out.empty() ? "" : "|";
      out += line;
    }
    return out;
  }
};

// A 4x4 opaque PNG, as an HTTP response. Built rather than checked in, for the
// reason SyntheticPng exists at all: a test needs bytes whose content is known.
std::string PngResponse() {
  const std::vector<std::byte> png =
      BuildPng(PngSpec{4, 4, 8, 6, false, {}, {}, SolidRgbaRows(4, 4, 0x11, 0x22, 0x33, 0xFF), 0});
  std::string bytes;
  bytes.reserve(png.size());
  for (const std::byte byte : png) {
    bytes.push_back(static_cast<char>(byte));
  }
  return "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " +
         std::to_string(bytes.size()) + "\r\n\r\n" + bytes;
}

long long Counter(PerfCounterId id) {
  return static_cast<long long>(ReadPerformanceCounter(id));
}

}  // namespace

void RegisterViewObserverTests(std::vector<TestCase>& tests) {
  // --- IntersectionObserver -------------------------------------------------

  AddTest(tests, "IntersectionObserver/TheFirstSampleReportsWhereEverythingIs", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'>"
        "<div id='near' style='height:100px'></div>"
        "<div id='far' style='height:100px;margin-top:2000px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (entries) {"
        "  for (var i = 0; i < entries.length; i++) {"
        "    console.log(entries[i].target.id + ':' + entries[i].isIntersecting);"
        "  }"
        "});"
        "o.observe(document.getElementById('near'));"
        "o.observe(document.getElementById('far'));"
        "</script></body>");
    ExpectEqString(session.Errors(), "", "no script threw");
    Expect(session.Frame(), "the first frame after observe() delivers");
    // Both, and the one that is off screen too: `observe()` promises an initial
    // observation, and a lazy loader depends on being told that nothing is
    // visible yet rather than on hearing nothing at all.
    ExpectEqString(session.Log(), "near:true|far:false",
                   "the element in the viewport intersects and the one 2000px down does not");
  });

  AddTest(tests, "IntersectionObserver/ASettledPageDeliversNothing", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'><div id='a' style='height:100px'></div>"
        "<script>"
        "var n = 0;"
        "var o = new IntersectionObserver(function (e) { console.log('call ' + (++n)); });"
        "o.observe(document.getElementById('a'));"
        "</script></body>");
    Expect(session.Frame(), "the first frame delivers");
    Expect(!session.Frame(), "and the second does not, because nothing changed");
    Expect(!session.Frame(), "nor the third");
    ExpectEqString(session.Log(), "call 1",
                   "an observer fires on a change, not on a frame -- which is the whole "
                   "reason it is sampled rather than subscribed to");
  });

  AddTest(tests, "IntersectionObserver/ScrollingSomethingIntoViewFiresIt", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'>"
        "<div id='a' style='height:100px;margin-top:2000px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (entries) {"
        "  console.log('is:' + entries[0].isIntersecting);"
        "});"
        "o.observe(document.getElementById('a'));"
        "</script></body>");
    Expect(session.Frame(), "the initial observation");
    ExpectEqString(session.Log(), "is:false", "2000px down is not on screen");
    // The scroll itself must not deliver anything. It moves an offset; the
    // frame that follows is what samples.
    session.page.SetScrollOffsetY(1900.0f);
    ExpectEqString(session.Log(), "is:false", "a scroll does not run an observer callback");
    Expect(session.Frame(), "the frame after the scroll does");
    ExpectEqString(session.Log(), "is:false|is:true", "and now it is on screen");
  });

  AddTest(tests, "IntersectionObserver/RootMarginReachesBeyondTheScrollport", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'>"
        "<div id='a' style='height:100px;margin-top:800px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (e) {"
        "  console.log('is:' + e[0].isIntersecting);"
        "}, {rootMargin: '400px'});"
        "o.observe(document.getElementById('a'));"
        "</script></body>");
    Expect(session.Frame(), "delivered");
    // The box starts at y=800 in a 600-tall viewport: 200px below it, which a
    // 400px margin reaches and a zero margin does not. This is the mechanism
    // every lazy loader is built on.
    ExpectEqString(session.Log(), "is:true",
                   "400px of root margin reaches a box 200px below the fold");
  });

  AddTest(tests, "IntersectionObserver/ThresholdsAreCrossingsRatherThanFrames", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'>"
        "<div id='a' style='height:400px;margin-top:400px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (e) {"
        "  console.log(Math.round(e[0].intersectionRatio * 100) / 100);"
        "}, {threshold: [0, 0.5, 1]});"
        "o.observe(document.getElementById('a'));"
        "</script></body>");
    Expect(session.Frame(), "the initial observation");
    // 400..800 against a 0..600 viewport: half of it.
    ExpectEqString(session.Log(), "0.5", "half the box is on screen");
    session.page.SetScrollOffsetY(200.0f);
    Expect(session.Frame(), "the box is now fully visible, which crosses a threshold");
    ExpectEqString(session.Log(), "0.5|1", "and the ratio says so");
    session.page.SetScrollOffsetY(210.0f);
    Expect(!session.Frame(),
           "a scroll that crosses no threshold delivers nothing -- which is what the "
           "threshold list is for, and the difference between an observer and a "
           "scroll handler");
  });

  AddTest(tests, "IntersectionObserver/AnElementRootObservesOnlyItsOwnDescendants", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'>"
        "<div id='root' style='height:100px;overflow:auto'>"
        "<div id='inside' style='height:50px'></div>"
        "</div>"
        "<div id='outside' style='height:50px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (entries) {"
        "  for (var i = 0; i < entries.length; i++) {"
        "    console.log(entries[i].target.id + ':' + entries[i].isIntersecting);"
        "  }"
        "}, {root: document.getElementById('root')});"
        "o.observe(document.getElementById('inside'));"
        "o.observe(document.getElementById('outside'));"
        "</script></body>");
    ExpectEqString(session.Errors(), "", "no script threw");
    Expect(session.Frame(), "delivered");
    // `outside` is geometrically right below the root and would overlap a
    // viewport root. It is not a descendant, so it does not intersect this one.
    ExpectEqString(session.Log(), "inside:true|outside:false",
                   "an element root observes its own subtree, not whatever happens to "
                   "overlap it on screen");
  });

  AddTest(tests, "IntersectionObserver/DisconnectDropsWhatWasQueuedToo", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'><div id='a' style='height:100px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (e) { console.log('fired'); });"
        "o.observe(document.getElementById('a'));"
        "o.disconnect();"
        "</script></body>");
    Expect(!session.Frame(),
           "a disconnected observer fires neither for what it watched nor for what it "
           "had already queued -- either alone would be the worst of both");
    ExpectEqString(session.Log(), "", "nothing was said");
  });

  AddTest(tests, "IntersectionObserver/TheQueueIsTakenBeforeTheCallbackSeesIt", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'><div id='a' style='height:100px'></div>"
        "<script>"
        "var o = new IntersectionObserver(function (entries) {"
        "  console.log('got ' + entries.length + ', left ' + o.takeRecords().length);"
        "});"
        "o.observe(document.getElementById('a'));"
        "</script></body>");
    Expect(session.Frame(), "the frame samples and delivers");
    // The records are handed over and the queue emptied *before* the callback
    // runs, which is what stops a callback that calls takeRecords from
    // receiving the same record twice.
    ExpectEqString(session.Log(), "got 1, left 0", "the callback holds the only copy");
  });

  // --- ResizeObserver -------------------------------------------------------

  AddTest(tests, "ResizeObserver/ReportsTheContentBoxAndThenOnlyChanges", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'>"
        "<div id='a' style='width:120px;height:40px;padding:10px;border:5px solid black'></div>"
        "<script>"
        "var o = new ResizeObserver(function (entries) {"
        "  var e = entries[0];"
        "  console.log(e.contentRect.width + 'x' + e.contentRect.height + ' @' +"
        "              e.contentRect.x + ',' + e.contentRect.y +"
        "              ' border ' + e.borderBoxSize[0].inlineSize);"
        "});"
        "o.observe(document.getElementById('a'));"
        "</script></body>");
    ExpectEqString(session.Errors(), "", "no script threw");
    Expect(session.Frame(), "the initial observation");
    // The content rect's origin is the padding, not the viewport: 10px in from
    // the padding box on both axes. The border box is 120 + 20 + 10.
    ExpectEqString(session.Log(), "120x40 @10,10 border 150",
                   "the content box, positioned inside the padding box, and the border box");
    Expect(!session.Frame(), "a page that did not resize hears nothing further");
  });

  AddTest(tests, "ResizeObserver/AResizeFromScriptIsReportedAtTheNextFrame", [] {
    Observed session;
    session.Load(
        "<body style='margin:0'><div id='a' style='width:100px;height:20px'></div>"
        "<script>"
        "var o = new ResizeObserver(function (e) {"
        "  console.log('w=' + e[0].contentRect.width);"
        "});"
        "o.observe(document.getElementById('a'));"
        "setTimeout(function () {"
        "  document.getElementById('a').style.width = '300px';"
        "}, 0);"
        "</script></body>");
    Expect(session.Frame(0), "the initial observation");
    ExpectEqString(session.Log(), "w=100", "as laid out");
    Expect(session.page.RunDueWork(1), "the timer ran and widened the box");
    Expect(session.Frame(2), "the frame after it reports the new size");
    ExpectEqString(session.Log(), "w=100|w=300",
                   "the observer reported the width the element actually has now, which "
                   "means the sample ran against a relaid-out box rather than a stale one");
  });

  AddTest(tests, "ResizeObserver/ACallbackThatResizesWhatItObservesIsCutOff", [] {
    Observed session;
    const long long before = Counter(PerfCounterId::ViewResizeLoopLimit);
    session.Load(
        "<body style='margin:0'><div id='a' style='width:10px;height:10px'></div>"
        "<script>"
        "var n = 0;"
        "var o = new ResizeObserver(function (e) {"
        "  n++;"
        "  document.getElementById('a').style.width = (10 + n * 10) + 'px';"
        "});"
        "o.observe(document.getElementById('a'));"
        "window.count = function () { return n; };"
        "</script></body>");
    Expect(session.Frame(), "it delivered");
    // The loop terminates. That is the assertion: without the depth bound this
    // test hangs, which is a thing a page can cause on purpose.
    Expect(Counter(PerfCounterId::ViewResizeLoopLimit) > before,
           "the depth bound was reached and counted rather than the browser spinning");
  });

  AddTest(tests, "ResizeObserver/DevicePixelContentBoxIsRefusedRatherThanFaked", [] {
    Observed session;
    session.Load(
        "<body><div id='a'></div>"
        "<script>"
        "try {"
        "  new ResizeObserver(function () {}).observe(document.getElementById('a'),"
        "                                            {box: 'device-pixel-content-box'});"
        "  console.log('accepted');"
        "} catch (e) { console.log('refused'); }"
        "</script></body>");
    // ADR 0012 and ADR 0029: it is the device pixel ratio times a size, and a
    // CSS-pixel answer under that name renders a canvas at the wrong
    // resolution. A page that feature-detects it gets the fallback it wrote.
    ExpectEqString(session.Log(), "refused", "not silently answered in the wrong units");
  });

  // --- the seam -------------------------------------------------------------

  AddTest(tests, "ViewObservers/APageWithNoObserversSamplesNothing", [] {
    Observed session;
    const long long before = Counter(PerfCounterId::ViewObservationFrames);
    session.Load("<body><p>ABC</p><script>console.log('ran');</script></body>");
    for (int frame = 0; frame < 20; ++frame) {
      Expect(!session.Frame(frame), "nothing to deliver");
    }
    ExpectEqString(session.Log(), "ran", "the page's own script ran");
    ExpectEqInt(Counter(PerfCounterId::ViewObservationFrames) - before, 0,
                "twenty frames on a page that constructed no observer cost no sampling "
                "at all -- the observers are not a per-frame tax");
  });

  AddTest(tests, "ViewObservers/InnerWidthAndHeightAreTheScrollport", [] {
    Observed session;
    session.Load("<body><script>console.log(window.innerWidth + 'x' + window.innerHeight);"
                 "</script></body>");
    ExpectEqString(session.Log(), "800x600",
                   "the viewport the page was given, and not undefined -- which is what a "
                   "carousel that sizes itself from it was reading before");
  });

  // --- loading=\"lazy\" ------------------------------------------------------

  AddTest(tests, "LazyImages/AnImageBelowTheFoldIsNotFetched", [] {
    TestFonts fonts;
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts.catalog};
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'>"
                   "<img loading='lazy' src='/top.png' width='40' height='40'>"
                   "<div style='height:4000px'></div>"
                   "<img loading='lazy' src='/bottom.png' width='40' height='40'>"
                   "</body>")});
    factory.script.push_back(
        ScriptedTransport::Exchange{"example.org", 443, true, PngResponse()});
    engine.PageLoader().SetTransport(factory);

    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);

    // The document and exactly one image. `bottom.png` is 4000px down, which is
    // more than one viewport height past the fold, so nothing asked for it.
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the document and the image on screen -- not the one below the fold");
    for (const std::string& request : factory.log.requests) {
      Expect(request.find("/bottom.png") == std::string::npos,
             "and nothing asked for the image 4000px down the page");
    }
  });

  AddTest(tests, "LazyImages/ScrollingTowardsOneFetchesIt", [] {
    TestFonts fonts;
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts.catalog};
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'>"
                   "<div style='height:4000px'></div>"
                   "<img loading='lazy' src='/bottom.png' width='40' height='40'>"
                   "</body>")});
    factory.script.push_back(
        ScriptedTransport::Exchange{"example.org", 443, true, PngResponse()});
    engine.PageLoader().SetTransport(factory);

    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 1,
                "only the document so far");

    ipc::ScrollMessage scroll;
    scroll.delta_y = 3900;
    scroll.position = gfx::IntPoint{10, 10};
    channel.Ui().Send(scroll);
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);

    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the image came within reach of the scrollport and was fetched -- after "
                "the navigation that carried the document was over, which is the first "
                "request this browser makes outside a load");
    Expect(factory.log.requests.at(1).find("/bottom.png") != std::string::npos,
           "and it is the right one");
  });

  AddTest(tests, "LazyImages/AnEagerImageIsStillFetchedImmediately", [] {
    TestFonts fonts;
    ipc::InProcessChannel channel;
    engine::Engine engine{channel.Engine(), fonts.catalog};
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'>"
                   "<div style='height:4000px'></div>"
                   // No `loading` attribute, and an invalid one: both are eager,
                   // which is the enumerated attribute's invalid-value default
                   // and the safe direction to be wrong in.
                   "<img src='/a.png'><img loading='eagre' src='/b.png'>"
                   "</body>")});
    for (int i = 0; i < 2; ++i) {
      factory.script.push_back(ScriptedTransport::Exchange{"example.org", 443, true,
                                                           PngResponse()});
    }
    engine.PageLoader().SetTransport(factory);

    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://example.org/page.html"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);

    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 3,
                "both images fetched with the document: `lazy` is the only value that "
                "defers, and a typo must not make an image never load");
  });
}

}  // namespace microbrowser::tests

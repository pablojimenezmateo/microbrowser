// The geometry seam: a page asking its own layout a question.
//
// ADR 0015 names the failure mode these tests exist for. `getBoundingClientRect`
// is defined to return the position a box *would have* if layout were up to
// date, so a rectangle read after a mutation and before a relayout is not
// slightly stale -- it describes a page that no longer exists. The ADR is
// explicit that "missing one produces a stale rect... It needs a test that
// mutates and immediately queries, for each kind of mutation", which is what
// MutationsForceLayout is.

#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "bindings/DomBindings.h"
#include "engine/Page.h"
#include "html/TreeBuilder.h"
#include "js/Interpreter.h"
#include "gfx/FontCatalog.h"
#include "util/PerformanceCounters.h"
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

// A page that has run one script and can be asked what it logged.
//
// Everything here goes through `console.log`, because that is the one channel a
// page has to report a value out and it is the same channel a real page's
// debugging goes down. Reading the numbers back as text also pins the
// *serialization*, which is half of what `getComputedStyle` is.
std::vector<std::string> RunAndCollect(engine::Page& page, std::string_view html) {
  page.Load(html, "https://example.org/");
  page.SetViewport(css::MediaContext{800.0f, 600.0f, 1.0f});
  page.Layout(800.0f);
  page.RunScripts(0);
  return page.ConsoleOutput();
}

std::string Line(const std::vector<std::string>& output, std::size_t index) {
  return index < output.size() ? output[index] : std::string("<missing>");
}

}  // namespace

void RegisterGeometryQueryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Geometry/BoundingClientRectIsTheBorderBox", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'>"
        "<div id='a' style='width:120px;height:40px;padding:10px;"
        "border:5px solid black;margin:20px'></div>"
        "<script>"
        "var r = document.getElementById('a').getBoundingClientRect();"
        "console.log(r.x + ',' + r.y + ',' + r.width + ',' + r.height);"
        "console.log(r.left + ',' + r.top + ',' + r.right + ',' + r.bottom);"
        "</script></body>");
    // 120 content + 20 padding + 10 border = 150 wide, 40 + 20 + 10 = 70 tall,
    // offset by the 20px margin on both axes.
    ExpectEqString(Line(output, 0), "20,20,150,70", "the border box, not the content box");
    ExpectEqString(Line(output, 1), "20,20,170,90", "and the same four numbers under both names");
  });

  AddTest(tests, "Geometry/OffsetAndClientMetricsDifferByTheBorder", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'>"
        "<div id='a' style='width:100px;height:30px;padding:4px;"
        "border:3px solid black'></div>"
        "<script>var e = document.getElementById('a');"
        "console.log(e.offsetWidth + 'x' + e.offsetHeight);"
        "console.log(e.clientWidth + 'x' + e.clientHeight);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "114x44", "offset* is the border box");
    ExpectEqString(Line(output, 1), "108x38", "client* is the padding box");
  });

  AddTest(tests, "Geometry/AnElementWithNoBoxIsAllZeros", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body><div id='a' style='display:none;width:100px'></div>"
        "<script>var e = document.getElementById('a');"
        "var r = e.getBoundingClientRect();"
        "console.log(r.x + ',' + r.y + ',' + r.width + ',' + r.height);"
        "console.log(e.offsetWidth + ',' + e.clientHeight);"
        // A hidden element still has a computed style, which is what the
        // specification says and what a page that measures before showing
        // depends on.
        "console.log(getComputedStyle(e).display);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "0,0,0,0", "no box means no geometry, honestly reported");
    ExpectEqString(Line(output, 1), "0,0", "and the same for the integer metrics");
    ExpectEqString(Line(output, 2), "none", "but the cascade still has an answer");
  });

  AddTest(tests, "Geometry/AnInlineElementReportsItsFragments", [] {
    // An Inline box carries no geometry of its own -- its content lives in the
    // line boxes of its container -- so the union of its fragments is the only
    // honest answer. Without it every <a> and <span> on every page reports a
    // zero rectangle.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        // The synthetic font has glyphs for A, B, C, D and the space, and
        // nothing else -- a word outside that set measures zero and would make
        // this test pass for the wrong reason.
        "<body style='margin:0'><p><span id='a'>ABC</span></p>"
        "<script>var r = document.getElementById('a').getBoundingClientRect();"
        "console.log(r.width > 0 && r.height > 0);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "true", "an inline element has a rectangle");
  });

  AddTest(tests, "Geometry/AMutationForcesLayoutBeforeTheAnswer", [] {
    // The session's check, and the ADR's central claim: a page that mutates a
    // style and immediately reads a rect gets the post-mutation rect, and the
    // forced layout is visible in the counters.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const util::PerfCounterSnapshot before = util::CapturePerformanceCounters();
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'><div id='a' style='width:100px;height:10px'></div>"
        "<script>var e = document.getElementById('a');"
        "console.log(e.getBoundingClientRect().width);"
        "e.style.width = '250px';"
        "console.log(e.getBoundingClientRect().width);"
        "e.style.width = '75px';"
        "console.log(e.getBoundingClientRect().width);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "100", "the width the document asked for");
    ExpectEqString(Line(output, 1), "250", "the width the script just wrote");
    ExpectEqString(Line(output, 2), "75", "and again");

    const util::PerfCounterSnapshot after = util::CapturePerformanceCounters();
    const std::uint64_t forced =
        after[static_cast<std::size_t>(util::PerfCounterId::LayoutForcedByScript)] -
        before[static_cast<std::size_t>(util::PerfCounterId::LayoutForcedByScript)];
    // Two, not three: the first read came after the engine's own layout with
    // nothing changed since, so it cost nothing. That is the layout-clean flag
    // doing its job rather than an accident of ordering.
    ExpectEqInt(static_cast<long long>(forced), 2, "one forced layout per mutation, and no more");
  });

  AddTest(tests, "Geometry/ReadingTwiceWithoutMutatingForcesNothing", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const util::PerfCounterSnapshot before = util::CapturePerformanceCounters();
    RunAndCollect(page,
                  "<body><div id='a'>x</div><script>"
                  "var e = document.getElementById('a');"
                  "for (var i = 0; i < 20; i++) { e.getBoundingClientRect(); }"
                  "</script></body>");
    const util::PerfCounterSnapshot after = util::CapturePerformanceCounters();
    const std::uint64_t forced =
        after[static_cast<std::size_t>(util::PerfCounterId::LayoutForcedByScript)] -
        before[static_cast<std::size_t>(util::PerfCounterId::LayoutForcedByScript)];
    ExpectEqInt(static_cast<long long>(forced), 0, "a clean layout answers for free");
  });

  AddTest(tests, "Geometry/MutationsForceLayout", [] {
    // One case per kind of mutation, because the failure mode is *missing one*
    // and a single case would pass with six of the seven marks absent. Each
    // changes the document some other way and then asks whether the answer
    // moved with it. They are marked in `dom` -- at the five primitives every
    // mutation anywhere goes through -- rather than at the binding that caused
    // them, which is what makes "did we remember to mark this one" not a
    // question anybody has to ask again.
    struct Case {
      const char* name;
      const char* html;
      const char* script;
      const char* expected;
    };
    const Case kCases[] = {
        {"a style property",
         "<div id='a'></div>",
         "e.style.width = '250px'; log(e.getBoundingClientRect().width);", "250"},
        {"setAttribute",
         "<div id='a'></div>",
         "e.setAttribute('style', 'width:250px'); log(e.getBoundingClientRect().width);", "250"},
        {"removeAttribute",
         "<div id='a' style='width:250px'></div>",
         "e.removeAttribute('style'); log(e.getBoundingClientRect().width);", "100"},
        // Through the reflected IDL property rather than through setAttribute,
        // which is the form `Object.assign(el, {...})` takes.
        {"a reflected property",
         "<div id='a'></div>",
         "e.className = 'wide'; log(e.getBoundingClientRect().width);", "250"},
        {"appendChild",
         "<div id='a'></div>",
         "var w = document.createElement('div'); w.className = 'wide';"
         "document.body.appendChild(w); log(w.getBoundingClientRect().width);", "250"},
        {"removeChild",
         "<div class='tall'></div><div id='a'></div>",
         "document.body.removeChild(document.body.firstChild);"
         "log(e.getBoundingClientRect().y);", "0"},
        {"textContent",
         "<div id='a'></div>",
         "e.textContent = 'ABCD'; log(e.getBoundingClientRect().height > 0);", "true"},
    };
    for (const Case& entry : kCases) {
      TestFonts fonts;
      engine::Page page(fonts.catalog);
      // The starting width is a rule rather than an inline style, so that the
      // class case is not fighting a declaration that outranks it.
      const std::string html =
          std::string("<head><style>div{width:100px}.wide{width:250px}.tall{height:40px}"
                      "</style></head><body style='margin:0'>") +
          entry.html + "<script>var log = console.log;" +
          "var e = document.getElementById('a');" +
          // Read once while the layout is clean, so that what follows is the
          // mutation's doing and not the first query's.
          "e.getBoundingClientRect();" + entry.script + "</script></body>";
      const std::vector<std::string> output = RunAndCollect(page, html);
      ExpectEqString(Line(output, 0), entry.expected, entry.name);
    }
  });

  AddTest(tests, "Geometry/ComputedStyleResolvesWidthToPixels", [] {
    // The half of `getComputedStyle` ADR 0015 calls the difficult one: `width`
    // is a *used* value, so an element that never named a width answers with
    // the number layout gave it and not with the `auto` the cascade holds.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'><div id='a' style='padding:6px'>x</div>"
        "<script>var s = getComputedStyle(document.getElementById('a'));"
        "console.log(s.width);"
        "console.log(s.paddingLeft);"
        "console.log(s.getPropertyValue('padding-left'));"
        "console.log(s.display);"
        "console.log(s.color);"
        "console.log(s.transform === '');"
        "</script></body>");
    // 800 viewport less two 6px paddings: the used value, which no amount of
    // reading the cascade would produce.
    ExpectEqString(Line(output, 0), "788px", "width is the used value in pixels");
    ExpectEqString(Line(output, 1), "6px", "and so is padding");
    ExpectEqString(Line(output, 2), "6px", "getPropertyValue and the property agree");
    ExpectEqString(Line(output, 3), "block", "a keyword property is the computed value");
    ExpectEqString(Line(output, 4), "rgb(0, 0, 0)", "a colour is rgb(), which is what engines say");
    ExpectEqString(Line(output, 5), "true",
                   "a property this engine does not have reads back empty, not invented");
  });

  AddTest(tests, "Geometry/ComputedStyleFollowsAMutation", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<body style='margin:0'><div id='a' style='width:100px'></div>"
        "<script>var e = document.getElementById('a');"
        "console.log(getComputedStyle(e).width);"
        "e.style.width = '300px';"
        "console.log(getComputedStyle(e).width);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "100px", "before");
    ExpectEqString(Line(output, 1), "300px", "and after, without a frame in between");
  });

  AddTest(tests, "Geometry/RectsAreViewportRelative", [] {
    // `getBoundingClientRect` is defined against the viewport, not the
    // document, so it moves when the page scrolls. The offset it subtracts is
    // the same one Paint translates by, which is why the page owns it.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><div style='height:500px'></div>"
        "<div id='a' style='height:20px'></div>"
        "<script>console.log(document.getElementById('a').getBoundingClientRect().y);"
        "</script></body>",
        "https://example.org/");
    page.SetViewport(css::MediaContext{800.0f, 600.0f, 1.0f});
    page.Layout(800.0f);
    page.SetScrollOffsetY(120.0f);
    page.RunScripts(0);
    ExpectEqString(Line(page.ConsoleOutput(), 0), "380",
                   "500 down the document, 120 of it scrolled away");
  });

  AddTest(tests, "Geometry/NamesAreAbsentWithoutALayoutToAnswer", [] {
    // ADR 0012's rule, applied to this seam. A page that feature-detects
    // `getBoundingClientRect` and finds a stub walks into the wall behind it;
    // one that finds nothing takes the polyfill path that works. So a binding
    // layer with no GeometrySource behind it does not declare the names at all.
    std::unique_ptr<dom::Document> document = html::ParseDocument("<body><p>x</p></body>");
    js::Interpreter interpreter;
    bindings::DomBindings bound(interpreter, *document, "https://example.org/");
    bound.Install();
    ExpectEqString(js::ToString(
                       interpreter.Run("typeof document.body.getBoundingClientRect").value),
                   "undefined", "no layout, so no name to detect");
    ExpectEqString(js::ToString(interpreter.Run("typeof getComputedStyle").value), "undefined",
                   "and none on the window either");

    // And present the moment there is a layout to answer with, which is the
    // other half of the same claim.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    const std::vector<std::string> output =
        RunAndCollect(page,
                      "<body><script>"
                      "console.log(typeof document.body.getBoundingClientRect);"
                      "console.log(typeof getComputedStyle);"
                      "</script></body>");
    ExpectEqString(Line(output, 0), "function", "a page with a layout behind it has the name");
    ExpectEqString(Line(output, 1), "function", "and the window one too");
  });

  AddTest(tests, "Geometry/AProbeAsksThePageItsOwnQuestions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    RunAndCollect(page, "<body><div id=a>x</div><script>globalThis.ran = 1;</script></body>");
    // The page's *own* interpreter, which is the whole point: a probe against
    // a fresh one would answer about a page that never ran.
    ExpectEqString(page.EvaluateScript("'' + ran"), "1", "the probe sees what the page defined");
    ExpectEqString(page.EvaluateScript("document.getElementById('a').textContent"), "x",
                   "and the tree the page has");
    // A throw is an answer rather than a crash, because most of what a probe
    // asks about is something that may not be there.
    ExpectEqString(page.EvaluateScript("nope.x"), "throw ReferenceError: nope is not defined",
                   "a probe that throws says so");
    // A probe that changes the page is laid out before the caller's next
    // frame, so `-eval` followed by a snapshot shows what the probe did.
    ExpectEqString(page.EvaluateScript(
                       "document.getElementById('a').textContent = 'probed';"
                       "document.getElementById('a').textContent"),
                   "probed", "a probe may change the document");
  });

  AddTest(tests, "Geometry/MatchMediaAgreesWithTheStylesheetAndWithInnerWidth", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    // The point of routing `matchMedia` through the geometry seam rather than
    // giving the binding layer a media context of its own: the stylesheet's
    // answer, the script's answer and `window.innerWidth` are one answer. A
    // page whose CSS thinks it is narrow and whose script thinks it is wide
    // renders a layout neither of them describes.
    const std::vector<std::string> output = RunAndCollect(
        page,
        "<style>@media (max-width: 700px) { #a { color: red } }"
        "@media (min-width: 700px) { #a { color: green } }</style>"
        "<body><p id=a>x</p><script>"
        "console.log(matchMedia('(min-width: 700px)').matches);"
        "console.log(matchMedia('(max-width: 700px)').matches);"
        "console.log(getComputedStyle(document.getElementById('a')).color);"
        "console.log(window.innerWidth);"
        "console.log(matchMedia('(orientation: landscape)').matches);"
        // A feature this evaluator does not implement is false rather than a
        // guess -- every media feature is something this browser tells a page
        // about the machine it is on. ADR 0029.
        "console.log(matchMedia('(min-resolution: 2dppx)').matches);"
        "console.log(matchMedia('(totally-invented-feature: 3)').matches);"
        "console.log(matchMedia('(min-width: 700px)').media);"
        "</script></body>");
    ExpectEqString(Line(output, 0), "true", "800 is at least 700");
    ExpectEqString(Line(output, 1), "false", "and not at most 700");
    ExpectEqString(Line(output, 2), "rgb(0, 128, 0)", "the cascade agrees with the script");
    ExpectEqString(Line(output, 3), "800", "and so does innerWidth");
    ExpectEqString(Line(output, 4), "true", "800x600 is landscape");
    ExpectEqString(Line(output, 5), "false", "a 1x display is not 2dppx");
    ExpectEqString(Line(output, 6), "false", "an unknown feature does not match");
    ExpectEqString(Line(output, 7), "(min-width: 700px)", "the list remembers its query");
  });

  AddTest(tests, "Geometry/AMatchMediaListenerFiresWhenTheViewportCrossesIt", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body><script>"
        "const narrow = matchMedia('(max-width: 700px)');"
        "narrow.addEventListener('change', e => console.log('listener:' + e.matches));"
        "narrow.onchange = e => console.log('onchange:' + e.matches);"
        // The older spelling, which a great deal of shipped script still uses
        // and which has to reach the same registry rather than a second one.
        "matchMedia('(max-width: 700px)').addListener(e => console.log('legacy:' + e.matches));"
        "</script></body>",
        "https://example.org/");
    page.SetViewport(css::MediaContext{800.0f, 600.0f, 1.0f});
    page.Layout(800.0f);
    page.RunScripts(0);
    page.DeliverObservations(0);
    ExpectEqInt(static_cast<long long>(page.ConsoleOutput().size()), 0,
                "nothing fires while the answer has not moved");

    // Across the boundary, which is the only thing that fires anything.
    page.SetViewport(css::MediaContext{500.0f, 600.0f, 1.0f});
    page.Layout(500.0f);
    page.DeliverObservations(16);
    ExpectEqString(Line(page.ConsoleOutput(), 0), "onchange:true",
                   "the handler property fires first, with the new answer");
    ExpectEqString(Line(page.ConsoleOutput(), 1), "listener:true", "then the listener");
    ExpectEqString(Line(page.ConsoleOutput(), 2), "legacy:true",
                   "and the older spelling reaches the same registry");

    // And not again while it stays there, which is the property that makes
    // this a *change* event rather than a per-frame callback.
    page.DeliverObservations(32);
    ExpectEqInt(static_cast<long long>(page.ConsoleOutput().size()), 3,
                "and nothing fires again while it stays there");
  });
}

}  // namespace microbrowser::tests

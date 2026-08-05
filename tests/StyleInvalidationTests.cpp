#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TestSupport.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "engine/Engine.h"
#include "gfx/FontCatalog.h"
#include "html/TreeBuilder.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/DriveLoop.h"
#include "support/SyntheticFont.h"
#include "util/PerformanceCounters.h"

// ADR 0016 §2-3: the dynamic pseudo-classes, and what a change to one costs.
//
// The first half is ordinary matcher work -- set a bit, assert a rule now
// applies -- and the ADR asks for exactly that, per bit, because "the engine
// gains the obligation to set state bits, and every bit it forgets is a rule
// that never applies".
//
// The second half is the sibling of IdleWaitStrategyTests, and it is here for
// the same reason that file exists: it guards a property that decays silently.
// A browser that restyles the document on every mouse move renders identically
// to one that does not, and the difference only shows up as a fan. The check
// this session is finished by is `MouseCrossingAPageWithNoHoverRulesCostsNothing`
// below, stated in counters because there is no other observable.

namespace microbrowser::tests {

namespace {

using css::StyleChangeEffect;
using dom::Element;
using dom::ElementState;
using util::PerfCounterId;
using util::ReadPerformanceCounter;

// Parses `html`, finds the first `target_tag`, applies `mutate` to it, and says
// whether `selector_text` then matches. The mutation is where a state bit is
// set, which is the whole point: the matcher is testable by setting a bit
// rather than by simulating a pointer, which is what keeps `src/css` free of
// `src/engine`.
template <typename Mutate>
bool MatchesAfter(std::string_view selector_text, std::string_view html,
                  std::string_view target_tag, Mutate&& mutate) {
  const std::vector<css::Selector> selectors = css::ParseSelectorList(selector_text);
  Expect(!selectors.empty(), std::string("selector did not parse: ") + std::string(selector_text));
  const std::unique_ptr<dom::Document> document = html::ParseDocument(html);
  Element* target = document->FirstElementByTagName(target_tag);
  Expect(target != nullptr, "target element not found");
  mutate(*document, *target);
  return selectors.front().Matches(*target);
}

bool MatchesWithState(std::string_view selector_text, std::string_view html,
                      std::string_view target_tag, ElementState state) {
  return MatchesAfter(selector_text, html, target_tag,
                      [state](dom::Document&, Element& element) { element.SetState(state, true); });
}

css::StyleInvalidation IndexOf(std::string_view css_text) {
  css::StyleInvalidation index;
  const css::StyleSheet sheet = css::ParseStyleSheet(css_text);
  for (const css::StyleRule& rule : sheet.rules) {
    for (const css::Selector& selector : rule.selectors) {
      index.AddRule(selector, rule.declarations);
    }
  }
  return index;
}

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
    catalog.SetGenericFamily("sans-serif", "Test");
  }
};

// One loaded page, and a pointer that can be moved across it.
struct HoverSession {
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

  void Load(std::string_view html) {
    Send(ipc::ResizeViewportMessage{gfx::IntSize{800, 600}, 1.0f});
    Send(ipc::NavigateMessage{std::string("data:text/html,") + std::string(html)});
  }

  void MoveTo(float x, float y) {
    ipc::PointerInputMessage pointer;
    pointer.kind = ipc::PointerInputMessage::Kind::Move;
    pointer.position = gfx::FloatPoint{x, y};
    Send(pointer);
  }
};

// The three counters the check is phrased in. Captured as one value so a test
// states "these three did not move" in one line rather than three.
struct RenderCost {
  std::uint64_t styles = 0;
  std::uint64_t layouts = 0;
  std::uint64_t paints = 0;

  static RenderCost Now() {
    return RenderCost{ReadPerformanceCounter(PerfCounterId::CssStylesResolved),
                      ReadPerformanceCounter(PerfCounterId::LayoutRuns),
                      ReadPerformanceCounter(PerfCounterId::EnginePaintsProduced)};
  }
};

}  // namespace

void RegisterStyleInvalidationTests(std::vector<TestCase>& tests) {
  // --- the bits, one test per state ------------------------------------------

  AddTest(tests, "DynamicState/HoverIsABitOnTheElement", [] {
    Expect(MatchesWithState("a:hover", "<a href=x>t</a>", "a", ElementState::Hover),
           ":hover must match an element the engine has marked hovered");
    Expect(!MatchesWithState("a:hover", "<a href=x>t</a>", "a", ElementState::Active),
           "and must not match one it has only marked active");
  });

  AddTest(tests, "DynamicState/ActiveChecked", [] {
    Expect(MatchesWithState("button:active", "<button>t</button>", "button", ElementState::Active),
           ":active");
    Expect(MatchesWithState("input:checked", "<input type=checkbox>", "input",
                            ElementState::Checked),
           ":checked");
  });

  AddTest(tests, "DynamicState/DisabledAndEnabledAreNotComplements", [] {
    Expect(MatchesWithState("input:disabled", "<input>", "input", ElementState::Disabled),
           ":disabled matches a control the engine marked disabled");
    Expect(!MatchesWithState("input:enabled", "<input>", "input", ElementState::Disabled),
           "and :enabled does not");
    Expect(MatchesWithState("input:enabled", "<input>", "input", ElementState::Hover),
           "an unmarked control is enabled");
    // The bug the tag list exists to prevent: `:enabled` reading as "not
    // disabled" would style every element on the page, because a <div> is
    // neither.
    Expect(!MatchesWithState("div:enabled", "<div>t</div>", "div", ElementState::Hover),
           "a div is neither enabled nor disabled");
    Expect(!MatchesWithState("div:disabled", "<div>t</div>", "div", ElementState::Disabled),
           "and marking one changes nothing");
  });

  AddTest(tests, "DynamicState/RequiredOptionalPlaceholderTarget", [] {
    Expect(MatchesWithState("input:required", "<input>", "input", ElementState::Required),
           ":required");
    Expect(MatchesWithState("input:optional", "<input>", "input", ElementState::Hover),
           ":optional is the complement, for a control that can be required");
    Expect(!MatchesWithState("div:optional", "<div>t</div>", "div", ElementState::Hover),
           "and a div is neither");
    Expect(MatchesWithState("input:placeholder-shown", "<input placeholder=p>", "input",
                            ElementState::PlaceholderShown),
           ":placeholder-shown");
    Expect(MatchesWithState("h1:target", "<h1 id=x>t</h1>", "h1", ElementState::Target), ":target");
  });

  // Focus is the one that is *not* a bit, and this is the test that says so:
  // writing the bit must change nothing, because there is one copy of focus and
  // it is on the document. A second copy is the bug the focus model removed.
  AddTest(tests, "DynamicState/FocusComesFromTheDocumentAndNotFromABit", [] {
    Expect(!MatchesWithState("input:focus", "<input>", "input", ElementState::Focus),
           "setting a Focus bit must not make :focus match -- focus is not stored on the element");
    Expect(MatchesAfter("input:focus", "<input>", "input",
                        [](dom::Document& document, Element& element) {
                          document.SetFocus(&element, false);
                        }),
           ":focus matches the document's focused element");
    Expect(!MatchesAfter("input:focus-visible", "<input>", "input",
                         [](dom::Document& document, Element& element) {
                           document.SetFocus(&element, false);
                         }),
           ":focus-visible does not match focus a pointer moved");
    Expect(MatchesAfter("input:focus-visible", "<input>", "input",
                        [](dom::Document& document, Element& element) {
                          document.SetFocus(&element, true);
                        }),
           "and does match focus the keyboard moved");
  });

  AddTest(tests, "DynamicState/FocusWithinReachesTheAncestors", [] {
    const auto focus_the_input = [](dom::Document& document, Element&) {
      document.SetFocus(document.FirstElementByTagName("input"), true);
    };
    Expect(MatchesAfter("form:focus-within", "<form><p><input></p></form>", "form",
                        focus_the_input),
           ":focus-within matches an ancestor of the focused element");
    Expect(!MatchesAfter("form:focus", "<form><p><input></p></form>", "form", focus_the_input),
           "and :focus does not");
    Expect(!MatchesAfter("form:focus-within", "<form></form><input>", "form", focus_the_input),
           "nor does :focus-within match an element the focus is not inside");
  });

  AddTest(tests, "DynamicState/StateNestsInsideTheFunctionalPseudoClasses", [] {
    Expect(MatchesWithState("a:not(:hover)", "<a href=x>t</a>", "a", ElementState::Active),
           ":not(:hover) matches an element that is not hovered");
    Expect(!MatchesWithState("a:not(:hover)", "<a href=x>t</a>", "a", ElementState::Hover),
           "and not one that is -- which is the case that was wrong before the bit existed");
    Expect(MatchesWithState(":is(a, button):hover", "<a href=x>t</a>", "a", ElementState::Hover),
           ":is() then :hover");
  });

  AddTest(tests, "DynamicState/HoverOnADescendantCombinator", [] {
    // `nav:hover .menu` is the whole reason the state goes on the ancestors as
    // well as on the element under the pointer.
    const std::vector<css::Selector> selectors = css::ParseSelectorList("nav:hover .menu");
    const std::unique_ptr<dom::Document> document =
        html::ParseDocument("<nav><ul class=menu><li>t</li></ul></nav>");
    Element* nav = document->FirstElementByTagName("nav");
    Element* menu = document->FirstElementByTagName("ul");
    Expect(!selectors.front().Matches(*menu), "nothing is hovered yet");
    nav->SetState(ElementState::Hover, true);
    Expect(selectors.front().Matches(*menu), "hovering the nav styles the menu inside it");
  });

  // --- the index -------------------------------------------------------------

  AddTest(tests, "StyleInvalidation/ARuleWithNoDynamicStateIsFiledUnderNothing", [] {
    const css::StyleInvalidation index = IndexOf(".a { color: red } #b div { width: 3px }");
    Expect(!index.DependsOn(ElementState::Hover), "no rule mentions :hover");
    Expect(index.EffectOf(ElementState::Hover) == StyleChangeEffect::None,
           "so a hover costs nothing");
  });

  AddTest(tests, "StyleInvalidation/APaintOnlyRuleAsksForAPaint", [] {
    const css::StyleInvalidation index = IndexOf("a:hover { color: red; background: blue }");
    Expect(index.DependsOn(ElementState::Hover), "the rule is filed under :hover");
    Expect(index.EffectOf(ElementState::Hover) == StyleChangeEffect::Paint,
           "colour and background move no box");
    Expect(index.EffectOf(ElementState::Checked) == StyleChangeEffect::None,
           "and a state nothing mentions is still free");
  });

  AddTest(tests, "StyleInvalidation/OneLayoutPropertyMakesTheWholeStateALayout", [] {
    Expect(IndexOf("a:hover { color: red } a:hover { padding: 4px }")
                   .EffectOf(ElementState::Hover) == StyleChangeEffect::Layout,
           "a second rule that moves a box makes every hover a layout");
    Expect(IndexOf("a:hover { display: none }").EffectOf(ElementState::Hover) ==
               StyleChangeEffect::Layout,
           "display is a layout property");
  });

  AddTest(tests, "StyleInvalidation/AnUnknownPropertyIsAssumedToAffectLayout", [] {
    // The direction the table has to default in: a property nobody classified
    // makes a change slower than it needed to be, where the other default would
    // make it *wrong* -- a box that moved and a screen that did not.
    Expect(css::PropertyAffectsLayout("some-property-nobody-has-heard-of"),
           "unknown must answer layout");
    Expect(!css::PropertyAffectsLayout("outline"),
           "an outline takes no space, which is the whole difference from a border");
    Expect(css::PropertyAffectsLayout("border-width"), "a border does");
    Expect(!css::PropertyAffectsLayout("border-color"), "its colour does not");
  });

  AddTest(tests, "StyleInvalidation/TheStateInsideAFunctionalPseudoClassIsIndexed", [] {
    Expect(IndexOf(":is(a, button):hover { color: red }").DependsOn(ElementState::Hover),
           "a state beside a functional pseudo-class");
    Expect(IndexOf("a:not(:checked) { color: red }").DependsOn(ElementState::Checked),
           "and one nested inside it -- a rule filed under nothing never re-applies");
    Expect(IndexOf("li:hover + li { color: red }").DependsOn(ElementState::Hover),
           "and one in a compound that is not the last, which styles a different element");
    Expect(IndexOf("input:enabled { color: red }").DependsOn(ElementState::Disabled),
           ":enabled depends on the Disabled bit even though it does not name it");
  });

  // --- the check -------------------------------------------------------------

  AddTest(tests, "StyleInvalidation/MouseCrossingAPageWithNoHoverRulesCostsNothing", [] {
    HoverSession session;
    session.Load(
        "<style>p { color: green; padding: 4px }</style>"
        "<p>one</p><p>two</p><p>three</p>");
    // Settled: the load is finished and the page is on screen.
    const RenderCost before = RenderCost::Now();
    const std::uint64_t hit_tests_before =
        ReadPerformanceCounter(PerfCounterId::StyleHoverHitTests);
    for (int y = 10; y < 120; y += 5) {
      session.MoveTo(40.0f, static_cast<float>(y));
    }
    const RenderCost after = RenderCost::Now();
    ExpectEqInt(static_cast<long long>(after.styles - before.styles), 0,
                "a pointer crossing a page with no :hover rules must not restyle it");
    ExpectEqInt(static_cast<long long>(after.layouts - before.layouts), 0,
                "must not relayout it");
    ExpectEqInt(static_cast<long long>(after.paints - before.paints), 0, "and must not repaint it");
    ExpectEqInt(static_cast<long long>(ReadPerformanceCounter(PerfCounterId::StyleHoverHitTests) -
                                      hit_tests_before),
                0, "and must not even hit-test: the index is asked before the box tree is walked");
  });

  AddTest(tests, "StyleInvalidation/AHoverThatOnlyChangesAColourDoesNotRunLayout", [] {
    HoverSession session;
    session.Load(
        "<style>p { color: green } p:hover { color: red }</style>"
        "<p>one</p><p>two</p>");
    const RenderCost before = RenderCost::Now();
    session.MoveTo(40.0f, 10.0f);
    const RenderCost after = RenderCost::Now();
    Expect(after.styles > before.styles, "the cascade is re-resolved");
    ExpectEqInt(static_cast<long long>(after.layouts - before.layouts), 0,
                "but a colour moves no box, so layout must not run");
    Expect(after.paints > before.paints, "and the frame goes out");
    Expect(ReadPerformanceCounter(PerfCounterId::StyleRestylesWithoutLayout) > 0,
           "through the paint-only path");
  });

  AddTest(tests, "StyleInvalidation/AHoverThatMovesABoxDoesRunLayout", [] {
    HoverSession session;
    session.Load(
        "<style>p { padding: 2px } p:hover { padding: 20px }</style>"
        "<p>one</p><p>two</p>");
    const RenderCost before = RenderCost::Now();
    session.MoveTo(40.0f, 10.0f);
    const RenderCost after = RenderCost::Now();
    Expect(after.layouts > before.layouts,
           "padding moves a box, so the layout path is the correct one");
  });

  AddTest(tests, "StyleInvalidation/MovingWithinTheSameElementRestylesOnce", [] {
    HoverSession session;
    session.Load(
        "<style>p { color: green; height: 200px } p:hover { color: red }</style>"
        "<p>one</p>");
    session.MoveTo(40.0f, 60.0f);
    // Already hovered, and the second move lands on the same element: the chain
    // did not change, so nothing is owed. This is the case that separates "ask
    // the index" from "diff the state" -- the index says a hover matters, and
    // it is the state comparison that says this particular move did not.
    const RenderCost before = RenderCost::Now();
    session.MoveTo(60.0f, 100.0f);
    const RenderCost after = RenderCost::Now();
    ExpectEqInt(static_cast<long long>(after.styles - before.styles), 0,
                "a move inside the hovered element changes no state");
    ExpectEqInt(static_cast<long long>(after.paints - before.paints), 0, "and paints nothing");
  });
}

}  // namespace microbrowser::tests

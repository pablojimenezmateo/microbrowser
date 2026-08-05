// The focus model: ADR 0017 §4.
//
// One focused element per document, on `dom::Document` and nowhere else. The
// engine moves it, the binding layer reports it as `document.activeElement`,
// and every key goes to it -- hit testing is consulted only for pointer events.
// That last sentence is the reason focus is worth this much test: it is the
// input router, so a wrong answer here does not produce a wrong pixel, it
// produces a keystroke delivered to the wrong element or to nothing at all.
//
// The rules that are silently wrong until asserted, and each has a case below:
// what is focusable (a `<div>` is not, a `<div tabindex=-1>` is, a disabled
// input never is), what a click focuses (the nearest focusable *ancestor*, so
// clicking the text inside a button focuses the button), what a click on
// nothing does (blurs, which is the only way to leave a field with the mouse),
// the order of the four events, whether a focus ring shows (keyboard yes,
// pointer no), the Tab order with `tabindex` in it, and what happens when the
// focused element is removed from the tree.

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "bindings/DomBindings.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "html/Focus.h"
#include "html/TreeBuilder.h"
#include "js/Interpreter.h"
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

struct Bound {
  std::unique_ptr<dom::Document> document;
  std::unique_ptr<js::Interpreter> interpreter;
  std::unique_ptr<bindings::DomBindings> bindings;
};

Bound Bind(std::string_view html) {
  Bound bound;
  bound.document = html::ParseDocument(html);
  bound.interpreter = std::make_unique<js::Interpreter>();
  bound.bindings = std::make_unique<bindings::DomBindings>(*bound.interpreter, *bound.document,
                                                          "https://example.org/");
  bound.bindings->Install();
  return bound;
}

std::string Run(std::string_view html, std::string_view source) {
  Bound bound = Bind(html);
  const js::Result result = bound.interpreter->Run(source);
  if (result.completion == js::Completion::Throw) {
    return "throw " + js::ToString(result.value);
  }
  return js::ToString(result.value);
}

void ExpectScript(std::string_view html, std::string_view source, std::string_view expected) {
  ExpectEqString(Run(html, source), std::string(expected),
                 std::string("running: ") + std::string(source));
}

const dom::Element* FirstWithId(const dom::Document& document, std::string_view id) {
  const dom::Element* found = nullptr;
  document.ForEachDescendant([&](const dom::Node& node) {
    if (found != nullptr || !node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    const std::string* value = element.GetAttribute("id");
    if (value != nullptr && *value == id) {
      found = &element;
    }
  });
  return found;
}

// The id of whatever the page has focused, or "-" for nothing. Every engine
// case below states its expectation this way so the failure message names an
// element rather than an address.
std::string FocusedId(const engine::Page& page) {
  const dom::Element* focused = page.FocusedElement();
  if (focused == nullptr) {
    return "-";
  }
  const std::string* id = focused->GetAttribute("id");
  return id == nullptr ? focused->TagName() : *id;
}

bindings::KeyInput NamedKey(std::string name, bool shift = false) {
  bindings::KeyInput key;
  key.key = std::move(name);
  key.shift = shift;
  return key;
}

}  // namespace

void RegisterFocusTests(std::vector<TestCase>& tests) {
  // --- What can hold focus ---------------------------------------------------

  AddTest(tests, "Focus/WhatIsFocusable", [] {
    const std::unique_ptr<dom::Document> document = html::ParseDocument(
        "<body><div id=plain>x</div><div id=indexed tabindex=0></div>"
        "<div id=negative tabindex=-1></div><div id=editable contenteditable></div>"
        "<div id=noteditable contenteditable=false></div>"
        "<a id=anchor href=/x>link</a><a id=named name=top>anchor</a>"
        "<input id=field><input id=off disabled><input id=covert type=hidden>"
        "<button id=press>go</button><select id=pick></select>"
        "<textarea id=note></textarea><span id=away tabindex=2>x</span>"
        "</body>");
    const auto focusable = [&](std::string_view id) {
      const dom::Element* element = FirstWithId(*document, id);
      Expect(element != nullptr, std::string("the fixture has #") + std::string(id));
      return html::IsFocusable(*element);
    };

    Expect(!focusable("plain"), "a bare div is not focusable, and a page relies on that");
    Expect(focusable("indexed"), "tabindex=0 makes anything focusable");
    Expect(focusable("negative"),
           "and so does tabindex=-1 -- negative means not *tab-reachable*, which is a "
           "different question");
    Expect(focusable("editable"), "an editing host is focusable");
    Expect(!focusable("noteditable"), "contenteditable=false is not");
    Expect(focusable("anchor"), "a link is");
    Expect(!focusable("named"), "an <a> with no href is not a link at all");
    Expect(focusable("field"), "an input is");
    Expect(!focusable("off"), "a disabled one is not");
    Expect(!focusable("covert"), "and neither is a hidden one");
    Expect(focusable("press") && focusable("pick") && focusable("note"),
           "button, select and textarea are");

    const dom::Element* negative = FirstWithId(*document, "negative");
    Expect(!html::IsTabReachable(*negative), "tabindex=-1 is skipped by Tab");
    const dom::Element* away = FirstWithId(*document, "away");
    Expect(html::IsTabReachable(*away) && html::TabIndex(*away).value_or(0) == 2,
           "and a positive tabindex is both reachable and ordered");
  });

  AddTest(tests, "Focus/ATabIndexSaturatesRatherThanWrapping", [] {
    // Attacker-controlled text. An overflow that turned a huge positive index
    // negative would take the element *out* of the Tab order, which is a page
    // deciding what the keyboard can reach.
    const std::unique_ptr<dom::Document> document = html::ParseDocument(
        "<body><i id=huge tabindex=99999999999999></i><i id=spaced tabindex=' 3 '></i>"
        "<i id=junk tabindex=abc></i><i id=empty tabindex></i></body>");
    const dom::Element* huge = FirstWithId(*document, "huge");
    Expect(html::TabIndex(*huge).has_value() && *html::TabIndex(*huge) > 0,
           "a value past INT_MAX saturates positive rather than wrapping");
    const dom::Element* spaced = FirstWithId(*document, "spaced");
    Expect(html::TabIndex(*spaced).value_or(0) == 3, "surrounding whitespace is stripped");
    Expect(!html::TabIndex(*FirstWithId(*document, "junk")).has_value(),
           "and text that is not an integer is no tabindex at all");
    Expect(!html::TabIndex(*FirstWithId(*document, "empty")).has_value(), "nor is an empty one");
  });

  // --- The document owns it --------------------------------------------------

  AddTest(tests, "Focus/RemovingTheFocusedElementClearsIt", [] {
    // The focus is a raw Element*. A page that removes the element holding it
    // would otherwise leave the next key routed at a node the tree no longer
    // contains -- and a use-after-free the moment removed nodes stop being kept
    // alive.
    std::unique_ptr<dom::Document> document =
        html::ParseDocument("<body><div id=box><input id=field></div></body>");
    auto* field = const_cast<dom::Element*>(FirstWithId(*document, "field"));
    auto* box = const_cast<dom::Element*>(FirstWithId(*document, "box"));
    document->SetFocus(field, true);
    Expect(document->Focus().element == field, "the field has focus");

    // The *ancestor* is removed, not the focused element itself. Walking up
    // from the focused element is what catches this; walking the removed
    // subtree would too, at the cost of the whole subtree.
    std::unique_ptr<dom::Node> detached = box->Parent()->Detach(box);
    Expect(document->Focus().element == nullptr,
           "removing a subtree containing the focused element clears the focus");
  });

  // --- The script side -------------------------------------------------------

  AddTest(tests, "Focus/ActiveElementReportsTheDocumentFocus", [] {
    ExpectScript("<body><input id=a></body>", "document.activeElement.tagName", "body");
    ExpectScript("<body><input id=a></body>",
                 "document.getElementById('a').focus();"
                 "document.activeElement.id",
                 "a");
    ExpectScript("<body><input id=a></body>",
                 "const a = document.getElementById('a'); a.focus(); a.blur();"
                 "document.activeElement.tagName",
                 "body");
  });

  AddTest(tests, "Focus/FocusingWhatCannotHoldFocusIsANoOp", [] {
    // Not an error. `focus()` on the wrong node is something every page does
    // and throwing would break more than it reported.
    ExpectScript("<body><div id=d></div><input id=a></body>",
                 "document.getElementById('a').focus();"
                 "document.getElementById('d').focus();"
                 "document.activeElement.id",
                 "a");
    ExpectScript("<body><input id=a><input id=b disabled></body>",
                 "document.getElementById('a').focus();"
                 "document.getElementById('b').focus();"
                 "document.activeElement.id",
                 "a");
  });

  AddTest(tests, "Focus/BlurOnlyWorksOnTheElementThatHasIt", [] {
    // Otherwise a page could take focus off anything by naming anything.
    ExpectScript("<body><input id=a><input id=b></body>",
                 "document.getElementById('a').focus();"
                 "document.getElementById('b').blur();"
                 "document.activeElement.id",
                 "a");
  });

  AddTest(tests, "Focus/TheFourEventsFireInOrderAndOnlyTwoOfThemBubble", [] {
    ExpectScript(
        "<body><div id=box><input id=a><input id=b></div></body>",
        "const seen = [];"
        "const box = document.getElementById('box');"
        "const a = document.getElementById('a');"
        "const b = document.getElementById('b');"
        // On the container: only the bubbling pair can reach it, which is the
        // entire reason there are four events rather than two.
        "box.addEventListener('focus', () => seen.push('box:focus'));"
        "box.addEventListener('blur', () => seen.push('box:blur'));"
        "box.addEventListener('focusin', e => seen.push('box:focusin<-' +"
        "  (e.relatedTarget ? e.relatedTarget.id : 'none')));"
        "box.addEventListener('focusout', () => seen.push('box:focusout'));"
        "a.addEventListener('blur', () => seen.push('a:blur'));"
        "b.addEventListener('focus', () => seen.push('b:focus'));"
        "a.focus(); b.focus();"
        "seen.join(',')",
        "box:focusin<-none,a:blur,box:focusout,b:focus,box:focusin<-a");
  });

  AddTest(tests, "Focus/AHandlerSeesTheNewFocusRatherThanTheOld", [] {
    // The state moves before any handler runs, so a `blur` handler that reads
    // `document.activeElement` is told where focus went.
    ExpectScript("<body><input id=a><input id=b></body>",
                 "let seen = '';"
                 "const a = document.getElementById('a');"
                 "a.addEventListener('blur', () => { seen = document.activeElement.id; });"
                 "a.focus(); document.getElementById('b').focus();"
                 "seen",
                 "b");
  });

  AddTest(tests, "Focus/AFocusEventIsTrustedOnlyWhenTheBrowserMovedFocus", [] {
    // ADR 0017 §3. A page dispatching its own `focus` must not be able to make
    // it look like the user's doing.
    ExpectScript("<body><input id=a></body>",
                 "const a = document.getElementById('a');"
                 "let flags = [];"
                 "a.addEventListener('focus', e => flags.push(e.isTrusted));"
                 "a.focus();"
                 "a.dispatchEvent(new Event('focus'));"
                 "flags.join(',')",
                 "true,false");
  });

  // --- The engine side -------------------------------------------------------

  AddTest(tests, "Focus/AClickFocusesTheNearestFocusableAncestor", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>body{margin:0}button{display:block;width:80px;height:20px;"
        "margin:0;padding:0;border:0}div{height:20px}</style>"
        "<body><button id=press>go</button><div id=plain>text</div></body>",
        "https://example.org/");
    page.Layout(400.0f);

    // Inside the button's text, not on its edge: the hit test lands on a text
    // box with no element of its own, so this only works if the walk goes up.
    Expect(page.FocusFromClickAt(gfx::FloatPoint{10.0f, 10.0f}), "clicking the button focused it");
    ExpectEqString(FocusedId(page), "press", "the button, not the text inside it");

    // And a click on something that cannot hold focus blurs, which is the only
    // way to leave a field with the mouse.
    Expect(page.FocusFromClickAt(gfx::FloatPoint{10.0f, 30.0f}), "clicking the div moved focus");
    ExpectEqString(FocusedId(page), "-", "onto nothing at all");
  });

  AddTest(tests, "Focus/TabWalksTheDocumentAndHonoursTabIndex", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body><input id=first><div id=skipped></div><a id=link href=/x>l</a>"
        "<input id=last><input id=hidden tabindex=-1></body>",
        "https://example.org/");
    page.Layout(400.0f);

    Expect(page.MoveFocusByTab(false), "Tab with nothing focused goes to the first");
    ExpectEqString(FocusedId(page), "first", "which is the first tab-reachable element");
    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "link", "past the div, which is not focusable");
    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "last", "and on to the next input");
    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "first",
                   "past the tabindex=-1 one, and wrapping rather than leaving the document");
    page.MoveFocusByTab(true);
    ExpectEqString(FocusedId(page), "last", "and Shift+Tab goes the other way");
  });

  AddTest(tests, "Focus/APositiveTabIndexComesFirst", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body><input id=third><input id=second tabindex=2><input id=first tabindex=1>"
        "<input id=fourth></body>",
        "https://example.org/");
    page.Layout(400.0f);

    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "first", "tabindex=1 before tabindex=2");
    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "second", "then tabindex=2");
    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "third",
                   "and only then the elements with no tabindex, in document order");
    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "fourth", "which is a stable order and not a sorted one");
  });

  AddTest(tests, "Focus/KeyboardFocusIsVisibleAndPointerFocusIsNot", [] {
    // The `:focus-visible` heuristic. A focus ring on every click is why
    // authors write `outline: none`, which is worse for the user than either
    // behaviour. ADR 0017 §4.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>body{margin:0}button{display:block;width:80px;height:20px;"
        "margin:0;padding:0;border:0}</style>"
        "<body><button id=a>a</button><button id=b>b</button></body>",
        "https://example.org/");
    page.Layout(400.0f);

    page.MoveFocusByTab(false);
    ExpectEqString(FocusedId(page), "a", "Tab focused the first button");
    Expect(page.FocusIsVisible(), "Tab shows a focus ring");
    page.FocusFromClickAt(gfx::FloatPoint{10.0f, 30.0f});
    ExpectEqString(FocusedId(page), "b", "the click focused the second button");
    Expect(!page.FocusIsVisible(), "and a click does not");
  });

  AddTest(tests, "Focus/AKeyGoesToTheFocusedElementAndNotToWhatWasClicked", [] {
    // The split ADR 0017 §4 names: focus is the input router, and hit testing
    // is consulted only for pointer events. Before the focus model, script had
    // no way to move focus at all, so a page that called `input.focus()` and
    // expected to be typed into was broken by construction.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body><input id=a><input id=b>"
        "<script>document.getElementById('b').focus();"
        "document.getElementById('b').addEventListener('keydown',"
        "  e => console.log('b heard ' + e.key));</script></body>",
        "https://example.org/");
    page.RunScripts(0);
    page.Layout(400.0f);
    ExpectEqString(FocusedId(page), "b", "the script moved focus");

    const engine::DispatchOutcome outcome = page.DispatchKeyToFocus(NamedKey("Escape"));
    Expect(!outcome.prevented, "nothing cancelled it");
    Expect(!page.ConsoleOutput().empty(), "the handler on the focused element ran");
    ExpectEqString(page.ConsoleOutput().front(), "b heard Escape",
                   "and it was the key that was pressed, not the text it produced");

    // And typing goes there too, which is the same fact stated as a value.
    Expect(page.InsertTextIntoFocusedTextControl("hi"), "typing reaches the focused control");
    const dom::Element* b = page.FocusedElement();
    Expect(b != nullptr && b->GetAttribute("value") != nullptr &&
               *b->GetAttribute("value") == "hi",
           "and lands in the field the script focused");
  });

  AddTest(tests, "Focus/EscapeReachesThePageAndClosesItsMenu", [] {
    // The session check's first clause, as a page rather than as a site. It is
    // written here and not against a real one because no page this browser can
    // currently *run* has a menu that Escape closes: old.reddit's two Escape
    // handlers are behind a login and its scripts all die on the masked `r is
    // not defined` anyway, and a search-suggestion menu is filled by `fetch`,
    // which is session 13. Recorded in the ledger rather than left implied.
    //
    // What it asserts is the whole path and not a stub of it: a real keydown
    // through the real dispatch algorithm, at the element focus routes to, with
    // the handler on an ancestor so the key has to bubble to reach it.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body><div id=menu>"
        "<button id=first>one</button><button id=second>two</button></div>"
        "<a id=after href='/next'>after the menu</a>"
        "<script>"
        "document.getElementById('menu').addEventListener('keydown', function (e) {"
        "  if (e.key === 'Escape') { this.setAttribute('hidden', ''); }"
        "});"
        "document.getElementById('second').focus();"
        "</script></body>",
        "https://example.org/");
    page.RunScripts(0);
    page.Layout(400.0f);
    ExpectEqString(FocusedId(page), "second", "the menu opened with an item focused");

    const engine::DispatchOutcome outcome = page.DispatchKeyToFocus(NamedKey("Escape"));
    Expect(outcome.ran, "the page had a handler for it");
    Expect(!outcome.prevented, "which did not cancel the default action");
    const dom::Node* menu = page.FocusedElement() != nullptr ? page.FocusedElement()->Parent()
                                                             : nullptr;
    Expect(menu != nullptr && menu->IsElement() &&
               static_cast<const dom::Element*>(menu)->HasAttribute("hidden"),
           "the handler closed the menu -- Escape bubbled from the focused item to it");

    // And Tab does not walk back into it. `hidden` is on the *container*, so
    // this only holds because focusability asks the ancestors as well as the
    // element -- which is what keeps a keystroke from reaching an item the
    // user cannot see.
    Expect(page.MoveFocusByTab(false), "Tab still has somewhere to go");
    ExpectEqString(FocusedId(page), "after",
                   "and it is past the menu, not back inside it -- the closed menu's items are "
                   "out of the Tab order");
  });

  AddTest(tests, "Focus/APageWithNoScriptStillMovesFocus", [] {
    // No interpreter means no handlers and no events, but focus still moves --
    // or a page with no script would have no keyboard at all.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body><input id=a></body>", "https://example.org/");
    page.Layout(400.0f);
    Expect(page.MoveFocusByTab(false), "Tab moved focus with no script on the page");
    ExpectEqString(FocusedId(page), "a", "to the only field there is");
  });
}

}  // namespace microbrowser::tests

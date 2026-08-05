#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "app/KeyRouting.h"
#include "ui/BrowserChrome.h"

namespace microbrowser::tests {

using app::KeyDestination;
using app::RouteKey;
using platform::Key;
using platform::KeyEvent;
using platform::Modifiers;
using platform::PointerEvent;
using ui::BrowserChrome;
using ui::TextField;
using ui::Toolbar;

namespace {

KeyEvent Typed(char32_t codepoint) {
  KeyEvent event;
  event.codepoint = codepoint;
  event.pressed = true;
  return event;
}

KeyEvent Named(Key key, Modifiers modifiers = {}) {
  KeyEvent event;
  event.key = key;
  event.modifiers = modifiers;
  event.pressed = true;
  return event;
}

KeyEvent Chord(char32_t codepoint, Modifiers modifiers) {
  KeyEvent event;
  event.codepoint = codepoint;
  event.modifiers = modifiers;
  event.pressed = true;
  return event;
}

Modifiers Control() {
  Modifiers modifiers;
  modifiers.control = true;
  return modifiers;
}

Modifiers Shift() {
  Modifiers modifiers;
  modifiers.shift = true;
  return modifiers;
}

Modifiers ControlShift() {
  Modifiers modifiers;
  modifiers.control = true;
  modifiers.shift = true;
  return modifiers;
}

void TypeString(TextField& field, std::string_view text) {
  for (const char c : text) {
    field.HandleKey(Typed(static_cast<char32_t>(static_cast<unsigned char>(c))));
  }
}

PointerEvent ClickAt(int x, int y) {
  PointerEvent event;
  event.kind = PointerEvent::Kind::Down;
  event.position = gfx::IntPoint{x, y};
  return event;
}

BrowserChrome MakeChrome(int width = 800) {
  BrowserChrome chrome;
  chrome.SetViewportWidth(width);
  return chrome;
}

}  // namespace

void RegisterBrowserChromeTests(std::vector<TestCase>& tests) {
  // --- Editing --------------------------------------------------------------

  AddTest(tests, "TextField/TypingInsertsAtTheCaret", [] {
    TextField field;
    TypeString(field, "abc");
    ExpectEqString(field.Text(), "abc", "characters land in order");
    field.MoveLeft(false);
    TypeString(field, "X");
    ExpectEqString(field.Text(), "abXc", "and at the caret rather than at the end");
  });

  AddTest(tests, "TextField/TypingReplacesTheSelection", [] {
    TextField field;
    field.SetText("hello");
    field.SelectAll();
    TypeString(field, "z");
    ExpectEqString(field.Text(), "z",
                   "select-all then type is how every URL gets replaced, and it must not "
                   "append");
    Expect(!field.HasSelection(), "and the selection is gone afterwards");
  });

  AddTest(tests, "TextField/BackspaceWithASelectionDeletesOnlyTheSelection", [] {
    // The classic double-delete: deleting the selection *and* the character
    // before it.
    TextField field;
    field.SetText("abcdef");
    field.MoveToStart(false);
    field.MoveRight(true);
    field.MoveRight(true);
    field.MoveRight(true);
    field.DeleteBackward();
    ExpectEqString(field.Text(), "def", "three characters gone, not four");
  });

  AddTest(tests, "TextField/DeleteAtTheEndsDoesNothing", [] {
    TextField field;
    field.SetText("ab");
    field.MoveToStart(false);
    field.DeleteBackward();
    ExpectEqString(field.Text(), "ab", "backspace at the start is a no-op, not an underflow");
    field.MoveToEnd(false);
    field.DeleteForward();
    ExpectEqString(field.Text(), "ab", "and delete at the end likewise");
  });

  AddTest(tests, "TextField/TheCaretStepsOverWholeCodepoints", [] {
    // Stepping by one byte splits a multi-byte character and leaves text that
    // is no longer UTF-8.
    TextField field;
    field.SetText("a\xC3\xA9z");  // a, e-acute, z
    field.MoveToStart(false);
    field.MoveRight(false);
    field.MoveRight(false);
    ExpectEqInt(static_cast<long long>(field.Caret()), 3,
                "one byte for 'a' and two for the accented character");
    field.DeleteBackward();
    ExpectEqString(field.Text(), "az", "and backspace removes the whole character");
  });

  AddTest(tests, "TextField/ArrowWithASelectionCollapsesToTheNearEdge", [] {
    TextField field;
    field.SetText("abcdef");
    field.SelectAll();
    field.MoveLeft(false);
    ExpectEqInt(static_cast<long long>(field.Caret()), 0, "left goes to the start of it");
    field.SelectAll();
    field.MoveRight(false);
    ExpectEqInt(static_cast<long long>(field.Caret()), 6, "and right to the end");
  });

  AddTest(tests, "TextField/ShiftArrowExtendsAndRetractsTheSelection", [] {
    TextField field;
    field.SetText("abcdef");
    field.MoveToStart(false);
    field.HandleKey(Named(Key::Right, Shift()));
    field.HandleKey(Named(Key::Right, Shift()));
    ExpectEqString(std::string(field.SelectedText()), "ab", "extended forward");
    field.HandleKey(Named(Key::Left, Shift()));
    ExpectEqString(std::string(field.SelectedText()), "a",
                   "and shrunk again -- a selection has a direction, which is why the anchor "
                   "is kept rather than a length");
  });

  AddTest(tests, "TextField/AControlChordIsNotTyped", [] {
    // Otherwise ctrl+R types an 'r' into the URL on its way to reloading.
    TextField field;
    field.SetText("");
    Expect(!field.HandleKey(Chord(U'r', Control())), "the chord is not consumed as text");
    ExpectEqString(field.Text(), "", "and nothing was typed");
  });

  AddTest(tests, "TextField/ControlAAndControlUAreHandled", [] {
    TextField field;
    field.SetText("https://example.org/");
    Expect(field.HandleKey(Chord(U'a', Control())), "ctrl+A is consumed");
    Expect(field.HasSelection() && field.SelectedText() == field.Text(), "and selects all");
    Expect(field.HandleKey(Chord(U'u', Control())), "ctrl+U is consumed");
    ExpectEqString(field.Text(), "", "and clears the line, as in a terminal");
  });

  // --- History ----------------------------------------------------------------
  //
  // The list itself moved to `src/engine` (ADR 0026 §1): a `pushState` entry is a
  // URL *plus a state object owned by a document*, and the chrome cannot see a
  // document. `engine::SessionHistory` is tested in tests/HistoryTests.cpp. What
  // is left here is the whole of what the chrome does with history now, which is
  // two bools and a delta.

  AddTest(tests, "Chrome/TheHistoryButtonsAreWhatTheEngineSaidTheyAre", [] {
    BrowserChrome chrome = MakeChrome();
    Expect(!chrome.GetToolbar().CanGoBack(), "nothing to go back to yet");
    chrome.OnHistoryState(true, false);
    Expect(chrome.GetToolbar().CanGoBack() && !chrome.GetToolbar().CanGoForward(),
           "and the engine is the only thing that decides");
    chrome.OnHistoryState(false, true);
    Expect(!chrome.GetToolbar().CanGoBack() && chrome.GetToolbar().CanGoForward(),
           "in both directions");
  });

  // --- What the omnibox does with what was typed ----------------------------

  AddTest(tests, "Omnibox/AnExplicitSchemeIsUsedAsIs", [] {
    ExpectEqString(ui::ResolveOmniboxInput("https://example.org/a"), "https://example.org/a",
                   "a URL is a URL");
    ExpectEqString(ui::ResolveOmniboxInput("about:blank"), "about:blank", "including about:");
    ExpectEqString(ui::ResolveOmniboxInput("data:text/html,hi"), "data:text/html,hi",
                   "and data:");
  });

  AddTest(tests, "Omnibox/AHostGetsHttpsRatherThanHttp", [] {
    ExpectEqString(ui::ResolveOmniboxInput("example.org"), "https://example.org",
                   "guessing the insecure scheme is a downgrade nobody asked for");
    ExpectEqString(ui::ResolveOmniboxInput("localhost:8080"), "https://localhost:8080",
                   "a port makes it host-shaped even with no dot");
    ExpectEqString(ui::ResolveOmniboxInput("  example.org  "), "https://example.org",
                   "and surrounding space is not part of a host");
  });

  AddTest(tests, "Omnibox/WordsBecomeASearch", [] {
    const std::string resolved = ui::ResolveOmniboxInput("how tall is everest");
    Expect(resolved.find("https://") == 0, "a search goes over TLS");
    Expect(resolved.find("q=how+tall+is+everest") != std::string::npos,
           "with the words as the query");
  });

  AddTest(tests, "Omnibox/SomethingThatCouldBeAHostIsTreatedAsOne", [] {
    // The direction this errs matters: treating a host as a search sends what
    // someone typed to a search engine, which leaks it. Treating a search as a
    // host merely fails to load.
    Expect(ui::ResolveOmniboxInput("12:30").find("https://12:30") == 0,
           "a colon makes it host-shaped");
    Expect(ui::ResolveOmniboxInput("my.local.thing").find("https://my.local.thing") == 0,
           "and so do dots with no space");
  });

  AddTest(tests, "Omnibox/EmptyInputNavigatesNowhere", [] {
    ExpectEqString(ui::ResolveOmniboxInput("   "), "", "whitespace is not a destination");
  });

  // --- The chrome as a whole ------------------------------------------------

  AddTest(tests, "Chrome/ControlLFocusesTheOmniboxAndSelectsIt", [] {
    BrowserChrome chrome = MakeChrome();
    chrome.OnNavigationCommitted("https://example.org/");
    const BrowserChrome::Response response = chrome.HandleKey(Chord(U'l', Control()));

    Expect(response.handled && response.needs_repaint, "the chord was consumed");
    Expect(chrome.GetToolbar().IsOmniboxFocused(), "the omnibox has focus");
    Expect(chrome.GetToolbar().Omnibox().HasSelection(),
           "and its contents are selected, so typing replaces the URL");
  });

  AddTest(tests, "Chrome/TypingThenEnterNavigatesToWhatWasTyped", [] {
    BrowserChrome chrome = MakeChrome();
    chrome.HandleKey(Chord(U'l', Control()));
    TypeString(chrome.GetToolbar().Omnibox(), "example.org");
    const BrowserChrome::Response response = chrome.HandleKey(Named(Key::Enter));

    Expect(response.intent.has_value(), "Enter produced an intent");
    Expect(response.intent->kind == BrowserChrome::Intent::Kind::Navigate, "to navigate");
    ExpectEqString(response.intent->url, "https://example.org", "to the resolved URL");
    Expect(!chrome.GetToolbar().IsOmniboxFocused(), "and focus left the field");
  });

  AddTest(tests, "Chrome/EscapeRestoresTheLoadedUrl", [] {
    BrowserChrome chrome = MakeChrome();
    chrome.OnNavigationCommitted("https://example.org/page");
    chrome.HandleKey(Chord(U'l', Control()));
    TypeString(chrome.GetToolbar().Omnibox(), "something else");

    const BrowserChrome::Response response = chrome.HandleKey(Named(Key::Escape));
    Expect(response.handled, "Escape was consumed");
    ExpectEqString(chrome.GetToolbar().Omnibox().Text(), "https://example.org/page",
                   "the omnibox shows what is loaded, not the last thing typed");
    Expect(!chrome.GetToolbar().IsOmniboxFocused(), "and focus left the field");
  });

  AddTest(tests, "Chrome/ACommittedRedirectIsWhatTheOmniboxShows", [] {
    // An omnibox that kept showing the typed URL would be lying about the
    // origin the page is running as.
    BrowserChrome chrome = MakeChrome();
    chrome.OnNavigationCommitted("https://example.org/redirected");
    ExpectEqString(chrome.GetToolbar().Omnibox().Text(), "https://example.org/redirected",
                   "where you ended up, not where you aimed");
  });

  // CHANGED IN THIS SESSION, deliberately. This test used to assert that the
  // chrome turned Down into a ScrollPage intent. It no longer does, and the
  // reason is ADR 0017 §2: scrolling is a *default action* of a keydown, so it
  // has to happen after the page's handlers have seen the key and only if none
  // of them cancelled it. The chrome taking the key meant a page never saw an
  // ArrowDown at all and `preventDefault` on one meant nothing. Scrolling now
  // happens in Engine::ScrollByKey, which is where the other keyboard default
  // actions are, and `Intent::Kind::ScrollPage` is gone with it.
  AddTest(tests, "Chrome/ArrowKeysBelongToThePageAndNotToTheChrome", [] {
    BrowserChrome chrome = MakeChrome();
    const BrowserChrome::Response down = chrome.HandleKey(Named(Key::Down));
    Expect(!down.handled && !down.intent.has_value(),
           "the chrome does not take an arrow key -- the page's handlers get it first");
    Expect(RouteKey(Named(Key::Down), chrome.GetToolbar().IsOmniboxFocused()) ==
               app::KeyDestination::Page,
           "and the routing rule sends it there");

    chrome.HandleKey(Chord(U'l', Control()));
    Expect(RouteKey(Named(Key::Down), chrome.GetToolbar().IsOmniboxFocused()) ==
               app::KeyDestination::Chrome,
           "but not while the omnibox has focus -- arrows belong to the field then");
  });

  // The chrome-or-page decision, which ADR 0017 §4 puts in `src/app` and calls
  // a security boundary. Two things it has to make impossible: a page learning
  // what is typed into the address bar, and a page typing into it.
  AddTest(tests, "Chrome/NothingTypedIntoTheOmniboxReachesThePage", [] {
    BrowserChrome chrome = MakeChrome();
    chrome.HandleKey(Chord(U'l', Control()));
    Expect(chrome.GetToolbar().IsOmniboxFocused(), "ctrl+L focused the omnibox");

    // Every key, not only the ones the chrome uses. "Whatever the chrome did
    // not handle" was the old rule and it is not a filter -- it is a channel:
    // a page listening for keydown would have learned the timing and the
    // identity of most of a typed URL.
    const KeyEvent probes[] = {Typed(U'a'),        Typed(U'.'),          Named(Key::Tab),
                               Named(Key::Up),     Named(Key::PageDown), Named(Key::Home),
                               Named(Key::Delete), Chord(U'a', Control())};
    for (const KeyEvent& probe : probes) {
      Expect(RouteKey(probe, chrome.GetToolbar().IsOmniboxFocused()) ==
                 app::KeyDestination::Chrome,
             "a key aimed at the omnibox never becomes a page message");
    }
  });

  AddTest(tests, "Chrome/TheWayOutOfAPageIsNotThePageToTake", [] {
    // ctrl+L and ctrl+R are reserved whatever has focus. A page that could
    // swallow either could stop the user leaving it.
    BrowserChrome chrome = MakeChrome();
    Expect(RouteKey(Chord(U'l', Control()), false) == app::KeyDestination::Chrome,
           "ctrl+L is the browser's");
    Expect(RouteKey(Chord(U'r', Control()), false) == app::KeyDestination::Chrome,
           "ctrl+R is the browser's");
    Expect(RouteKey(Chord(U'R', ControlShift()), false) == app::KeyDestination::Chrome,
           "and so is ctrl+shift+R");
    // And the list is deliberately short: everything else is the page's, which
    // is what makes a keyboard-driven page work at all.
    Expect(RouteKey(Named(Key::Escape), false) == app::KeyDestination::Page,
           "Escape is the page's");
    Expect(RouteKey(Chord(U'a', Control()), false) == app::KeyDestination::Page,
           "and so is ctrl+A, which a page may bind");
    Expect(RouteKey(Typed(U'j'), false) == app::KeyDestination::Page,
           "and so is an ordinary character");
  });

  AddTest(tests, "Chrome/ControlShiftRRequestsACacheBypassingReload", [] {
    BrowserChrome chrome = MakeChrome();
    const BrowserChrome::Response ordinary = chrome.HandleKey(Chord(U'r', Control()));
    Expect(ordinary.intent.has_value() &&
               ordinary.intent->kind == BrowserChrome::Intent::Kind::Reload,
           "ctrl+R reloads");
    Expect(!ordinary.intent->bypass_cache, "without bypassing a fresh cache entry");

    const BrowserChrome::Response bypass = chrome.HandleKey(Chord(U'R', ControlShift()));
    Expect(bypass.intent.has_value() && bypass.intent->kind == BrowserChrome::Intent::Kind::Reload,
           "ctrl+shift+R also reloads");
    Expect(bypass.intent->bypass_cache, "and asks the engine to reach the network");
  });

  AddTest(tests, "Chrome/BackAndForwardButtonsNavigateAndDoNotStrandTheHistory", [] {
    BrowserChrome chrome = MakeChrome();
    chrome.OnNavigationCommitted("https://a.test/");
    chrome.OnNavigationCommitted("https://b.test/");
    chrome.OnHistoryState(true, false);

    const Toolbar& toolbar = chrome.GetToolbar();
    Expect(toolbar.CanGoBack(), "there is somewhere to go back to");

    // Clicking back asks for a *traversal*, not a navigation to a URL. The chrome
    // does not know where back is any more, and that is what lets the engine
    // answer a `pushState` entry with a paint instead of a load.
    const gfx::IntRect back_button{4, 4, 28, 28};
    const BrowserChrome::Response response =
        chrome.HandlePointer(ClickAt(back_button.x + 5, back_button.y + 5));
    Expect(response.intent.has_value(), "the click asks for something");
    Expect(response.intent->kind == BrowserChrome::Intent::Kind::TraverseHistory,
           "and it is a traversal rather than a navigation");
    ExpectEqInt(response.intent->delta, -1, "one entry back");
    Expect(response.intent->url.empty(), "with no URL, because the chrome has none");
  });

  AddTest(tests, "Chrome/ClickingTheOmniboxFocusesItAndClickingThePageLeaves", [] {
    BrowserChrome chrome = MakeChrome();
    const BrowserChrome::Response focus = chrome.HandlePointer(ClickAt(400, 18));
    Expect(focus.handled && chrome.GetToolbar().IsOmniboxFocused(), "clicking the field focuses");

    const BrowserChrome::Response leave = chrome.HandlePointer(ClickAt(400, 300));
    Expect(!leave.handled, "a click in the page is the page's");
    Expect(!chrome.GetToolbar().IsOmniboxFocused(), "and takes focus off the omnibox");
  });

  AddTest(tests, "Chrome/ThePageNeverGetsTheToolbarsPixels", [] {
    const BrowserChrome chrome = MakeChrome();
    const gfx::IntRect page = chrome.PageBounds(gfx::IntSize{800, 600});
    ExpectEqInt(page.y, Toolbar::kHeight, "the page starts below the chrome");
    ExpectEqInt(page.height, 600 - Toolbar::kHeight, "and is that much shorter");
    Expect(page.y > 0, "which is why a page can never paint over the chrome: it is not given "
                       "those pixels");
  });

  AddTest(tests, "Chrome/AWindowShorterThanTheToolbarStillHasAValidPageRect", [] {
    const BrowserChrome chrome = MakeChrome();
    const gfx::IntRect page = chrome.PageBounds(gfx::IntSize{800, 10});
    Expect(page.height >= 0, "a negative height would be a canvas allocation of a negative size");
  });

  AddTest(tests, "Chrome/ThePaintedChromeIsBoundedByTheToolbar", [] {
    BrowserChrome chrome = MakeChrome(800);
    chrome.OnNavigationCommitted("https://example.org/");
    chrome.GetToolbar().SetOmniboxFocused(true);

    gfx::DisplayList list;
    chrome.GetToolbar().Paint(list, Toolbar::OmniboxMetrics{40.0f, 0.0f, 0.0f});
    Expect(!list.IsEmpty(), "something was drawn");
    const gfx::IntRect bounds = list.Bounds();
    Expect(bounds.Bottom() <= Toolbar::kHeight,
           "the chrome cannot draw below itself -- it is damage-tracked by the same diff as "
           "the page and must not claim the page's rows");
    Expect(bounds.x >= 0 && bounds.Right() <= 800, "nor outside the window");
  });
}

}  // namespace microbrowser::tests

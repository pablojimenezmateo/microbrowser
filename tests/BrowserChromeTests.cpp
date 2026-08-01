#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "ui/BrowserChrome.h"

namespace microbrowser::tests {

using platform::Key;
using platform::KeyEvent;
using platform::Modifiers;
using platform::PointerEvent;
using ui::BrowserChrome;
using ui::NavigationHistory;
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

  // --- History --------------------------------------------------------------

  AddTest(tests, "History/BackAndForwardWalkTheList", [] {
    NavigationHistory history;
    Expect(!history.CanGoBack() && !history.CanGoForward(), "an empty history goes nowhere");

    history.Push("a", "A");
    Expect(!history.CanGoBack(), "one entry is not something to go back from");
    history.Push("b", "B");
    history.Push("c", "C");

    Expect(history.CanGoBack() && !history.CanGoForward(), "at the end of the list");
    ExpectEqString(history.GoBack()->url, "b", "back one");
    ExpectEqString(history.GoBack()->url, "a", "and one more");
    Expect(!history.CanGoBack() && history.CanGoForward(), "now at the start");
    ExpectEqString(history.GoForward()->url, "b", "and forward again");
  });

  AddTest(tests, "History/NavigatingAfterGoingBackTruncatesTheForwardEntries", [] {
    // The branch you left is not reachable, and pretending otherwise is worse
    // than losing it.
    NavigationHistory history;
    history.Push("a", "A");
    history.Push("b", "B");
    history.Push("c", "C");
    history.GoBack();
    history.Push("d", "D");

    Expect(!history.CanGoForward(), "forward stops working after taking a different path");
    ExpectEqInt(static_cast<long long>(history.Entries().size()), 3, "a, b, d");
    ExpectEqString(history.Current()->url, "d", "and d is where we are");
  });

  AddTest(tests, "History/ALateTitleUpdatesTheEntryRatherThanAddingOne", [] {
    // A <title> arriving after the navigation must not become a second entry,
    // or the back button needs pressing twice.
    NavigationHistory history;
    history.Push("https://example.org/", "https://example.org/");
    history.SetCurrentTitle("Example Domain");
    ExpectEqInt(static_cast<long long>(history.Entries().size()), 1, "still one entry");
    ExpectEqString(history.Current()->title, "Example Domain", "with the real title");
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

  AddTest(tests, "Chrome/ArrowKeysScrollThePageWhenTheOmniboxIsNotFocused", [] {
    BrowserChrome chrome = MakeChrome();
    const BrowserChrome::Response down = chrome.HandleKey(Named(Key::Down));
    Expect(down.intent.has_value() &&
               down.intent->kind == BrowserChrome::Intent::Kind::ScrollPage,
           "Down scrolls");
    Expect(down.intent->scroll_delta > 0, "downward");

    chrome.HandleKey(Chord(U'l', Control()));
    const BrowserChrome::Response typing = chrome.HandleKey(Named(Key::Down));
    Expect(!typing.intent.has_value(),
           "but not while the omnibox has focus -- arrows belong to the field then");
  });

  AddTest(tests, "Chrome/BackAndForwardButtonsNavigateAndDoNotStrandTheHistory", [] {
    BrowserChrome chrome = MakeChrome();
    chrome.OnNavigationCommitted("https://a.test/");
    chrome.OnNavigationCommitted("https://b.test/");

    const Toolbar& toolbar = chrome.GetToolbar();
    Expect(toolbar.CanGoBack(), "there is somewhere to go back to");

    // Click the back button, then commit what the engine loads as a result.
    const gfx::IntRect back_button{4, 4, 28, 28};
    const BrowserChrome::Response response =
        chrome.HandlePointer(ClickAt(back_button.x + 5, back_button.y + 5));
    Expect(response.intent.has_value(), "the click navigates");
    ExpectEqString(response.intent->url, "https://a.test/", "to the previous entry");

    chrome.OnNavigationCommitted("https://a.test/");
    Expect(chrome.GetToolbar().CanGoForward(),
           "and forward still works: a history move must not push a new entry and strand "
           "everything in front of it");
    ExpectEqInt(static_cast<long long>(chrome.History().Entries().size()), 2,
                "the history did not grow");
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

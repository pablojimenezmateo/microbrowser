#pragma once

#include <optional>
#include <string>

#include "platform/InputEvent.h"

#include "ui/Toolbar.h"

namespace microbrowser::ui {

// The browser around the page: the toolbar, the history, and the rules that
// connect them to input.
//
// It returns *intents* rather than performing navigation. The chrome has no
// engine, no transport and no window, so every test of "does ctrl+L focus the
// omnibox and Enter navigate to what was typed" runs without any of them —
// and the day the engine moves into another process, none of this changes.
class BrowserChrome {
 public:
  // What the host should do as a result of an event. Absent means nothing.
  struct Intent {
    // Two, and scrolling is deliberately not a third. The arrow and page keys
    // scroll from the engine now, as a keydown's default action -- the chrome
    // doing it meant a page never saw the key. See Engine::ScrollByKey.
    // Three now. A traversal is not a Navigate with a URL, because the chrome
    // no longer knows one: ADR 0026 §1 moved session history into the engine,
    // where the documents are, and the chrome knows only that a button was
    // pressed. That is what makes a same-document traversal possible at all --
    // only the engine can tell a `pushState` entry from a loaded one.
    enum class Kind : std::uint8_t { Navigate, Reload, TraverseHistory };

    Kind kind = Kind::Navigate;
    std::string url;
    bool bypass_cache = false;
    // Negative is back. Meaningful only for TraverseHistory.
    int delta = 0;
  };

  // The event was consumed by the chrome and must not reach the page.
  struct Response {
    bool handled = false;
    bool needs_repaint = false;
    std::optional<Intent> intent;
  };

  void SetViewportWidth(int width);

  // The page's area, below the toolbar. The engine is told about this rather
  // than about the window, which is why a page can never paint over the
  // chrome: it is not given the pixels.
  gfx::IntRect PageBounds(const gfx::IntSize& window) const;

  Response HandleKey(const platform::KeyEvent& event);
  Response HandlePointer(const platform::PointerEvent& event);

  // Called when a navigation actually committed, which is not the same as when
  // one was requested: a redirect changes the URL, and the omnibox must show
  // where the user ended up rather than where they aimed.
  void OnNavigationCommitted(std::string url);
  void OnTitleChanged(std::string title);

  Toolbar& GetToolbar() { return toolbar_; }
  const Toolbar& GetToolbar() const { return toolbar_; }
  // What the two buttons should look like, as the engine last reported it. The
  // chrome holds no entries: ADR 0026 §1's narrowing is that it knows two bools
  // and the URL of the document being displayed.
  void OnHistoryState(bool can_go_back, bool can_go_forward);

  // The window title: the page's, falling back to its URL.
  std::string WindowTitle() const;

 private:
  Response Navigate(std::string url);


  Toolbar toolbar_;
  // The URL of the document on screen and its title, for the omnibox and the
  // window title. The whole of what the chrome remembers about navigation now.
  std::string url_;
  std::string title_;
  // Set while a history move is in flight, so the commit it produces does not
  // push a new entry and strand the forward button.

};

// Turns what a person typed into something to navigate to.
//
// `example.com` is a URL, `how tall is everest` is a search, and `localhost:8080`
// is a URL despite looking like neither. Getting this wrong in the safe
// direction means searching for something that was a URL; getting it wrong in
// the unsafe direction means sending what someone typed to a search engine when
// they meant to visit a host, which leaks it. So the rule errs toward treating
// input as a URL, and a search needs a space or an explicit lack of anything
// host-shaped.
std::string ResolveOmniboxInput(std::string_view typed);

}  // namespace microbrowser::ui

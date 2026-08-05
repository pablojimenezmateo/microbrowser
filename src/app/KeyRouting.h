#pragma once

#include <cstdint>

#include "platform/InputEvent.h"

namespace microbrowser::app {

// Whose key is it: the browser's, or the page's.
//
// **This is a security boundary and not a convenience.** ADR 0017 §4 puts the
// decision here, in `src/app`, and before the key becomes an
// `ipc::KeyInputMessage` -- which is the message that will one day cross into a
// sandboxed renderer. Two things follow from getting it wrong:
//
//   - A page that could see a key aimed at the omnibox learns what the user is
//     typing into the address bar. That is not a leak of one keystroke; a URL
//     is a bookmark, a session token in a query string, an internal hostname.
//   - A page that could *type into* the omnibox controls what the address bar
//     says while showing its own content, which is a phishing primitive rather
//     than a bug. Nothing gives it that route today -- the engine has no
//     message that reaches the chrome -- and this function is the other half:
//     the chrome's keys never leave the chrome.
//
// A pure function of the event and one bit of chrome state, so the whole rule
// is testable without a window, an engine or a transport.
enum class KeyDestination : std::uint8_t { Chrome, Page };

// The reserved shortcuts: the ones the browser keeps whatever the page wants.
//
// Deliberately short. Every entry is a key a page can no longer see, and the
// two here are the ways *out* of a page -- reloading it and typing a new
// address. A page that could swallow either could stop the user leaving it,
// which is the failure this list exists to prevent and the reason it is not
// longer than it has to be.
inline bool IsReservedChromeShortcut(const platform::KeyEvent& event) {
  if (!event.modifiers.control) {
    return false;
  }
  return event.codepoint == U'l' || event.codepoint == U'L' || event.codepoint == U'r' ||
         event.codepoint == U'R';
}

inline KeyDestination RouteKey(const platform::KeyEvent& event, bool omnibox_focused) {
  // While the omnibox has focus the chrome takes *everything*, including keys
  // it does nothing with. That is the whole point: a page must not learn the
  // timing or the identity of a keystroke aimed at the address bar, and
  // "whatever the chrome did not use" is not a filter -- it is a channel.
  if (omnibox_focused) {
    return KeyDestination::Chrome;
  }
  return IsReservedChromeShortcut(event) ? KeyDestination::Chrome : KeyDestination::Page;
}

// A known imprecision, written down rather than papered over: the destination
// is decided per event, so a key pressed while the omnibox had focus and
// released after it lost it delivers its release to the page. Enter, which
// navigates and blurs the omnibox in one step, is the case. Fixing it means
// remembering which keys the chrome captured -- state, in the routing rule --
// and the leak it would close is a keyup with no keydown on a document that is
// being navigated away from. Revisit if a page is ever found that notices.

}  // namespace microbrowser::app

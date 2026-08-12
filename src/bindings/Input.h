#pragma once

#include <cstdint>
#include <string>

#include "dom/Node.h"

// The three value types the engine hands this module, and the one it hands
// back.
//
// Split out of DomBindings.h, which had reached the module's line cap. The
// seam they sit on is the reason they belong together: each is a *fact the
// browser observed* -- a pointer act, a key, a form the page asked to submit --
// crossing this seam by value. None is part of the DomBindings class and none
// can be built by script, which is the whole security content of the input
// path (ADR 0017): the only thing allowed to say a click happened is the thing
// that saw one.

namespace microbrowser::bindings {

// A form submission a script asked for and has not had yet.
//
// Recorded rather than performed, for two reasons and the second is the one
// that matters. This module cannot navigate: it cannot see a URL, a loader or
// a network, which is the module contract working. And a navigation started
// from inside a running script would tear down the interpreter that is running
// it -- ADR 0026 §3 makes document teardown the most safety-critical routine in
// the engine, and "not while script is on the stack" is the first rule of it.
// So the engine takes this after the turn ends.
struct PendingSubmit {
  dom::Element* form = nullptr;
  // The button that submitted, or null. It decides `formaction`, `formmethod`
  // and which submit control appears in the form data set.
  dom::Element* submitter = nullptr;
};

// One pointer act, as the thing that saw it describes it. The coordinates are
// CSS pixels: `client` is measured from the viewport and `page` from the top of
// the document, and they differ by the scroll offset -- which is why both are
// here rather than one plus a subtraction a caller might forget.
struct PointerInput {
  float client_x = 0.0f;
  float client_y = 0.0f;
  float page_x = 0.0f;
  float page_y = 0.0f;
  // The DOM's numbering: 0 is the primary button, and `buttons` is the bitmask
  // of what is still held.
  std::uint8_t button = 0;
  std::uint16_t buttons = 0;
  bool control = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;
};

// One key press or release, as the thing that saw it describes it.
//
// Three strings rather than one, and ADR 0017 §1 is where the reasoning is: a
// game reads `code` because WASD is a shape on the keyboard, a shortcut reads
// `key` because Ctrl+C is a letter, and an editor reads `text` because a dead
// key produces nothing until the next one. This struct is deliberately not
// `ipc::KeyInputMessage`: this module cannot see `ipc`, and the engine
// translating one into the other at the seam is what keeps it that way.
struct KeyInput {
  bool down = true;
  std::string code;
  std::string key;
  std::string text;
  bool control = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;
  bool repeat = false;
};

}  // namespace microbrowser::bindings

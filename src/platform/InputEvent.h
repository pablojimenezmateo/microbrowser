#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "gfx/Geometry.h"

namespace microbrowser::platform {

// Window-system events, with the window system removed.
//
// This type exists so that exactly one file in the project knows what an
// SDL_Event is. Everything downstream — the app loop, and through it the
// engine — sees a closed vocabulary that has no opinion about SDL, X11,
// Wayland, or Win32, and that can be constructed in a test without a display.

struct QuitEvent {};

struct ResizeEvent {
  gfx::IntSize pixel_size;
  // Physical pixels per logical pixel, from the display the window is on.
  float device_scale = 1.0f;
};

// The window contents were destroyed by the compositor and must be redrawn in
// full. Distinct from ResizeEvent: nothing about the geometry changed, so the
// canvas need not be reallocated, but no damage tracking can be trusted.
struct ExposeEvent {};

// Keys that produce no character but mean something.
//
// Deliberately small. Every entry here is one the UI acts on today; a table
// mirroring every key a keyboard has would be a translation layer with no
// consumer, and the missing ones arrive as Key::None with a codepoint, which
// is what typing is.
enum class Key : std::uint8_t {
  None,
  Enter,
  Escape,
  Backspace,
  Delete,
  Tab,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
};

// Modifiers as a set rather than three bools: "control and shift" is one state
// and the combinations are what shortcuts are made of.
struct Modifiers {
  bool control = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;

  bool Any() const { return control || shift || alt || meta; }
  // True when no modifier that would change a key's meaning is held. Shift is
  // excluded on purpose: shift produces capitals, it does not make a shortcut.
  bool PlainTyping() const { return !control && !alt && !meta; }

  friend bool operator==(const Modifiers&, const Modifiers&) = default;
};

struct PointerEvent {
  enum class Kind : std::uint8_t { Move, Down, Up };

  Kind kind = Kind::Move;
  gfx::IntPoint position;
  std::uint8_t button = 0;
  // What was held. A ctrl+click and a click are different acts, and the page is
  // entitled to know which one it got.
  Modifiers modifiers;
};

struct WheelEvent {
  int delta_x = 0;
  int delta_y = 0;
};

struct KeyEvent {
  // A Unicode codepoint when the key produced one, else 0.
  char32_t codepoint = 0;
  // A named key when the key was one, else Key::None. A key can be both --
  // Enter has a codepoint on some platforms -- so a consumer checks the named
  // key first.
  Key key = Key::None;
  // The *physical* key, named the way the DOM names one: "KeyA", "Escape",
  // "ArrowLeft". Empty when this platform could not say which key it was.
  //
  // This is here rather than derived downstream because it is the one part of
  // a key event that cannot be recovered from the character: 'a' is KeyA on a
  // QWERTY keyboard and KeyQ on AZERTY, and only the window system knows which
  // one the user is holding. ADR 0017's "send raw platform key codes and decode
  // them in the engine" is rejected for the mirror-image reason -- the keyboard
  // layout is platform state and belongs on this side of the seam. What a key
  // *means* and what it *inserts* are layout questions this struct already
  // answers with `key` and `codepoint`; what it *is* is this field.
  std::string code;
  Modifiers modifiers;
  bool pressed = false;
  // The key was held down and the platform's auto-repeat produced this. A page
  // that acts on every keydown wants to know; a page that counts keystrokes has
  // to be able to tell them apart.
  bool repeat = false;
};

using InputEvent = std::variant<QuitEvent, ResizeEvent, ExposeEvent, PointerEvent, WheelEvent,
                                KeyEvent>;

}  // namespace microbrowser::platform

#pragma once

#include <cstdint>
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

struct PointerEvent {
  enum class Kind : std::uint8_t { Move, Down, Up };

  Kind kind = Kind::Move;
  gfx::IntPoint position;
  std::uint8_t button = 0;
};

struct WheelEvent {
  int delta_x = 0;
  int delta_y = 0;
};

struct KeyEvent {
  // A Unicode codepoint when the key produced one, else 0. Raw keycodes and a
  // full modifier model arrive with the UI layer in M7; M0 needs only enough to
  // prove the event path is wired.
  char32_t codepoint = 0;
  bool pressed = false;
};

using InputEvent = std::variant<QuitEvent, ResizeEvent, ExposeEvent, PointerEvent, WheelEvent,
                                KeyEvent>;

}  // namespace microbrowser::platform

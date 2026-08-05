#include "platform/SdlWindow.h"

#include <algorithm>

#include "platform/DescriptorWait.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "util/StartupTrace.h"

namespace microbrowser::platform {

namespace {

// SDL keycode to the project's small named-key set. Everything not here is a
// character, and characters arrive through SDL_EVENT_TEXT_INPUT, which is the
// event that understands keyboard layouts and dead keys.
Key NamedKey(SDL_Keycode code) {
  switch (code) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return Key::Enter;
    case SDLK_ESCAPE:
      return Key::Escape;
    case SDLK_BACKSPACE:
      return Key::Backspace;
    case SDLK_DELETE:
      return Key::Delete;
    case SDLK_TAB:
      return Key::Tab;
    case SDLK_LEFT:
      return Key::Left;
    case SDLK_RIGHT:
      return Key::Right;
    case SDLK_UP:
      return Key::Up;
    case SDLK_DOWN:
      return Key::Down;
    case SDLK_HOME:
      return Key::Home;
    case SDLK_END:
      return Key::End;
    case SDLK_PAGEUP:
      return Key::PageUp;
    case SDLK_PAGEDOWN:
      return Key::PageDown;
    default:
      return Key::None;
  }
}

// SDL scancode to the DOM's name for the same physical key.
//
// A scancode is a position on the keyboard, which is exactly what `code` is, so
// this is a rename rather than an interpretation. Deliberately not exhaustive:
// every entry is a key something can plausibly be bound to, and a key that is
// not here reports an empty code -- "this platform did not say" -- rather than
// a guess. Guessing is how `code` stops meaning anything.
std::string CodeName(SDL_Scancode scancode) {
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return std::string("Key") + static_cast<char>('A' + (scancode - SDL_SCANCODE_A));
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return std::string("Digit") + static_cast<char>('1' + (scancode - SDL_SCANCODE_1));
  }
  switch (scancode) {
    case SDL_SCANCODE_0: return "Digit0";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_KP_ENTER: return "NumpadEnter";
    case SDL_SCANCODE_ESCAPE: return "Escape";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_LEFT: return "ArrowLeft";
    case SDL_SCANCODE_RIGHT: return "ArrowRight";
    case SDL_SCANCODE_UP: return "ArrowUp";
    case SDL_SCANCODE_DOWN: return "ArrowDown";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_MINUS: return "Minus";
    case SDL_SCANCODE_EQUALS: return "Equal";
    case SDL_SCANCODE_LEFTBRACKET: return "BracketLeft";
    case SDL_SCANCODE_RIGHTBRACKET: return "BracketRight";
    case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "Quote";
    case SDL_SCANCODE_GRAVE: return "Backquote";
    case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_PERIOD: return "Period";
    case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
    case SDL_SCANCODE_RSHIFT: return "ShiftRight";
    case SDL_SCANCODE_LCTRL: return "ControlLeft";
    case SDL_SCANCODE_RCTRL: return "ControlRight";
    case SDL_SCANCODE_LALT: return "AltLeft";
    case SDL_SCANCODE_RALT: return "AltRight";
    case SDL_SCANCODE_LGUI: return "MetaLeft";
    case SDL_SCANCODE_RGUI: return "MetaRight";
    default: return {};
  }
}

// SDL reports pointer positions in logical (window) coordinates as floats. The
// canvas is in physical pixels, so scaling happens here, once, at the boundary
// — not scattered across whoever consumes the event.
gfx::IntPoint ToPixelPoint(float logical_x, float logical_y, float scale) {
  return gfx::IntPoint{gfx::SaturateFloatToInt(logical_x * scale),
                       gfx::SaturateFloatToInt(logical_y * scale)};
}

std::uint8_t ToButton(Uint8 sdl_button) {
  return static_cast<std::uint8_t>(sdl_button);
}

Modifiers ToModifiers(SDL_Keymod mods) {
  Modifiers modifiers;
  modifiers.control = (mods & SDL_KMOD_CTRL) != 0;
  modifiers.shift = (mods & SDL_KMOD_SHIFT) != 0;
  modifiers.alt = (mods & SDL_KMOD_ALT) != 0;
  modifiers.meta = (mods & SDL_KMOD_GUI) != 0;
  return modifiers;
}

// What is held right now. A keyboard event carries its own modifier state; a
// mouse event does not, so this is the only way to know whether a click was a
// ctrl+click.
Modifiers HeldModifiers() {
  return ToModifiers(SDL_GetModState());
}

}  // namespace

SdlWindow::~SdlWindow() {
  Close();
}

bool SdlWindow::Open(const WindowOptions& options) {
  util::StartupTrace::Scope scope("SdlWindow::Open");

  if (window_ != nullptr) {
    return true;
  }

  // Video only. Audio, gamepad, and sensor subsystems are not initialized
  // because a browser that has not been asked to play media has no business
  // opening the sound device — that is a privacy posture, not just a startup
  // saving.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  initialized_sdl_ = true;

  const std::string title(options.title);
  window_ = SDL_CreateWindow(title.c_str(), options.width, options.height,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window_ == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    Close();
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (renderer_ == nullptr) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    Close();
    return false;
  }

  // Never present a frame that was not asked for. VSync would make the loop
  // wake at the display refresh rate whether or not anything changed, which is
  // exactly the idle-CPU behavior this project exists to avoid. Frame pacing,
  // when animation needs it, is the loop's job.
  SDL_SetRenderVSync(renderer_, 0);

  return true;
}

void SdlWindow::Close() {
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (initialized_sdl_) {
    SDL_Quit();
    initialized_sdl_ = false;
  }
}

gfx::IntSize SdlWindow::PixelSize() const {
  if (window_ == nullptr) {
    return gfx::IntSize{};
  }
  int width = 0;
  int height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
    return gfx::IntSize{};
  }
  return gfx::IntSize{width, height};
}

float SdlWindow::DeviceScale() const {
  if (window_ == nullptr) {
    return 1.0f;
  }
  const float scale = SDL_GetWindowPixelDensity(window_);
  return scale > 0.0f ? scale : 1.0f;
}

void SdlWindow::SetTitle(std::string_view title) {
  if (window_ == nullptr) {
    return;
  }
  const std::string owned(title);
  SDL_SetWindowTitle(window_, owned.c_str());
}

namespace {

// `pending_code` is the physical key of the KEY_DOWN that was swallowed because
// SDL was about to report the character it produced. SDL sends KEY_DOWN and then
// TEXT_INPUT for the same press, and only the first knows which key was struck
// while only the second knows what it typed; carrying one field between them is
// what lets one KeyEvent have both. See the TEXT_INPUT case.
std::optional<InputEvent> Translate(const SDL_Event& event, std::string& pending_code) {
  switch (event.type) {
    case SDL_EVENT_QUIT:
      return QuitEvent{};

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
      ResizeEvent resize;
      resize.pixel_size = gfx::IntSize{event.window.data1, event.window.data2};
      // The window pointer is not available here (Translate is deliberately
      // stateless), so the scale is filled in by the caller, which has one.
      resize.device_scale = 1.0f;
      return resize;
    }

    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
      return ExposeEvent{};

    case SDL_EVENT_MOUSE_MOTION: {
      PointerEvent pointer;
      pointer.kind = PointerEvent::Kind::Move;
      pointer.position = ToPixelPoint(event.motion.x, event.motion.y, 1.0f);
      pointer.modifiers = HeldModifiers();
      return pointer;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      PointerEvent pointer;
      pointer.kind = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PointerEvent::Kind::Down
                                                               : PointerEvent::Kind::Up;
      pointer.position = ToPixelPoint(event.button.x, event.button.y, 1.0f);
      pointer.button = ToButton(event.button.button);
      // Asked for rather than carried: a mouse event has no modifier field of
      // its own, and a ctrl+click that arrives as a click is a different act
      // reported as the wrong one.
      pointer.modifiers = HeldModifiers();
      return pointer;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
      WheelEvent wheel;
      // SDL reports wheel "clicks"; the pixels-per-click conversion is a UI
      // policy decision and belongs with the code that owns scrolling, so this
      // passes the raw click count through with a sign convention only.
      wheel.delta_x = static_cast<int>(event.wheel.x);
      wheel.delta_y = static_cast<int>(event.wheel.y);
      if (wheel.delta_x == 0 && wheel.delta_y == 0) {
        return std::nullopt;
      }
      return wheel;
    }

    case SDL_EVENT_TEXT_INPUT: {
      if (event.text.text == nullptr || event.text.text[0] == '\0') {
        return std::nullopt;
      }
      KeyEvent key;
      // ASCII only until there is real text handling; a multi-byte sequence
      // reports its first byte rather than a wrong codepoint.
      key.codepoint = static_cast<char32_t>(static_cast<unsigned char>(event.text.text[0]));
      key.pressed = true;
      // The key that produced it, from the KEY_DOWN this event follows. Taken
      // rather than copied: one press produces one character, and a stale code
      // on the next one would name the wrong key.
      //
      // `repeat` is not carried across. Auto-repeat of a printable key is
      // indistinguishable here from typing the same character quickly, and for
      // the one thing that acts on it -- inserting the character -- they are
      // the same act.
      key.code = std::move(pending_code);
      pending_code.clear();
      return key;
    }

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      const Key named = NamedKey(event.key.key);
      const Modifiers modifiers = ToModifiers(event.key.mod);

      // A printable key with no modifier arrives again as TEXT_INPUT, which is
      // the event that knows about layouts and dead keys. Reporting it here too
      // would type every character twice. Its *physical* key is remembered for
      // that event to pick up, because TEXT_INPUT does not carry one.
      //
      // Only the press. There is no TEXT_INPUT for a release, so suppressing
      // that one too is how a `keyup` for an ordinary character came to not
      // exist at all.
      if (named == Key::None && !modifiers.control && !modifiers.alt && !modifiers.meta &&
          event.type == SDL_EVENT_KEY_DOWN) {
        pending_code = CodeName(event.key.scancode);
        return std::nullopt;
      }
      KeyEvent key;
      key.key = named;
      key.code = CodeName(event.key.scancode);
      key.modifiers = modifiers;
      key.pressed = event.type == SDL_EVENT_KEY_DOWN;
      key.repeat = event.key.repeat;
      // Carried for shortcuts, where the character is the thing bound: ctrl+L
      // is a codepoint plus a modifier, not a named key.
      if (named == Key::None && event.key.key < 0x80) {
        key.codepoint = static_cast<char32_t>(event.key.key);
      }
      return key;
    }

    default:
      return std::nullopt;
  }
}

}  // namespace

bool SdlWindow::PollEvent(std::optional<InputEvent>& out) {
  SDL_Event event;
  if (!SDL_PollEvent(&event)) {
    return false;
  }
  out = Translate(event, pending_key_code_);
  return true;
}

bool SdlWindow::WaitEvent(std::optional<InputEvent>& out) {
  SDL_Event event;
  if (!SDL_WaitEvent(&event)) {
    return false;
  }
  out = Translate(event, pending_key_code_);
  return true;
}

bool SdlWindow::WaitEventTimeout(std::int32_t timeout_ms, std::optional<InputEvent>& out) {
  SDL_Event event;
  if (!SDL_WaitEventTimeout(&event, timeout_ms)) {
    return false;
  }
  out = Translate(event, pending_key_code_);
  return true;
}

bool SdlWindow::WaitEventOrDescriptors(std::span<const util::WaitDescriptor> descriptors,
                                       std::int32_t timeout_ms,
                                       std::optional<InputEvent>& out) {
  if (descriptors.empty()) {
    return timeout_ms < 0 ? WaitEvent(out) : WaitEventTimeout(timeout_ms, out);
  }
  // An event that has already arrived must not wait behind a socket.
  if (PollEvent(out)) {
    return true;
  }
  const std::int32_t bounded =
      timeout_ms < 0 ? kDescriptorWaitInputMs : std::min(timeout_ms, kDescriptorWaitInputMs);
  WaitOnDescriptors(descriptors, bounded);
  // Whatever woke it, the caller wants both halves drained: a socket becoming
  // ready is picked up by the engine on the way round, and an event is picked
  // up here.
  return PollEvent(out);
}

}  // namespace microbrowser::platform

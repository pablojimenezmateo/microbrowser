#include "platform/SdlWindow.h"

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

std::optional<InputEvent> Translate(const SDL_Event& event) {
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
      return pointer;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      PointerEvent pointer;
      pointer.kind = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PointerEvent::Kind::Down
                                                               : PointerEvent::Kind::Up;
      pointer.position = ToPixelPoint(event.button.x, event.button.y, 1.0f);
      pointer.button = ToButton(event.button.button);
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
      return key;
    }

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      const Key named = NamedKey(event.key.key);
      const SDL_Keymod mods = event.key.mod;
      Modifiers modifiers;
      modifiers.control = (mods & SDL_KMOD_CTRL) != 0;
      modifiers.shift = (mods & SDL_KMOD_SHIFT) != 0;
      modifiers.alt = (mods & SDL_KMOD_ALT) != 0;
      modifiers.meta = (mods & SDL_KMOD_GUI) != 0;

      // A printable key with no modifier arrives again as TEXT_INPUT, which is
      // the event that knows about layouts and dead keys. Reporting it here too
      // would type every character twice.
      if (named == Key::None && !modifiers.control && !modifiers.alt && !modifiers.meta) {
        return std::nullopt;
      }
      KeyEvent key;
      key.key = named;
      key.modifiers = modifiers;
      key.pressed = event.type == SDL_EVENT_KEY_DOWN;
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
  out = Translate(event);
  return true;
}

bool SdlWindow::WaitEvent(std::optional<InputEvent>& out) {
  SDL_Event event;
  if (!SDL_WaitEvent(&event)) {
    return false;
  }
  out = Translate(event);
  return true;
}

bool SdlWindow::WaitEventTimeout(std::int32_t timeout_ms, std::optional<InputEvent>& out) {
  SDL_Event event;
  if (!SDL_WaitEventTimeout(&event, timeout_ms)) {
    return false;
  }
  out = Translate(event);
  return true;
}

}  // namespace microbrowser::platform

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "gfx/Geometry.h"
#include "platform/InputEvent.h"

struct SDL_Window;
struct SDL_Renderer;

namespace microbrowser::platform {

struct WindowOptions {
  int width = 1280;
  int height = 800;
  std::string_view title = "microbrowser";
};

// Owns the SDL window, renderer, and SDL's own initialization, as one RAII
// unit. Non-copyable and non-movable: there is exactly one of these, and making
// it movable would only add a moved-from state for the destructor to worry
// about.
//
// SDL types are forward-declared here so that including this header does not
// drag SDL into a translation unit that only wants a window size. The
// architecture lint checks that no header outside src/platform includes SDL at
// all; this keeps that honest rather than merely true by accident.
class SdlWindow {
 public:
  SdlWindow() = default;
  ~SdlWindow();

  SdlWindow(const SdlWindow&) = delete;
  SdlWindow& operator=(const SdlWindow&) = delete;

  // Returns false and logs on failure; the caller exits. Safe to call once.
  bool Open(const WindowOptions& options);
  void Close();

  bool IsOpen() const { return window_ != nullptr; }
  SDL_Renderer* Renderer() const { return renderer_; }

  // Size of the drawable surface in physical pixels — what the canvas must
  // match. Not the logical window size: on a scaled display they differ, and
  // rasterizing at logical size is how a browser ends up blurry.
  gfx::IntSize PixelSize() const;
  float DeviceScale() const;

  void SetTitle(std::string_view title);

  // Event retrieval. Each returns true when an event was taken off the queue,
  // and sets `out` only when that event is one the application cares about —
  // "an event arrived" and "the event was interesting" are different questions,
  // and collapsing them is how a drain loop ends early.
  //
  // These are the *only* three ways the process sleeps. Keeping them here, and
  // out of the app loop, is what lets the app layer be built and tested without
  // a window system at all, and is enforced by the module contract: src/app
  // declares no external dependencies, so it cannot include SDL even by
  // accident.
  bool PollEvent(std::optional<InputEvent>& out);
  bool WaitEvent(std::optional<InputEvent>& out);
  bool WaitEventTimeout(std::int32_t timeout_ms, std::optional<InputEvent>& out);

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool initialized_sdl_ = false;
};

}  // namespace microbrowser::platform

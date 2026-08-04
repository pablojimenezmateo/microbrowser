#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "gfx/Geometry.h"
#include "platform/InputEvent.h"
#include "util/WaitDescriptor.h"

struct SDL_Window;
struct SDL_Renderer;

namespace microbrowser::platform {

// How long a wait that is watching sockets may hold an input event. See
// `SdlWindow::WaitEventOrDescriptors` for why this number exists at all; it is
// one frame at 60Hz, which is the point below which a delay stops being
// something a hand can feel.
inline constexpr std::int32_t kDescriptorWaitInputMs = 16;

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

  // Waits for a window event, for one of `descriptors` to become ready, or for
  // the timeout -- whichever comes first. A negative timeout means no deadline.
  // With no descriptors it is exactly the two calls above, which is what keeps
  // an idle browser blocking on input alone.
  //
  // **This is the one place the ADR 0011 design is approximated, and the
  // approximation is bounded.** SDL exposes no descriptor for its own event
  // queue, so there is no single call that can wait on both. With sockets
  // outstanding this waits on them and caps the wait at
  // `kDescriptorWaitInputMs`, so a socket wakes the loop the instant it is
  // ready and an input event waits at worst that long. It costs a wakeup every
  // 16ms *while a load is in flight and nothing else is happening*, and nothing
  // at all when none is -- which is the case the zero-idle-CPU invariant is
  // about. Removing the cap needs the display connection's own descriptor,
  // which SDL only offers through platform-specific window properties and
  // would make X11 or Wayland a build dependency: an ADR, not a patch.
  bool WaitEventOrDescriptors(std::span<const util::WaitDescriptor> descriptors,
                              std::int32_t timeout_ms, std::optional<InputEvent>& out);

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  bool initialized_sdl_ = false;
};

}  // namespace microbrowser::platform

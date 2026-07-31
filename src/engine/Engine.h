#pragma once

#include <string>

#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "ipc/Message.h"
#include "ipc/Transport.h"

namespace microbrowser::engine {

// The engine half of the seam.
//
// At M0 it has no DOM, no CSS, and no network — it turns a navigation into a
// placeholder page and paints it. What it establishes is the shape everything
// later plugs into:
//
//   * It talks to the outside world only through ipc::EngineEndpoint. It has no
//     window handle, no renderer, no canvas, and no way to acquire one.
//   * It is driven, never driving. HandlePendingMessages() runs to completion
//     and returns; it does not own a loop or a thread. A future process split
//     gives it its own loop without changing anything above.
//   * Painting is producing a display list. It never touches a pixel.
//
// The temptation this class must resist for the next year is becoming the place
// where "the browser" lives. Document, navigation history, network, and script
// each get their own type; Engine stays the thing that routes messages to them.
// Its budget in src/engine/MODULE.deps is the tripwire.
class Engine {
 public:
  explicit Engine(ipc::EngineEndpoint& endpoint);

  // Drain and act on everything the UI has queued. Returns true when the engine
  // produced any outgoing message, which is what tells the host loop a repaint
  // may be pending.
  bool HandlePendingMessages();

  const std::string& Title() const { return title_; }
  const std::string& Url() const { return url_; }
  gfx::IntSize ViewportSize() const { return viewport_size_; }

 private:
  void Navigate(const std::string& url);
  void SetViewport(const gfx::IntSize& size, float device_scale);
  void ScrollBy(int delta_x, int delta_y);

  // Rebuild the display list from current state and send it with full-viewport
  // damage. Incremental damage arrives with the paint system in M6; reporting
  // the truth (everything changed) is the correct placeholder, and is why the
  // damage field is not simply omitted.
  void PaintAndSend();

  ipc::EngineEndpoint& endpoint_;
  gfx::DisplayList display_list_;
  gfx::IntSize viewport_size_;
  float device_scale_ = 1.0f;
  int scroll_y_ = 0;
  std::string url_;
  std::string title_;
};

}  // namespace microbrowser::engine

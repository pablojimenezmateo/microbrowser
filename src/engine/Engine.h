#pragma once

#include <string>

#include "engine/Loader.h"
#include "engine/Page.h"
#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "ipc/Message.h"
#include "ipc/Transport.h"

namespace microbrowser::engine {

// The engine half of the seam.
//
// It loads a URL, parses it into a document, resolves its styles, lays it out,
// and paints it into a display list. The properties that matter are structural:
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
  // Fonts arrive from the caller because which fonts exist is a property of
  // the machine, and the engine is the half of the seam that does not know
  // what machine it is on. That is the same reason it has no window.
  Engine(ipc::EngineEndpoint& endpoint, gfx::FontProvider& fonts);

  // Drain and act on everything the UI has queued. Returns true when the engine
  // produced any outgoing message, which is what tells the host loop a repaint
  // may be pending.
  bool HandlePendingMessages();

  // Milliseconds until the page's soonest timer, or nothing when it has none.
  // The loop asks this to decide how long it may sleep, which is what keeps a
  // page with nothing scheduled from ever waking it.
  std::optional<std::uint32_t> NextTimerDelay() const;
  // Runs every timer that is due, and repaints when one changed the page.
  // True when anything ran.
  bool RunDueTimers();

  const std::string& Title() const { return page_.Title(); }
  const std::string& Url() const { return page_.Url(); }
  gfx::IntSize ViewportSize() const { return viewport_size_; }

  // The loader, so a caller can install a transport or adjust privacy settings
  // before the first navigation. Tests serve canned bytes through it; there is
  // no other way to exercise a navigation without a network.
  Loader& PageLoader() { return loader_; }

 private:
  void Navigate(const std::string& url);
  void Navigate(const std::string& url, const net::FetchOptions& options);
  void Navigate(const std::string& url, const net::FetchOptions& options,
                const url::Url* referrer_document);
  void NavigateFromCurrentDocument(const std::string& url, const net::FetchOptions& options);
  bool Navigate(const FormSubmission& submission);
  void SetViewport(const gfx::IntSize& size, float device_scale);
  void ScrollBy(int delta_x, int delta_y);
  bool HandlePointer(const ipc::PointerMessage& pointer);
  // Fetches what the document referenced -- stylesheets, then images -- before
  // the first layout, because both change it.
  void LoadSubresources(bool bypass_cache);

  // Lays out at the current viewport width, then paints. Separate from
  // PaintAndSend because scrolling repaints without relaying out, and a
  // scroll that ran layout would be the classic reason scrolling is slow.
  void LayoutAndPaint();

  // Rebuild the display list from current state and send it with full-viewport
  // damage. Incremental damage arrives with the paint system in M6; reporting
  // the truth (everything changed) is the correct placeholder, and is why the
  // damage field is not simply omitted.
  void PaintAndSend();

  // Renders `message` as the page, for a load that failed. A blank window is
  // indistinguishable from a hung browser.
  void ShowError(std::string_view url, std::string_view message);

  // Clamped so that scrolling stops at the end of the document rather than
  // running off into blank space.
  int MaxScroll() const;

  ipc::EngineEndpoint& endpoint_;
  Loader loader_;
  Page page_;
  // The frame most recently sent, kept so the next one can be diffed against
  // it. This is what the display list being a comparable value buys: damage is
  // computed from two frames rather than trusted from every call site that
  // invalidated something.
  gfx::DisplayList display_list_;
  // Reused rather than reallocated per frame; painting is the hot path.
  gfx::DisplayList pending_;
  gfx::IntSize viewport_size_;
  float device_scale_ = 1.0f;
  int scroll_y_ = 0;
};

}  // namespace microbrowser::engine

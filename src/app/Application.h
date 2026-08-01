#pragma once

#include "app/AppStartupOptions.h"
#include "engine/Engine.h"
#include "gfx/Canvas.h"
#include "gfx/DirtyRegion.h"
#include "gfx/DisplayList.h"
#include "gfx/Painter.h"
#include "gfx/TextRenderer.h"
#include "ipc/InProcessTransport.h"
#include "platform/AppDirectories.h"
#include "platform/InputEvent.h"
#include "platform/SdlPresenter.h"
#include "platform/SdlWindow.h"
#include "platform/SystemFonts.h"
#include "ui/BrowserChrome.h"

namespace microbrowser::app {

// The host process: owns the window, the canvas, the transport, and the loop.
//
// It is the UI side of the seam, and it must stay that way. It may not include
// a dom/, css/, layout/, or html/ header, and the architecture lint enforces
// that. Everything it knows about the page arrives as an ipc::EngineToUi
// message.
//
// This class has a strict budget (see src/app/MODULE.deps) because it is the
// obvious place for unrelated work to accumulate — the browser equivalent of
// the god object every UI shell grows. Tabs, the omnibox, history, and settings
// each get their own type in src/ui; Application routes to them.
class Application {
 public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  // Returns the process exit code. Opens the window, runs until quit, closes.
  int Run(const AppStartupOptions& options);

 private:
  // One iteration: wait per policy, drain events, let the engine act, consume
  // its output, and present if anything changed. Returns false to stop.
  bool RunOneIteration();

  // Blocks, sleeps, or polls per IdleWaitStrategy, then drains up to the event
  // budget. Returns false when the window asked to close.
  bool WaitAndDrainEvents();

  void HandleInputEvent(const platform::InputEvent& event);
  void ConsumeEngineMessages();

  // Rasterize the pending display list over the damaged rects and present.
  void PaintAndPresent();

  void SyncViewportToWindow();
  void InvalidateAll();

  // Acts on what the chrome decided, and marks the chrome for repaint. Kept
  // apart from event handling so that "what the click meant" and "what the
  // browser does about it" are separately testable.
  void ApplyChromeResponse(const ui::BrowserChrome::Response& response);
  void InvalidateChrome();
  // Where the page's pixels start, which is below the toolbar.
  gfx::IntPoint PageOrigin() const;
  ui::Toolbar::OmniboxMetrics MeasureOmnibox();

  platform::SdlWindow window_;
  platform::SdlPresenter presenter_;
  platform::AppDirectories directories_;

  gfx::Canvas canvas_;
  gfx::DirtyRegion dirty_;
  gfx::DisplayList display_list_;
  // Declared after the canvas it draws into, and holding a pointer rather than a
  // copy, so a resize that reallocates the pixel buffer leaves it valid.
  gfx::Painter painter_{canvas_};

  // One font stack, shared by the engine that measures text and the painter
  // that draws it. In process, that sharing is free and correct. After the
  // process split each side gets its own -- which is why the engine takes a
  // gfx::FontProvider rather than reaching for a global one.
  gfx::FontLibrary font_library_;
  platform::SystemFontProvider fonts_{font_library_};
  gfx::TextRenderer text_{fonts_};

  ipc::InProcessChannel channel_;
  engine::Engine engine_{channel_.Engine(), fonts_};

  // The browser around the page. It gets every event first and the page gets
  // what is left, which is the only ordering under which a page cannot steal
  // ctrl+L.
  ui::BrowserChrome chrome_;
  gfx::DisplayList chrome_list_;

  bool running_ = true;
  bool repaint_pending_ = false;
  // Set when damage cannot be trusted: first frame, resize, expose. Distinct
  // from "the whole viewport is dirty" so the presenter can also skip its
  // partial-upload bookkeeping.
  bool full_repaint_pending_ = true;
};

}  // namespace microbrowser::app

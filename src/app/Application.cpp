#include "app/Application.h"

#include <cstdio>
#include <utility>

#include "app/DirtyRegionPolicy.h"
#include "app/EventDrainBudget.h"
#include "app/IdleWaitStrategy.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"

namespace microbrowser::app {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Wheel deltas arrive as notches. This is the only place a notch becomes a
// pixel count, so changing scroll feel is a one-line change here rather than a
// hunt through the engine.
constexpr int kPixelsPerWheelNotch = 53;

// MICROBROWSER_TRACE_REDRAW=1 prints one line per presented frame: whether it
// was full or partial, how many rects it covered, and what fraction of the
// surface that was.
//
// A function-local static rather than an Application member on purpose: the
// env read happens once, on first use, and the class does not grow a field for
// a debugging flag. Application's member budget exists to make exactly that
// kind of incremental widening visible.
bool RedrawTraceEnabled() {
  static const bool enabled = util::PerformanceTrace::FlagEnabled("MICROBROWSER_TRACE_REDRAW");
  return enabled;
}

}  // namespace

Application::Application() = default;
Application::~Application() = default;

int Application::Run(const AppStartupOptions& options) {
  util::StartupTrace::Reset();
  util::StartupTrace::Scope scope("Application::Run");

  // Best-effort: a profile directory that cannot be created costs persistence,
  // not the session. Browsing without saving anything is a legitimate mode, and
  // in this project it is arguably the more principled one.
  directories_.EnsureExist();

  platform::WindowOptions window_options;
  window_options.width = options.width;
  window_options.height = options.height;
  if (!window_.Open(window_options)) {
    return 1;
  }

  SyncViewportToWindow();
  channel_.Ui().Send(ipc::NavigateMessage{options.url});

  while (running_) {
    if (!RunOneIteration()) {
      break;
    }
  }

  window_.Close();
  util::PerformanceTrace::DumpSummaryOnce();
  util::StartupTrace::DumpSummaryOnce();
  util::DumpPerformanceCountersOnce();
  return 0;
}

bool Application::RunOneIteration() {
  AddPerformanceCounter(PerfCounterId::LoopIterations);

  if (!WaitAndDrainEvents()) {
    return false;
  }

  // The engine runs inline here. When it moves to its own process this call
  // disappears and the messages arrive over a socket instead; nothing else in
  // this function changes.
  engine_.HandlePendingMessages();
  ConsumeEngineMessages();

  if (repaint_pending_) {
    PaintAndPresent();
  }
  return running_;
}

bool Application::WaitAndDrainEvents() {
  IdleWaitState state;
  state.repaint_pending = repaint_pending_;
  state.messages_pending = channel_.PendingForEngine() > 0 || channel_.PendingForUi() > 0;
  // No animations, timers, or blinking caret exist yet, so there is genuinely
  // nothing scheduled and the loop is free to block indefinitely. This is what
  // makes idle CPU zero, and it is the property every later feature has to
  // justify breaking.
  const IdleWaitDecision decision = ChooseIdleWait(state);

  std::optional<platform::InputEvent> translated;
  bool have_event = false;
  switch (decision.mode) {
    case IdleWaitMode::Poll:
      AddPerformanceCounter(PerfCounterId::LoopPolls);
      have_event = window_.PollEvent(translated);
      break;
    case IdleWaitMode::WaitTimeout:
      AddPerformanceCounter(PerfCounterId::LoopTimedWaits);
      have_event = window_.WaitEventTimeout(decision.timeout_ms, translated);
      break;
    case IdleWaitMode::Wait:
      AddPerformanceCounter(PerfCounterId::LoopBlockingWaits);
      have_event = window_.WaitEvent(translated);
      break;
  }

  int processed = 0;
  while (have_event) {
    AddPerformanceCounter(PerfCounterId::LoopEventsProcessed);
    ++processed;

    if (translated.has_value()) {
      HandleInputEvent(*translated);
      if (!running_) {
        return false;
      }
    }

    if (ShouldYieldEventDrain(processed, repaint_pending_)) {
      AddPerformanceCounter(PerfCounterId::LoopEventDrainYields);
      break;
    }
    translated.reset();
    have_event = window_.PollEvent(translated);
  }

  return true;
}

void Application::HandleInputEvent(const platform::InputEvent& event) {
  if (std::holds_alternative<platform::QuitEvent>(event)) {
    running_ = false;
    return;
  }

  if (std::holds_alternative<platform::ResizeEvent>(event)) {
    // Read the size back from the window rather than trusting the event: a
    // burst of resize events during a drag would otherwise make us allocate a
    // canvas per intermediate size, and the window already coalesced them.
    SyncViewportToWindow();
    return;
  }

  if (std::holds_alternative<platform::ExposeEvent>(event)) {
    InvalidateAll();
    return;
  }

  if (const auto* wheel = std::get_if<platform::WheelEvent>(&event)) {
    channel_.Ui().Send(ipc::ScrollMessage{wheel->delta_x * kPixelsPerWheelNotch,
                                          -wheel->delta_y * kPixelsPerWheelNotch});
    return;
  }

  if (const auto* pointer = std::get_if<platform::PointerEvent>(&event)) {
    ipc::PointerMessage message;
    message.kind = static_cast<ipc::PointerMessage::Kind>(pointer->kind);
    message.position = pointer->position;
    message.button = pointer->button;
    channel_.Ui().Send(message);
    return;
  }

  // KeyEvent has no consumer until there is an omnibox to type into.
}

void Application::ConsumeEngineMessages() {
  while (std::optional<ipc::EngineToUi> message = channel_.Ui().TryReceive()) {
    if (auto* paint = std::get_if<ipc::PaintFrameMessage>(&*message)) {
      display_list_ = std::move(paint->display_list);
      if (paint->damage.empty()) {
        // Empty damage means the engine could not narrow it. Honor that rather
        // than inventing a region.
        InvalidateAll();
      } else {
        for (const gfx::IntRect& rect : paint->damage) {
          dirty_.Add(rect);
        }
        repaint_pending_ = true;
      }
    } else if (const auto* title = std::get_if<ipc::TitleChangedMessage>(&*message)) {
      window_.SetTitle(title->title);
    }
    // LoadProgress and NavigationCommitted have no surface to display them
    // until the UI chrome exists in M7.
  }
}

void Application::PaintAndPresent() {
  util::PerformanceTrace::Scope scope("Application::PaintAndPresent");

  if (canvas_.IsEmpty()) {
    repaint_pending_ = false;
    return;
  }

  dirty_.IntersectWith(canvas_.Bounds());

  const DirtyRegionAnalysis analysis = AnalyzeDirtyRegion(dirty_, canvas_.Bounds());
  const bool full = full_repaint_pending_ || ShouldPromoteToFullRepaint(analysis);

  if (full) {
    gfx::Execute(display_list_, canvas_, canvas_.Bounds());
  } else {
    for (const gfx::IntRect& rect : dirty_.Rects()) {
      gfx::Execute(display_list_, canvas_, rect);
    }
  }

  if (RedrawTraceEnabled()) {
    std::fprintf(stderr, "[redraw] %-7s rects=%zu coverage=%5.1f%% surface=%dx%d commands=%zu\n",
                 full ? "full" : "partial", analysis.rect_count,
                 static_cast<double>(analysis.coverage) * 100.0, canvas_.Width(), canvas_.Height(),
                 display_list_.Size());
  }

  if (!presenter_.Present(window_.Renderer(), canvas_, dirty_, full)) {
    // The presenter dropped its texture. Do not clear the pending flags: the
    // next iteration must try again with a full repaint rather than leaving a
    // stale or empty window on screen.
    full_repaint_pending_ = true;
    return;
  }

  dirty_.Clear();
  repaint_pending_ = false;
  full_repaint_pending_ = false;
}

void Application::SyncViewportToWindow() {
  const gfx::IntSize size = window_.PixelSize();
  if (size.IsEmpty()) {
    return;
  }

  if (size.width != canvas_.Width() || size.height != canvas_.Height()) {
    canvas_.Resize(size.width, size.height);
    presenter_.Reset();
    InvalidateAll();
  }

  channel_.Ui().Send(ipc::ResizeViewportMessage{size, window_.DeviceScale()});
}

void Application::InvalidateAll() {
  dirty_.Clear();
  dirty_.Add(canvas_.Bounds());
  repaint_pending_ = true;
  full_repaint_pending_ = true;
}

}  // namespace microbrowser::app

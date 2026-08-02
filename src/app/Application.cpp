#include "app/Application.h"

#include <cstdio>
#include <optional>
#include <string>
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

void AppendUtf8(char32_t codepoint, std::string& out) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

std::string TextInputFor(const platform::KeyEvent& event) {
  if (!event.pressed || !event.modifiers.PlainTyping() || event.codepoint < 0x20 ||
      event.codepoint == 0x7F) {
    return {};
  }
  std::string text;
  AppendUtf8(event.codepoint, text);
  return text;
}

std::optional<ipc::InputCommandMessage> InputCommandFor(const platform::KeyEvent& event) {
  if (!event.pressed || event.modifiers.Any()) {
    return std::nullopt;
  }
  using Command = ipc::InputCommandMessage::Command;
  switch (event.key) {
    case platform::Key::Backspace:
      return ipc::InputCommandMessage{Command::Backspace};
    case platform::Key::Delete:
      return ipc::InputCommandMessage{Command::Delete};
    case platform::Key::Enter:
      return ipc::InputCommandMessage{Command::Enter};
    default:
      return std::nullopt;
  }
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

  {
    // Indexing the installed fonts, before the window opens. Each file is
    // parsed for its metadata and the bytes are dropped; the files themselves
    // load lazily, when a page selects one. A machine with no fonts still
    // runs -- text simply does not draw, which is a legible failure rather
    // than a crash on the first paragraph.
    util::StartupTrace::Scope fonts_scope("Application::ScanFonts");
    fonts_.Scan();
  }

  platform::WindowOptions window_options;
  window_options.width = options.width;
  window_options.height = options.height;
  if (!window_.Open(window_options)) {
    return 1;
  }

  SyncViewportToWindow();
  chrome_.GetToolbar().Omnibox().SetText(options.url);
  InvalidateChrome();
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
    const ui::BrowserChrome::Response response = chrome_.HandlePointer(*pointer);
    ApplyChromeResponse(response);
    if (response.handled) {
      return;
    }
    ipc::PointerMessage message;
    message.kind = static_cast<ipc::PointerMessage::Kind>(pointer->kind);
    // In page coordinates. The page's idea of where the pointer is has to match
    // where its own pixels are, or every hit test is off by the toolbar.
    const gfx::IntPoint origin = PageOrigin();
    message.position = gfx::IntPoint{pointer->position.x - origin.x,
                                     pointer->position.y - origin.y};
    message.button = pointer->button;
    channel_.Ui().Send(message);
    return;
  }

  if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
    // The chrome first, always. A page that could see ctrl+L before the browser
    // did could stop the user leaving it.
    const ui::BrowserChrome::Response response = chrome_.HandleKey(*key);
    ApplyChromeResponse(response);
    if (!response.handled) {
      if (const std::optional<ipc::InputCommandMessage> command = InputCommandFor(*key)) {
        channel_.Ui().Send(*command);
      } else if (std::string text = TextInputFor(*key); !text.empty()) {
        channel_.Ui().Send(ipc::TextInputMessage{std::move(text)});
      }
    }
    return;
  }
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
        // Translated into window coordinates: the engine reports damage in the
        // page's space, which starts below the toolbar.
        const gfx::IntPoint origin = PageOrigin();
        for (const gfx::IntRect& rect : paint->damage) {
          dirty_.Add(rect.Translated(origin.x, origin.y));
        }
        repaint_pending_ = true;
      }
    } else if (const auto* title = std::get_if<ipc::TitleChangedMessage>(&*message)) {
      chrome_.OnTitleChanged(title->title);
      window_.SetTitle(chrome_.WindowTitle());
    } else if (const auto* committed =
                   std::get_if<ipc::NavigationCommittedMessage>(&*message)) {
      chrome_.OnNavigationCommitted(committed->url);
      InvalidateChrome();
    }
    // LoadProgress has no surface to display it: a progress bar needs a load
    // that takes long enough to see, and loading is synchronous today.
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

  // The chrome at the origin, the page below it. Two lists rather than one
  // because they come from different places -- one from this process, one
  // across the IPC seam -- and merging them would mean the engine's list could
  // name coordinates inside the chrome.
  const gfx::IntPoint origin = PageOrigin();
  const auto execute_all = [this, origin](const gfx::IntRect& region) {
    painter_.SetTransform(gfx::AffineTransform{});
    gfx::Execute(chrome_list_, painter_, region, &text_);
    painter_.SetTransform(gfx::AffineTransform::Translation(static_cast<float>(origin.x),
                                                            static_cast<float>(origin.y)));
    gfx::Execute(display_list_, painter_, region, &text_);
    painter_.SetTransform(gfx::AffineTransform{});
  };

  if (full) {
    execute_all(canvas_.Bounds());
  } else {
    for (const gfx::IntRect& rect : dirty_.Rects()) {
      execute_all(rect);
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

// Where the caret and selection edges fall, measured with the same font the
// toolbar draws with. Measuring with a different one puts the caret in the
// wrong place, which is why the toolbar exposes its font rather than the app
// choosing one.
ui::Toolbar::OmniboxMetrics Application::MeasureOmnibox() {
  const ui::TextField& field = chrome_.GetToolbar().Omnibox();
  const gfx::FontRequest font = ui::Toolbar::OmniboxFont();
  const std::string_view text = field.Text();
  const auto advance = [&](std::size_t bytes) {
    return text_.MeasureRun(text.substr(0, std::min(bytes, text.size())), font);
  };
  return ui::Toolbar::OmniboxMetrics{advance(field.Caret()), advance(field.SelectionBegin()),
                                     advance(field.SelectionEnd())};
}

gfx::IntPoint Application::PageOrigin() const {
  const gfx::IntRect page = chrome_.PageBounds(gfx::IntSize{canvas_.Width(), canvas_.Height()});
  return gfx::IntPoint{page.x, page.y};
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

  chrome_.SetViewportWidth(size.width);
  InvalidateChrome();

  // The engine is told the *page's* size, not the window's. That is what makes
  // it impossible for a page to paint over the chrome: it is never given those
  // pixels, rather than being trusted not to use them.
  const gfx::IntRect page = chrome_.PageBounds(size);
  channel_.Ui().Send(
      ipc::ResizeViewportMessage{gfx::IntSize{page.width, page.height}, window_.DeviceScale()});
}

void Application::InvalidateChrome() {
  chrome_list_.Clear();
  chrome_.GetToolbar().Paint(chrome_list_, MeasureOmnibox());
  dirty_.Add(chrome_.GetToolbar().Bounds());
  repaint_pending_ = true;
}

void Application::ApplyChromeResponse(const ui::BrowserChrome::Response& response) {
  if (response.needs_repaint) {
    InvalidateChrome();
  }
  if (!response.intent.has_value()) {
    return;
  }
  switch (response.intent->kind) {
    case ui::BrowserChrome::Intent::Kind::Navigate:
      channel_.Ui().Send(ipc::NavigateMessage{response.intent->url});
      break;
    case ui::BrowserChrome::Intent::Kind::Reload:
      channel_.Ui().Send(ipc::ReloadMessage{response.intent->bypass_cache});
      break;
    case ui::BrowserChrome::Intent::Kind::ScrollPage:
      channel_.Ui().Send(ipc::ScrollMessage{0, response.intent->scroll_delta});
      break;
  }
}

void Application::InvalidateAll() {
  dirty_.Clear();
  dirty_.Add(canvas_.Bounds());
  repaint_pending_ = true;
  full_repaint_pending_ = true;
}

}  // namespace microbrowser::app

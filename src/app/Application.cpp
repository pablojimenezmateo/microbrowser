#include "app/Application.h"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "app/DirtyRegionPolicy.h"
#include "app/EventDrainBudget.h"
#include "app/IdleWaitStrategy.h"
#include "app/KeyRouting.h"
#include "util/PerformanceCounters.h"
#include "util/WaitDescriptor.h"
#include "util/LoadTimeline.h"
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

// The text a key inserts, or empty. Not the same question as what the key
// *is*: a key with a modifier on it is a shortcut and inserts nothing, and a
// control character is not text however it arrived.
std::string TextInputFor(const platform::KeyEvent& event) {
  if (!event.pressed || !event.modifiers.PlainTyping() || event.codepoint < 0x20 ||
      event.codepoint == 0x7F) {
    return {};
  }
  std::string text;
  AppendUtf8(event.codepoint, text);
  return text;
}

// What the key *means*, as the DOM names it. The named keys get their DOM
// spelling; anything else is the character it produced, which is what `key` is
// for a printable key. "Unidentified" is the specification's answer for a key
// that produced neither, and it is a real answer rather than an empty string
// that a page would read as a key with no name.
std::string KeyNameFor(const platform::KeyEvent& event) {
  switch (event.key) {
    case platform::Key::Enter: return "Enter";
    case platform::Key::Escape: return "Escape";
    case platform::Key::Backspace: return "Backspace";
    case platform::Key::Delete: return "Delete";
    case platform::Key::Tab: return "Tab";
    case platform::Key::Left: return "ArrowLeft";
    case platform::Key::Right: return "ArrowRight";
    case platform::Key::Up: return "ArrowUp";
    case platform::Key::Down: return "ArrowDown";
    case platform::Key::Home: return "Home";
    case platform::Key::End: return "End";
    case platform::Key::PageUp: return "PageUp";
    case platform::Key::PageDown: return "PageDown";
    case platform::Key::None:
      break;
  }
  if (event.codepoint == 0x20) {
    return " ";
  }
  if (event.codepoint >= 0x20 && event.codepoint != 0x7F) {
    std::string name;
    AppendUtf8(event.codepoint, name);
    return name;
  }
  return "Unidentified";
}

ipc::InputModifiers ModifiersFor(const platform::Modifiers& modifiers) {
  return ipc::InputModifiers{modifiers.control, modifiers.shift, modifiers.alt, modifiers.meta};
}

// A platform key event as the message ADR 0017 §1 describes: what key it was,
// what it means, and what it inserts, as three separate strings. The split
// happens here rather than in the engine because the engine is the process that
// will be sandboxed, and a keyboard layout is host state.
ipc::KeyInputMessage KeyMessageFor(const platform::KeyEvent& event) {
  ipc::KeyInputMessage message;
  message.kind =
      event.pressed ? ipc::KeyInputMessage::Kind::Down : ipc::KeyInputMessage::Kind::Up;
  message.code = event.code;
  message.key = KeyNameFor(event);
  message.text = TextInputFor(event);
  message.modifiers = ModifiersFor(event.modifiers);
  message.repeat = event.repeat;
  return message;
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
  util::LoadTimeline::DumpOnce(stderr);
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
  // A load in flight moves one turn. It is a separate call from the message
  // drain because it is driven by the network rather than by the UI, and after
  // the process split the two arrive on different descriptors.
  engine_.Advance();
  // Due timers and the animation frame run at the top of a turn, before
  // anything is painted, so a callback that changes the page is on screen in
  // the same frame.
  engine_.RunDueWork();
  ConsumeEngineMessages();

  if (repaint_pending_) {
    PaintAndPresent();
  }
  return running_;
}

bool Application::WaitAndDrainEvents() {
  // Deliberately a local rather than a member. It is empty whenever nothing is
  // outstanding -- which is most of a browser's life and costs no allocation at
  // all -- and Application's member budget exists to make growth like this a
  // decision rather than a habit.
  util::WaitDescriptorList descriptors;
  engine_.AppendWaitDescriptors(descriptors);

  IdleWaitState state;
  state.repaint_pending = repaint_pending_;
  state.messages_pending = channel_.PendingForEngine() > 0 || channel_.PendingForUi() > 0;
  state.work_runnable = engine_.HasRunnableWork();
  // The soonest deadline the engine has: a page timer, an animation frame, or
  // the point at which a silent server is given up on. A page with none pending
  // hands back nothing and the loop blocks indefinitely, which is what keeps
  // idle CPU at zero -- the deadline exists only while something is actually
  // waiting for it, and disappears the moment nothing is.
  state.next_deadline_ms = engine_.NextDeadlineMs();
  // And the sockets. Empty when nothing is loading, which is the case the
  // invariant is about.
  state.descriptors = descriptors;
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
      have_event = decision.watch_descriptors
                       ? window_.WaitEventOrDescriptors(descriptors, decision.timeout_ms,
                                                        translated)
                       : window_.WaitEventTimeout(decision.timeout_ms, translated);
      break;
    case IdleWaitMode::Wait:
      AddPerformanceCounter(PerfCounterId::LoopBlockingWaits);
      have_event = decision.watch_descriptors
                       ? window_.WaitEventOrDescriptors(descriptors, -1, translated)
                       : window_.WaitEvent(translated);
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
    // In page coordinates, like a pointer event: the engine routes the wheel to
    // whatever box is under it, and a position measured from the window would
    // be off by the toolbar.
    const gfx::IntPoint origin = PageOrigin();
    channel_.Ui().Send(ipc::ScrollMessage{
        wheel->delta_x * kPixelsPerWheelNotch, -wheel->delta_y * kPixelsPerWheelNotch,
        gfx::IntPoint{pointer_.x - origin.x, pointer_.y - origin.y}});
    return;
  }

  if (const auto* pointer = std::get_if<platform::PointerEvent>(&event)) {
    pointer_ = pointer->position;
    const ui::BrowserChrome::Response response = chrome_.HandlePointer(*pointer);
    ApplyChromeResponse(response);
    if (response.handled) {
      return;
    }
    ipc::PointerInputMessage message;
    message.kind = static_cast<ipc::PointerInputMessage::Kind>(pointer->kind);
    // In page coordinates, and in CSS pixels. The page's idea of where the
    // pointer is has to match where its own pixels are, or every hit test is
    // off by the toolbar; and the scale is divided out here, once, because
    // every answer the engine gives about this point -- a rect, a clientX -- is
    // in CSS pixels too.
    const gfx::IntPoint origin = PageOrigin();
    const float device_scale = window_.DeviceScale();
    const float scale = device_scale > 0.0f ? device_scale : 1.0f;
    message.position = gfx::FloatPoint{
        static_cast<float>(pointer->position.x - origin.x) / scale,
        static_cast<float>(pointer->position.y - origin.y) / scale};
    // The platform numbers buttons from one; the DOM numbers them from zero and
    // keeps a separate bitmask of what is still held.
    message.button = pointer->button > 0 ? static_cast<std::uint8_t>(pointer->button - 1) : 0;
    if (pointer->kind != platform::PointerEvent::Kind::Up && pointer->button > 0) {
      message.buttons = static_cast<std::uint16_t>(1u << message.button);
    }
    message.modifiers = ModifiersFor(pointer->modifiers);
    channel_.Ui().Send(message);
    return;
  }

  if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
    // Chrome or page, decided before the key becomes a message and never both.
    // ADR 0017 §4 puts this decision in `src/app`, and KeyRouting.h says why:
    // a page that could see a key aimed at the omnibox learns what is being
    // typed into the address bar, and one that could type into it controls
    // what the address bar says while showing its own content.
    //
    // "Not handled by the chrome" is not the same rule and used to be the one
    // in force here: it forwarded every key the chrome had no use for, so a
    // page saw most of what was typed into the omnibox.
    if (RouteKey(*key, chrome_.GetToolbar().IsOmniboxFocused()) == KeyDestination::Chrome) {
      ApplyChromeResponse(chrome_.HandleKey(*key));
      return;
    }
    // One message for every key that belongs to the page, whatever it is.
    // Deciding here which keys are "text" and which are "commands" is what the
    // message set this replaces did, and it is why a page could never learn
    // that Escape was pressed: the decision belongs to the page's own handlers,
    // and the engine's default action runs after them.
    channel_.Ui().Send(KeyMessageFor(*key));
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
        // The blit, before the damage is painted over it. The engine says this
        // frame is the previous one moved; copying the overlap within the
        // canvas is what makes a scroll cost the exposed strip rather than the
        // window (ADR 0018 §2). The delta is advisory and bounded inside
        // ScrollRegion, because after the process split it arrives from a
        // renderer -- and a blit driven by an unchecked offset is a read into
        // somebody else's memory.
        if (paint->scroll_delta != gfx::IntPoint{} && !full_repaint_pending_) {
          canvas_.ScrollRegion(chrome_.PageBounds(gfx::IntSize{canvas_.Width(),
                                                               canvas_.Height()}),
                               paint->scroll_delta.x, paint->scroll_delta.y);
          scroll_blitted_ = true;
        }
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
      window_.SetTitle(chrome_.WindowTitle());
      InvalidateChrome();
    } else if (const auto* history = std::get_if<ipc::HistoryStateMessage>(&*message)) {
      // Two bools, which is all the chrome knows about history since ADR 0026 §1
      // moved the list into the engine.
      chrome_.OnHistoryState(history->can_go_back, history->can_go_forward);
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

  // A blitted frame painted only its exposed band, and the texture still has to
  // learn about every pixel that slid: a streaming texture cannot be told its
  // contents moved. So the *paint* is partial and the *upload* is whole, which
  // is the split worth having -- rasterizing paths, glyphs and images is the
  // expensive half and a full-surface upload is a memcpy.
  if (!presenter_.Present(window_.Renderer(), canvas_, dirty_, full || scroll_blitted_)) {
    // The presenter dropped its texture. Do not clear the pending flags: the
    // next iteration must try again with a full repaint rather than leaving a
    // stale or empty window on screen.
    full_repaint_pending_ = true;
    return;
  }

  dirty_.Clear();
  repaint_pending_ = false;
  full_repaint_pending_ = false;
  scroll_blitted_ = false;
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
    case ui::BrowserChrome::Intent::Kind::TraverseHistory:
      channel_.Ui().Send(ipc::TraverseHistoryMessage{response.intent->delta});
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

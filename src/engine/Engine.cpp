#include "engine/Engine.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Placeholder page chrome. These become style-resolved colors the moment there
// is a stylesheet to resolve; until then they live here rather than in a
// "theme" abstraction that would have exactly one caller.
constexpr gfx::Color kPageBackground = gfx::Color::Rgb(0xFF, 0xFF, 0xFF);
constexpr gfx::Color kBandColor = gfx::Color::Rgb(0x1F, 0x6F, 0xEB);
constexpr gfx::Color kBlockColor = gfx::Color::Rgba(0x20, 0x20, 0x28, 0x30);
constexpr gfx::Color kBlockOutline = gfx::Color::Rgba(0x20, 0x20, 0x28, 0x60);
constexpr float kBlockRadius = 4.0f;

constexpr int kBandHeight = 48;
constexpr int kBlockHeight = 18;
constexpr int kBlockGap = 12;
constexpr int kPageMargin = 32;

}  // namespace

Engine::Engine(ipc::EngineEndpoint& endpoint) : endpoint_(endpoint) {}

bool Engine::HandlePendingMessages() {
  bool produced_output = false;

  while (std::optional<ipc::UiToEngine> message = endpoint_.TryReceive()) {
    if (const auto* navigate = std::get_if<ipc::NavigateMessage>(&*message)) {
      Navigate(navigate->url);
      produced_output = true;
    } else if (const auto* resize = std::get_if<ipc::ResizeViewportMessage>(&*message)) {
      SetViewport(resize->size, resize->device_scale);
      produced_output = true;
    } else if (const auto* scroll = std::get_if<ipc::ScrollMessage>(&*message)) {
      ScrollBy(scroll->delta_x, scroll->delta_y);
      produced_output = true;
    } else if (std::holds_alternative<ipc::ReloadMessage>(*message)) {
      Navigate(url_);
      produced_output = true;
    }
    // StopLoad and Pointer are accepted and ignored: there is nothing to stop
    // and nothing to hit-test yet. They are in the vocabulary now so the UI can
    // be written against the final shape.
  }

  return produced_output;
}

void Engine::Navigate(const std::string& url) {
  util::PerformanceTrace::Scope scope("engine::Navigate");
  AddPerformanceCounter(PerfCounterId::EngineNavigations);

  url_ = url;
  title_ = url.empty() ? std::string("New Tab") : url;
  scroll_y_ = 0;

  endpoint_.Send(ipc::NavigationCommittedMessage{url_});
  endpoint_.Send(ipc::TitleChangedMessage{title_});
  PaintAndSend();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::SetViewport(const gfx::IntSize& size, float device_scale) {
  if (size == viewport_size_ && device_scale == device_scale_) {
    return;
  }
  viewport_size_ = size;
  device_scale_ = device_scale;
  PaintAndSend();
}

void Engine::ScrollBy(int delta_x, int delta_y) {
  (void)delta_x;  // No horizontal overflow to scroll until there is layout.
  const int previous = scroll_y_;
  scroll_y_ = std::max(0, scroll_y_ + delta_y);
  if (scroll_y_ != previous) {
    PaintAndSend();
  }
}

void Engine::PaintAndSend() {
  util::PerformanceTrace::Scope scope("engine::Paint");
  AddPerformanceCounter(PerfCounterId::EnginePaintsProduced);
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);

  display_list_.Clear();

  const gfx::IntRect viewport{0, 0, viewport_size_.width, viewport_size_.height};
  if (viewport.IsEmpty()) {
    return;
  }

  display_list_.FillRect(viewport, kPageBackground);
  display_list_.FillRect(gfx::IntRect{0, -scroll_y_, viewport.width, kBandHeight}, kBandColor);

  // Stand-in content blocks: enough structure that scrolling, clipping, and
  // partial repaint are all visibly exercised before there is a real document.
  //
  // Rounded and outlined rather than plain rectangles so that the running
  // application exercises the path rasterizer and the stroker, not only the
  // tests. A pipeline that is only ever driven by its own test suite is a
  // pipeline with an untested last mile.
  display_list_.PushClip(gfx::IntRect{0, kBandHeight, viewport.width, viewport.height});
  int y = kBandHeight + kBlockGap - scroll_y_;
  while (y < viewport.height) {
    const float width = static_cast<float>(viewport.width - 2 * kPageMargin);
    const gfx::FloatRect block{static_cast<float>(kPageMargin), static_cast<float>(y), width,
                               static_cast<float>(kBlockHeight)};
    gfx::Path rounded;
    rounded.AddRoundedRect(block, kBlockRadius, kBlockRadius, kBlockRadius, kBlockRadius);
    display_list_.FillPath(rounded, kBlockColor);

    gfx::StrokeStyle outline;
    outline.width = 1.0f;
    outline.join = gfx::LineJoin::Round;
    display_list_.StrokePath(rounded, outline, kBlockOutline);
    y += kBlockHeight + kBlockGap;
  }
  display_list_.PopClip();

  ipc::PaintFrameMessage frame;
  frame.display_list = display_list_;
  // Empty damage means "the whole viewport". Correct at M0: with no layout
  // there is no way to know less, and claiming a narrower region would paint
  // over stale pixels.
  endpoint_.Send(std::move(frame));
}

}  // namespace microbrowser::engine

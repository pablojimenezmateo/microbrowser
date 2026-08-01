#include "engine/Engine.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "gfx/DisplayListDiff.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Escapes text for placement inside an HTML document.
//
// The error page is built by string concatenation, and the URL in it comes from
// whoever asked for the navigation. Interpolating it raw would make a URL
// containing markup a script injection into the browser's own error page --
// the classic self-XSS in the one document a browser writes itself.
std::string EscapeHtml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The blank document. Not an empty string: "" parses to a document with a body
// too, but saying it here means about:blank is a real page rather than a
// failure that happens to look like one.
constexpr std::string_view kBlankDocument =
    "<!DOCTYPE html><html><head><title>New Tab</title></head><body></body></html>";

}  // namespace

Engine::Engine(ipc::EngineEndpoint& endpoint, gfx::FontProvider& fonts)
    : endpoint_(endpoint), page_(fonts) {}

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
      Navigate(page_.Url());
      produced_output = true;
    }
    // StopLoad and Pointer are accepted and ignored: loading is synchronous so
    // there is nothing to stop, and nothing is hit-testable yet. They are in
    // the vocabulary now so the UI can be written against the final shape.
  }

  return produced_output;
}

void Engine::Navigate(const std::string& url) {
  util::PerformanceTrace::Scope scope("engine::Navigate");
  AddPerformanceCounter(PerfCounterId::EngineNavigations);

  scroll_y_ = 0;

  if (url.empty() || url == "about:blank") {
    page_.Load(kBlankDocument, url.empty() ? std::string("about:blank") : url);
  } else {
    // Synchronous, and that is a stated limitation rather than a design: the
    // loop blocks for the length of a load. Making it asynchronous is a change
    // to this function and the message vocabulary, not to the seam -- which is
    // why it can wait until there is something worth waiting on.
    const Loader::Result loaded = loader_.Load(url, NowSeconds());
    if (!loaded.ok) {
      ShowError(url, loaded.error == nullptr ? "the load failed" : loaded.error);
      return;
    }
    page_.Load(loaded.body, loaded.final_url.empty() ? url : loaded.final_url);
  }

  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::ShowError(std::string_view url, std::string_view message) {
  // A browser that shows nothing when a load fails is indistinguishable from a
  // browser that has hung.
  std::string html =
      "<!DOCTYPE html><html><head><title>Cannot load page</title>"
      "<style>body{margin:32px;font-family:sans-serif;color:#202028}"
      "h1{font-size:1.5em;color:#8b1a1a}code{font-family:monospace;color:#404050}</style>"
      "</head><body><h1>Cannot load this page</h1><p>";
  html += EscapeHtml(message);
  html += "</p><p><code>";
  html += EscapeHtml(url);
  html += "</code></p></body></html>";

  page_.Load(html, std::string(url));
  endpoint_.Send(ipc::NavigationCommittedMessage{page_.Url()});
  endpoint_.Send(ipc::TitleChangedMessage{page_.Title()});
  LayoutAndPaint();
  endpoint_.Send(ipc::LoadProgressMessage{1.0f});
}

void Engine::SetViewport(const gfx::IntSize& size, float device_scale) {
  if (size == viewport_size_ && device_scale == device_scale_) {
    return;
  }
  viewport_size_ = size;
  device_scale_ = device_scale;
  // A resize changes the containing block, so it relays out. This is the one
  // input that does.
  LayoutAndPaint();
}

int Engine::MaxScroll() const {
  return std::max(0, static_cast<int>(page_.ContentHeight()) - viewport_size_.height);
}

void Engine::ScrollBy(int delta_x, int delta_y) {
  (void)delta_x;  // No horizontal overflow yet: layout never exceeds the width.
  const int previous = scroll_y_;
  scroll_y_ = std::clamp(scroll_y_ + delta_y, 0, MaxScroll());
  if (scroll_y_ != previous) {
    // Paints without laying out. The geometry has not changed, and a scroll
    // that relaid out is the classic reason scrolling is slow.
    PaintAndSend();
  }
}

void Engine::LayoutAndPaint() {
  if (viewport_size_.width > 0) {
    page_.Layout(static_cast<float>(viewport_size_.width) / device_scale_);
    scroll_y_ = std::clamp(scroll_y_, 0, MaxScroll());
  }
  PaintAndSend();
}

void Engine::PaintAndSend() {
  util::PerformanceTrace::Scope scope("engine::Paint");
  AddPerformanceCounter(PerfCounterId::EnginePaintsProduced);
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);

  const gfx::IntRect viewport{0, 0, viewport_size_.width, viewport_size_.height};
  if (viewport.IsEmpty()) {
    return;
  }

  pending_.Clear();
  // The canvas behind the document, painted here rather than by the page: a
  // document shorter than the viewport still has a window under it, and the
  // page has no opinion about pixels it does not cover.
  pending_.FillRect(viewport, gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  page_.Paint(pending_, static_cast<float>(scroll_y_));

  gfx::DirtyRegion damage;
  const bool bounded = gfx::ComputeDamage(display_list_, pending_, viewport, damage);
  if (bounded && damage.IsEmpty()) {
    // Nothing on screen would change. Sending the frame anyway would make the
    // UI upload a texture to draw the same picture, which is most of what a
    // browser wastes power on.
    AddPerformanceCounter(PerfCounterId::EnginePaintsSkipped);
    return;
  }

  ipc::PaintFrameMessage frame;
  frame.display_list = pending_;
  // Empty damage means "the whole viewport", which is what the diff reports
  // when it cannot bound the change -- a clip moved, and every command after a
  // clip reads it as state.
  if (bounded) {
    frame.damage = damage.Rects();
  }
  display_list_ = pending_;
  endpoint_.Send(std::move(frame));
}

}  // namespace microbrowser::engine

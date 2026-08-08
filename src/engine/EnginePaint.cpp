#include "engine/Engine.h"

#include "engine/Clock.h"

#include <algorithm>
#include <cstdio>

#include "gfx/DisplayListDiff.h"
#include "util/Env.h"
#include "util/LoadTimeline.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

int Engine::ScrollY() const {
  return static_cast<int>(page_.ScrollOffsetY());
}

int Engine::MaxScroll() const {
  return std::max(0, static_cast<int>(page_.ContentHeight()) - viewport_size_.height);
}

void Engine::LayoutAndPaint() {
  const bool trace = util::EnvFlagEnabled("MICROBROWSER_LOAD_TURN_TRACE");
  if (trace) {
    std::fprintf(stderr, "[load] LayoutAndPaint enter\n");
    std::fflush(stderr);
  }
  // The instant this frame is at, set before anything restyles. Every restyle in a turn -- a hover, a
  // script, a resize -- must agree about what time it is, or two halves of one transition end up at
  // two different progresses on the same frame.
  page_.SetAnimationTime(NowMilliseconds());
  if (viewport_size_.width > 0) {
    if (trace) {
      std::fprintf(stderr, "[load] Layout enter\n");
      std::fflush(stderr);
    }
    page_.Layout(static_cast<float>(viewport_size_.width) / device_scale_);
    if (trace) {
      std::fprintf(stderr, "[load] Layout end\n");
      std::fflush(stderr);
    }
    page_.SetScrollOffsetY(static_cast<float>(std::clamp(ScrollY(), 0, MaxScroll())));
  }
  PaintAndSend();
  if (trace) {
    std::fprintf(stderr, "[load] LayoutAndPaint end\n");
    std::fflush(stderr);
  }
}

void Engine::PaintAndSend() { PaintAndSend(gfx::IntPoint{}, nullptr); }

void Engine::PaintAndSend(gfx::IntPoint scroll_delta, const gfx::IntRect* only) {
  util::PerformanceTrace::Scope scope("engine::Paint");
  // Every path that puts something on screen comes through here, so this is
  // *the* paint milestone. On a browser with no incremental rendering the first
  // one is the whole answer to "when did the user see anything" -- see ADR 0030
  // and TD-0008.
  util::LoadTimeline::Mark("paint");
  AddPerformanceCounter(PerfCounterId::EnginePaintsProduced);
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);

  const gfx::IntRect viewport{0, 0, viewport_size_.width, viewport_size_.height};
  if (viewport.IsEmpty()) {
    return;
  }

  // The frame's observation step, and the single place it happens: every path
  // that puts something on screen comes through here, so an observer cannot be
  // sampled twice for one frame or missed for another. ADR 0018 §5 -- the
  // sample is at the frame and never inside the scroll that caused it.
  //
  // A callback that ran may have moved the document, and a frame whose
  // geometry changed is no longer the previous frame shifted: the blit is off
  // the table and the damage goes back to being whatever the display-list diff
  // finds.
  if (page_.DeliverObservations(NowMilliseconds())) {
    scroll_delta = gfx::IntPoint{};
    only = nullptr;
  }
  // And the images that came within reach of the scrollport while all that was
  // happening. Here rather than beside the observers because it is not one: a
  // lazy image is a geometry test the browser performs, not a callback a page
  // registered, and it works on a page with no script at all.
  StartImageRequests();

  pending_.Clear();
  // The canvas behind the document, painted here rather than by the page: a
  // document shorter than the viewport still has a window under it, and the
  // page has no opinion about pixels it does not cover.
  pending_.FillRect(viewport, gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  page_.Paint(pending_);

  ipc::PaintFrameMessage frame;
  // A scroll knows its own damage and the diff cannot compute it: every command
  // in the list moved by the same amount, so the diff reports the whole viewport
  // and the browser repaints the window for a two-pixel wheel notch. This is
  // the one place the truth is cheaper to state than to derive. Likewise a box
  // that scrolled inside the page: what changed is its clip rectangle, and
  // nothing else.
  if (scroll_delta != gfx::IntPoint{}) {
    frame.damage = ScrollDamage(scroll_delta);
    frame.scroll_delta = scroll_delta;
  } else if (only != nullptr) {
    frame.damage.push_back(*only);
  } else {
    gfx::DirtyRegion damage;
    const bool bounded = gfx::ComputeDamage(display_list_, pending_, viewport, damage);
    if (bounded) {
      frame.damage = damage.Rects();
    }
    page_.AddVideoSurfaceDamage(pending_, frame.damage);
    if (bounded && frame.damage.empty()) {
      AddPerformanceCounter(PerfCounterId::EnginePaintsSkipped);
      return;
    }
  }
  if (scroll_delta != gfx::IntPoint{} || only != nullptr) {
    page_.AddVideoSurfaceDamage(pending_, frame.damage);
  }
  frame.display_list = pending_;
  display_list_ = pending_;
  endpoint_.Send(std::move(frame));
}

}  // namespace microbrowser::engine

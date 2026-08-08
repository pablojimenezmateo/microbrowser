// Renders one URL to a PPM file and exits. No window, no event loop.
//
// This exists because the highest-yield way to find a layout or paint bug in
// this project is to look at a rendered page, and the browser binary can only
// show one on a machine with a display server. It drives the *real* seam --
// ipc messages into engine::Engine, a display list back, gfx::Execute onto a
// Canvas -- rather than reaching into Page directly, so what it shows is what
// the browser would show.
//
// PPM rather than PNG because src/gfx has a decoder and no encoder, and adding
// one to make a debugging tool prettier is the wrong order to do work in.
// `pnmtopng` or ImageMagick converts it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <variant>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Engine.h"
#include "platform/DescriptorWait.h"
#include "util/WaitDescriptor.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"
#include "gfx/Surface.h"
#include "gfx/Painter.h"
#include "gfx/TextRenderer.h"
#include "ipc/InProcessTransport.h"
#include "platform/SystemFonts.h"
#include "util/Parse.h"
#include "util/Env.h"
#include "util/LoadTimeline.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "util/PerformanceCounters.h"
#include "util/TraceChannel.h"

namespace {

struct Options {
  std::string url;
  std::string output = "snapshot.ppm";
  int width = 1280;
  int height = 900;
  int scroll_y = 0;
  // Device pixels per CSS pixel. It decides which `srcset` candidate an <img>
  // loads, and it is a flag rather than a property of the machine because the
  // machine this runs on has no display at all.
  float device_scale = 1.0f;
  bool dump = false;
  // Scripts to run against the loaded page, in order, each printed with what
  // it evaluated to. `-eval` may be repeated.
  //
  // The reason it exists: a snapshot can show what a page *looks* like and had
  // no way to ask it anything. "Did the custom elements upgrade", "is that
  // response in the tree", "what is in this shadow root" are each one line of
  // JavaScript, and each of them used to cost an `fprintf` and a rebuild.
  std::vector<std::string> probes;
  // A click to deliver before the snapshot, in viewport pixels. Negative means
  // none -- 0,0 is a real point.
  int click_x = -1;
  int click_y = -1;
  // A pointer *move*, delivered before the click. Separate from the click
  // because they are different questions: a click asks what an element does and
  // a move asks what the cascade does about `:hover`, and a page can get the
  // second wrong while getting the first right. Negative means none.
  int hover_x = -1;
  int hover_y = -1;
  // Keys to deliver after the click, in order. Each is one press and release.
  // `-type` expands to one entry per character with the character as its text;
  // `-key` names a key and inserts nothing, which is how Escape and Enter
  // arrive. This is the whole of what a check phrased as an interaction needs,
  // and it is here because the browser needs a display and this does not.
  std::vector<microbrowser::ipc::KeyInputMessage> keys;
};

// The DOM's `key` for a character, which for a printable character is the
// character. Split from `text` for the reason ADR 0017 §1 splits them: a key
// with no text still has a name, and Escape is the case that matters.
microbrowser::ipc::KeyInputMessage TypedKey(std::string character) {
  microbrowser::ipc::KeyInputMessage key;
  key.key = character;
  key.text = std::move(character);
  return key;
}

microbrowser::ipc::KeyInputMessage NamedKey(std::string_view name) {
  microbrowser::ipc::KeyInputMessage key;
  key.key = std::string(name);
  return key;
}

const char* kUsage =
    "usage: microbrowser_snapshot <url> [-o out.ppm] [-w width] [-h height] [-y scroll]\n"
    "                            [-dpr ratio] [-hover x,y] [-click x,y] [-type text]\n"
    "                            [-key name] [-v]\n"
    "  -dpr    device pixels per CSS pixel: which srcset candidate an <img> picks\n"
    "  -hover  move the pointer there first: what `:hover` and `:active` do to the page\n"
    "  -click  deliver a click before the snapshot, to follow a link or submit a form\n"
    "  -type   type text into whatever the click focused; repeatable, in order\n"
    "  -key    press one named key -- Escape, Enter, Tab, ArrowDown; repeatable\n"
    "  -v      print every display list command: what was painted, where, in what colour\n";

// One line per command. The point of a dump rather than a pixel is that a rect
// of the right colour in the wrong place and a rect that was never recorded
// look identical in a screenshot and completely different here.
void DumpDisplayList(const microbrowser::gfx::DisplayList& list) {
  using namespace microbrowser::gfx;  // NOLINT(build/namespaces) -- a debug dump
  std::size_t index = 0;
  for (const DisplayCommand& command : list.Commands()) {
    std::fprintf(stderr, "  [%3zu] ", index++);
    if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
      std::fprintf(stderr, "FillRect   %d,%d %dx%d #%08X\n", fill->rect.x, fill->rect.y,
                   fill->rect.width, fill->rect.height, fill->color.argb);
    } else if (const auto* fill_path = std::get_if<FillPathCommand>(&command)) {
      const Path* path = list.PathAt(fill_path->path);
      const FloatRect bounds = path == nullptr ? FloatRect{} : path->ControlBounds();
      std::fprintf(stderr, "FillPath   %.1f,%.1f %.1fx%.1f #%08X\n",
                   static_cast<double>(bounds.x), static_cast<double>(bounds.y),
                   static_cast<double>(bounds.width), static_cast<double>(bounds.height),
                   fill_path->color.argb);
    } else if (const auto* transform = std::get_if<PushTransformCommand>(&command)) {
      // Printed as the matrix rather than as a name, because "the transform is
      // wrong" is almost always "the matrix is right and the origin is not", and
      // only the six numbers can tell those apart.
      const AffineTransform matrix = list.TransformAt(transform->matrix);
      std::fprintf(stderr, "PushXform  [%.3f %.3f %.3f %.3f %.1f %.1f]\n",
                   static_cast<double>(matrix.A()), static_cast<double>(matrix.B()),
                   static_cast<double>(matrix.C()), static_cast<double>(matrix.D()),
                   static_cast<double>(matrix.E()), static_cast<double>(matrix.F()));
    } else if (std::holds_alternative<PopTransformCommand>(command)) {
      std::fprintf(stderr, "PopXform\n");
    } else if (const auto* stroke = std::get_if<StrokePathCommand>(&command)) {
      const Path* path = list.PathAt(stroke->path);
      const FloatRect bounds = path == nullptr ? FloatRect{} : path->ControlBounds();
      std::fprintf(stderr, "StrokePath %.1f,%.1f %.1fx%.1f #%08X\n",
                   static_cast<double>(bounds.x), static_cast<double>(bounds.y),
                   static_cast<double>(bounds.width), static_cast<double>(bounds.height),
                   stroke->color.argb);
    } else if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
      const DisplayList::TextRun* run = list.TextAt(text->text);
      std::fprintf(stderr, "Text       %.1f,%.1f w=%.1f #%08X \"%s\"\n",
                   static_cast<double>(text->origin.x), static_cast<double>(text->origin.y),
                   static_cast<double>(run == nullptr ? 0.0f : run->advance), text->color.argb,
                   run == nullptr ? "" : run->text.c_str());
    } else if (const auto* image = std::get_if<DrawImageCommand>(&command)) {
      std::fprintf(stderr, "Image      %d,%d %dx%d\n", image->destination.x, image->destination.y,
                   image->destination.width, image->destination.height);
    } else if (std::get_if<PushClipCommand>(&command) != nullptr) {
      std::fprintf(stderr, "PushClip\n");
    } else {
      std::fprintf(stderr, "PopClip\n");
    }
  }
}

std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  if (text.empty()) {
    return std::nullopt;
  }
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
    if (value > 1 << 16) {
      return std::nullopt;
    }
  }
  return value;
}

bool ParseOptions(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    const auto value = [&]() -> std::string_view {
      return i + 1 < argc ? std::string_view{argv[++i]} : std::string_view{};
    };
    if (argument == "-o") {
      out.output = value();
    } else if (argument == "-w") {
      const std::optional<int> parsed = ParseInt(value());
      if (!parsed) return false;
      out.width = *parsed;
    } else if (argument == "-h") {
      const std::optional<int> parsed = ParseInt(value());
      if (!parsed) return false;
      out.height = *parsed;
    } else if (argument == "-hover" || argument == "-click") {
      const std::string_view text = value();
      const std::size_t comma = text.find(',');
      if (comma == std::string_view::npos) return false;
      const std::optional<int> x = ParseInt(text.substr(0, comma));
      const std::optional<int> y = ParseInt(text.substr(comma + 1));
      if (!x || !y) return false;
      int& into_x = argument == "-hover" ? out.hover_x : out.click_x;
      int& into_y = argument == "-hover" ? out.hover_y : out.click_y;
      into_x = *x;
      into_y = *y;
    } else if (argument == "-type") {
      const std::string_view text = value();
      if (text.empty()) return false;
      // One key per byte. ASCII only, which is what the window path delivers
      // too -- a multi-byte character would need a codepoint boundary walk and
      // there is nothing yet on the other end that would read it differently.
      for (const char c : text) {
        out.keys.push_back(TypedKey(std::string(1, c)));
      }
    } else if (argument == "-key") {
      const std::string_view name = value();
      if (name.empty()) return false;
      out.keys.push_back(NamedKey(name));
    } else if (argument == "-dpr") {
      const std::optional<float> parsed = microbrowser::util::ParseFloat(value());
      if (!parsed || !(*parsed > 0.0f) || *parsed > 8.0f) return false;
      out.device_scale = *parsed;
    } else if (argument == "-eval") {
      const std::string_view text = value();
      if (text.empty()) return false;
      out.probes.emplace_back(text);
    } else if (argument == "-v") {
      out.dump = true;
    } else if (argument == "-y") {
      const std::optional<int> parsed = ParseInt(value());
      if (!parsed) return false;
      out.scroll_y = *parsed;
    } else if (!argument.empty() && argument.front() == '-') {
      return false;
    } else {
      out.url = argument;
    }
  }
  return !out.url.empty() && out.width > 0 && out.height > 0;
}

bool WritePpm(const microbrowser::gfx::Canvas& canvas, const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  std::fprintf(file, "P6\n%d %d\n255\n", canvas.Width(), canvas.Height());
  std::vector<unsigned char> row(static_cast<std::size_t>(canvas.Width()) * 3);
  for (int y = 0; y < canvas.Height(); ++y) {
    const std::uint32_t* pixels = canvas.Row(y);
    for (int x = 0; x < canvas.Width(); ++x) {
      const std::uint32_t pixel = pixels[x];
      row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<unsigned char>((pixel >> 16) & 0xFF);
      row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<unsigned char>((pixel >> 8) & 0xFF);
      row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<unsigned char>(pixel & 0xFF);
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  return std::fclose(file) == 0;
}

// Turns the loop's crank until the navigation is finished.
//
// This is the whole of what a host has to do since ADR 0011, minus a window: it
// lets the engine make progress, and when the engine can make none it blocks on
// the sockets the engine says it is waiting for. There is no polling and no
// sleep, which is why this is a faithful stand-in for the real loop rather than
// a shortcut that only works because nothing else is happening.
void RunLoadToCompletion(microbrowser::engine::Engine& engine) {
  std::uint64_t turns = 0;
  const bool trace = microbrowser::util::EnvFlagEnabled("MICROBROWSER_LOAD_TURN_TRACE");
  while (engine.IsLoading()) {
    ++turns;
    const auto turn_started = std::chrono::steady_clock::now();
    if (trace) {
      // Print before Advance every time: a hang inside Advance never reaches
      // the after-line, and that missing pair is the diagnosis (TD-0013).
      std::fprintf(stderr, "[load] turn=%llu enter Advance\n",
                   static_cast<unsigned long long>(turns));
      std::fflush(stderr);
    }
    const bool advanced = engine.Advance();
    if (trace) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - turn_started)
                          .count();
      std::fprintf(stderr, "[load] turn=%llu Advance=%d HasRunnable=%d ms=%lld\n",
                   static_cast<unsigned long long>(turns), advanced ? 1 : 0,
                   engine.HasRunnableWork() ? 1 : 0, static_cast<long long>(ms));
      std::fflush(stderr);
    }
    if (advanced || engine.HasRunnableWork()) {
      continue;
    }
    // Due timers, animation frames and worker messages. The real loop runs this
    // beside Advance() and this one did not, which was two bugs rather than an
    // omission: a page that armed a timer during its load made `NextDeadlineMs`
    // answer zero, and with nothing here to consume it the wait returned
    // instantly and the loop **span** -- 376,522 turns and 768ms on
    // youtube.com's front page. And a snapshot showed a document whose timers
    // had never run, which is not the page the browser draws.
    if (engine.RunDueWork()) {
      if (trace && (turns <= 20ULL || (turns % 10000ULL) == 0ULL)) {
        std::fprintf(stderr, "[load] turn=%llu due_work\n",
                     static_cast<unsigned long long>(turns));
        std::fflush(stderr);
      }
      continue;
    }
    microbrowser::util::WaitDescriptorList descriptors;
    engine.AppendWaitDescriptors(descriptors);
    const std::optional<std::uint32_t> deadline = engine.NextDeadlineMs();
    if (trace && (turns % 50ULL) == 0ULL) {
      std::fprintf(stderr,
                   "[load] turn=%llu wait descriptors=%zu deadline=%s reason=%s\n",
                   static_cast<unsigned long long>(turns), descriptors.size(),
                   deadline.has_value() ? "yes" : "no", engine.LoadingReason().c_str());
      std::fflush(stderr);
    }
    if (descriptors.empty() && !deadline.has_value()) {
      break;  // nothing outstanding, nothing runnable, no timer: stuck
    }
    if (descriptors.empty()) {
      // A timer or animation frame is due later. Sleep until then rather than
      // spinning Advance/HasRunnableWork, which is how a page that armed rAF
      // during load burned a core at 99% with nothing on the wire (TD-0013).
      microbrowser::util::PerformanceTrace::Scope wait("wait::Deadline");
      microbrowser::platform::WaitOnDescriptors(
          descriptors, static_cast<std::int32_t>(*deadline));
      continue;
    }
    // Scoped under `wait::` rather than a module name, and that prefix is the
    // convention: a `wait::` row is time the loop spent *blocked*, not time it
    // spent working, so it must not be read as a hotspot. Without it a page
    // whose whole cost is round trips shows a summary that adds up to a tenth
    // of the wall clock and says nothing about the other nine.
    microbrowser::util::PerformanceTrace::Scope wait("wait::Network");
    // Never block forever: a decoder pipe that is open but quiet would hang the tool after
    // `video.play()` started a session (session 27). Cap matches the post-load drain budget.
    microbrowser::platform::WaitOnDescriptors(
        descriptors,
        deadline.has_value() ? static_cast<std::int32_t>(*deadline) : 200);
  }
  if (trace) {
    std::fprintf(stderr, "[load] finished after %llu turns\n",
                 static_cast<unsigned long long>(turns));
  }
  // A page's last script turn often registers a timer, MessageChannel task, or
  // `requestIdleCallback`; if `Advance()` returned true that same iteration,
  // `RunDueWork` never ran inside the load loop. Drain until idle.
  //
  // Cap is moderate: TimerQueue batches up to 64 host tasks per RunDue
  // (TD-0018), so a few hundred passes cover a large stamp. Finite so a
  // forever-posting channel cannot hang the snapshot tool.
  //
  // Sleep when the next deadline is in the future. A tight `RunDueWork`-only
  // loop finishes in a few milliseconds and sees every pending rAF as "not
  // due yet" (16ms frame spacing), then breaks — which left youtube's lazy
  // list stuck at `initialCount` (Ot → rAF → tryRenderChunk_ → rAF…) even
  // after Polymer.dom.children and BeginTask-on-rAF were fixed.
  //
  // Wall-clock budget: youtube's stamp can keep `RunDueWork` returning true
  // for hundreds of passes (each a layout), which turned a 40s snapshot into
  // six minutes. Autofill that needs more than a few seconds of post-load
  // time is still owed frames; the interactive loop keeps going.
  const auto drain_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  for (int pass = 0; pass < 512; ++pass) {
    if (std::chrono::steady_clock::now() >= drain_deadline) {
      break;
    }
    if (engine.RunDueWork()) {
      continue;
    }
    const std::optional<std::uint32_t> deadline = engine.NextDeadlineMs();
    if (!deadline.has_value()) {
      break;
    }
    microbrowser::util::WaitDescriptorList descriptors;
    microbrowser::util::PerformanceTrace::Scope wait("wait::Deadline");
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               drain_deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
      break;
    }
    const std::int32_t wait_ms =
        static_cast<std::int32_t>(std::min<std::int64_t>(
            remaining, static_cast<std::int64_t>(*deadline)));
    microbrowser::platform::WaitOnDescriptors(descriptors, wait_ms);
  }
  // Stamp finishes on rAF; IntersectionObservers sample on paint. Schedule one
  // frame so youtube's lazy imgs can assign `src` after the last stamp chunk,
  // then keep turning until idle so the fetches that assignment starts can land.
  (void)engine.EvaluateScript("requestAnimationFrame(() => {});");
  for (int pass = 0; pass < 64; ++pass) {
    if (std::chrono::steady_clock::now() >= drain_deadline) {
      break;
    }
    if (engine.RunDueWork()) {
      continue;
    }
    const std::optional<std::uint32_t> deadline = engine.NextDeadlineMs();
    if (!deadline.has_value()) {
      // Still waiting on sockets the last frame opened (thumbnail `src`s).
      microbrowser::util::WaitDescriptorList descriptors;
      engine.AppendWaitDescriptors(descriptors);
      if (descriptors.empty()) {
        break;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 drain_deadline - std::chrono::steady_clock::now())
                                 .count();
      if (remaining <= 0) {
        break;
      }
      microbrowser::platform::WaitOnDescriptors(
          descriptors, static_cast<std::int32_t>(std::min<std::int64_t>(remaining, 50)));
      continue;
    }
    microbrowser::util::WaitDescriptorList descriptors;
    engine.AppendWaitDescriptors(descriptors);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               drain_deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
      break;
    }
    microbrowser::platform::WaitOnDescriptors(
        descriptors,
        static_cast<std::int32_t>(std::min<std::int64_t>(
            remaining, static_cast<std::int64_t>(*deadline))));
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Same as the browser's main(): identify the thread whose latency a user
  // feels before anything can record a scope, or every ranked summary is
  // misleading in the same direction.
  microbrowser::util::MarkTracingMainThread();

  Options options;
  if (!ParseOptions(argc, argv, options)) {
    std::fputs(kUsage, stderr);
    return 2;
  }

  microbrowser::gfx::FontLibrary font_library;
  microbrowser::platform::SystemFontProvider fonts{font_library};
  fonts.Scan();
  microbrowser::gfx::TextRenderer text{fonts};

  microbrowser::ipc::InProcessChannel channel;
  microbrowser::engine::Engine engine{channel.Engine(), fonts};

  channel.Ui().Send(microbrowser::ipc::ResizeViewportMessage{
      microbrowser::gfx::IntSize{options.width, options.height}, options.device_scale});
  channel.Ui().Send(microbrowser::ipc::NavigateMessage{options.url});
  engine.HandlePendingMessages();
  RunLoadToCompletion(engine);
  if (options.hover_x >= 0 && options.hover_y >= 0) {
    microbrowser::ipc::PointerInputMessage pointer;
    pointer.kind = microbrowser::ipc::PointerInputMessage::Kind::Move;
    pointer.position = microbrowser::gfx::FloatPoint{static_cast<float>(options.hover_x),
                                                     static_cast<float>(options.hover_y)};
    channel.Ui().Send(pointer);
    engine.HandlePendingMessages();
    RunLoadToCompletion(engine);
  }
  if (options.click_x >= 0 && options.click_y >= 0) {
    // Down then up, the way a real click arrives, so the engine sees the same
    // sequence the window would deliver.
    for (const auto kind : {microbrowser::ipc::PointerInputMessage::Kind::Down,
                            microbrowser::ipc::PointerInputMessage::Kind::Up}) {
      microbrowser::ipc::PointerInputMessage pointer;
      pointer.kind = kind;
      pointer.position = microbrowser::gfx::FloatPoint{static_cast<float>(options.click_x),
                                                       static_cast<float>(options.click_y)};
      pointer.buttons = kind == microbrowser::ipc::PointerInputMessage::Kind::Down ? 1 : 0;
      channel.Ui().Send(pointer);
    }
    engine.HandlePendingMessages();
    RunLoadToCompletion(engine);
  }
  for (microbrowser::ipc::KeyInputMessage key : options.keys) {
    // Down then up, the way a real key arrives. A handler that runs on keyup
    // and a default action that runs on keydown are both real, and delivering
    // only the press would test half of the path.
    key.kind = microbrowser::ipc::KeyInputMessage::Kind::Down;
    channel.Ui().Send(key);
    key.kind = microbrowser::ipc::KeyInputMessage::Kind::Up;
    channel.Ui().Send(key);
    engine.HandlePendingMessages();
    RunLoadToCompletion(engine);
  }
  if (options.scroll_y > 0) {
    channel.Ui().Send(
        microbrowser::ipc::ScrollMessage{0, options.scroll_y, microbrowser::gfx::IntPoint{}});
    engine.HandlePendingMessages();
    // And then turn the crank, like the click and the key above already do. A
    // scroll can start a fetch now -- an `<img loading="lazy">` that came
    // within reach of the scrollport -- and a snapshot that stopped here would
    // write out the frame from *before* the image arrived, which looks exactly
    // like a lazy loader that does not work.
    RunLoadToCompletion(engine);
  }

  // The probes, after every input has been delivered and the page has settled,
  // so they describe the page the snapshot is about to write out. Before the
  // frame is taken rather than after, because a probe that changes the document
  // should show up in it.
  for (const std::string& probe : options.probes) {
    const std::string answer = engine.EvaluateScript(probe);
    std::printf("eval: %s\n", answer.c_str());
  }
  if (!options.probes.empty()) {
    RunLoadToCompletion(engine);
  }

  // Keep the last frame. A navigation sends more than one -- resize, then load
  // -- and the last is the finished page.
  microbrowser::gfx::DisplayList display_list;
  std::string title;
  std::string url = options.url;
  bool painted = false;
  while (std::optional<microbrowser::ipc::EngineToUi> message = channel.Ui().TryReceive()) {
    if (auto* paint = std::get_if<microbrowser::ipc::PaintFrameMessage>(&*message)) {
      // MICROBROWSER_TRACE_REDRAW=1 means the same thing here as it does in the
      // browser -- what changed, and how much of the window that is -- except
      // that the browser can only be asked from a machine with a display and
      // this cannot. A scroll's damage is the whole point of ADR 0018, and a
      // check for it that needs a window is one nobody runs.
      if (microbrowser::util::PerformanceTrace::FlagEnabled("MICROBROWSER_TRACE_REDRAW")) {
        long long covered = 0;
        for (const microbrowser::gfx::IntRect& rect : paint->damage) {
          covered += static_cast<long long>(rect.width) * rect.height;
        }
        const long long surface =
            static_cast<long long>(options.width) * options.height;
        std::fprintf(stderr,
                     "[redraw] %-7s rects=%zu coverage=%5.1f%% surface=%dx%d "
                     "scroll=%d,%d commands=%zu\n",
                     paint->damage.empty() ? "full" : "partial", paint->damage.size(),
                     surface > 0 ? static_cast<double>(covered) * 100.0 /
                                       static_cast<double>(surface)
                                 : 0.0,
                     options.width, options.height, paint->scroll_delta.x,
                     paint->scroll_delta.y, paint->display_list.Size());
      }
      display_list = std::move(paint->display_list);
      painted = true;
    } else if (auto* changed = std::get_if<microbrowser::ipc::TitleChangedMessage>(&*message)) {
      title = changed->title;
    } else if (auto* committed =
                   std::get_if<microbrowser::ipc::NavigationCommittedMessage>(&*message)) {
      url = committed->url;
    }
  }
  if (!painted) {
    std::fputs("the engine produced no frame\n", stderr);
    return 1;
  }

  microbrowser::gfx::Canvas canvas{options.width, options.height};
  canvas.Clear(microbrowser::gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  microbrowser::gfx::Painter painter{canvas};
  microbrowser::gfx::Execute(display_list, painter, canvas.Bounds(), &text);
  microbrowser::gfx::CompositeSurfaces(canvas, display_list, engine.VideoSurfaces());

  if (!WritePpm(canvas, options.output)) {
    std::fprintf(stderr, "could not write %s\n", options.output.c_str());
    return 1;
  }
  // MICROBROWSER_PERF_SUMMARY=1 and MICROBROWSER_STARTUP_SUMMARY=1 work here
  // exactly as they do in the browser. Without this they read as "no scopes
  // ran", which is the wrong answer to a question about where the time went.
  microbrowser::util::LoadTimeline::DumpOnce(stderr);
  microbrowser::util::PerformanceTrace::DumpSummaryOnce();
  microbrowser::util::StartupTrace::DumpSummaryOnce();
  microbrowser::util::DumpPerformanceCountersOnce();
  std::fprintf(stderr, "%s: %zu commands, %zu runs, %zu fonts, %zu images, title \"%s\" -> %s\n",
               url.c_str(), display_list.Size(), display_list.Texts().size(),
               display_list.Fonts().size(), display_list.Images().size(), title.c_str(),
               options.output.c_str());
  // Always, not behind -v. A script that threw is the most likely reason a
  // page rendered less than it should have, and a debugging tool that makes
  // you pass a flag to learn that is one you find out about too late.
  // What the page said, not only what it threw. A page's own `console.log` is how it
  // reports on itself, and half the questions asked of this tool -- did storage answer,
  // did the fetch land -- are answered by a line the page already prints.
  for (const std::string& line : engine.ConsoleOutput()) {
    std::fprintf(stderr, "  console: %s\n", line.c_str());
  }
  for (const std::string& error : engine.ScriptErrors()) {
    std::fprintf(stderr, "  script error: %s\n", error.c_str());
  }
  // Only when something was driven at the page, and then always -- for the
  // reason above. Every check from ADR 0017 on is phrased as an interaction,
  // and where a click sent focus decides where every key after it goes. A
  // click that focused the wrong thing renders identically to one that worked.
  if (!options.keys.empty() || (options.click_x >= 0 && options.click_y >= 0)) {
    std::fprintf(stderr, "  focus: %s\n", engine.FocusDescription().c_str());
  }
  if (options.dump) {
    DumpDisplayList(display_list);
  }
  for (const microbrowser::gfx::FontRequest& font : display_list.Fonts()) {
    std::string families;
    for (const std::string& family : font.families) {
      families += families.empty() ? "" : ", ";
      families += family;
    }
    std::fprintf(stderr, "  font: family=\"%s\" size=%.1f weight=%d italic=%d -> %s\n",
                 families.c_str(), static_cast<double>(font.size), font.weight,
                 font.italic ? 1 : 0, fonts.FontFor(font) == nullptr ? "MISSING" : "ok");
  }
  return 0;
}

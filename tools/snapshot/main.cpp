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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
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
#include "gfx/Painter.h"
#include "gfx/TextRenderer.h"
#include "ipc/InProcessTransport.h"
#include "platform/SystemFonts.h"
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
  bool dump = false;
  // A click to deliver before the snapshot, in viewport pixels. Negative means
  // none -- 0,0 is a real point.
  int click_x = -1;
  int click_y = -1;
};

const char* kUsage =
    "usage: microbrowser_snapshot <url> [-o out.ppm] [-w width] [-h height] [-y scroll]\n"
    "                            [-click x,y] [-v]\n"
    "  -click  deliver a click before the snapshot, to follow a link or submit a form\n"
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
    } else if (argument == "-click") {
      const std::string_view text = value();
      const std::size_t comma = text.find(',');
      if (comma == std::string_view::npos) return false;
      const std::optional<int> x = ParseInt(text.substr(0, comma));
      const std::optional<int> y = ParseInt(text.substr(comma + 1));
      if (!x || !y) return false;
      out.click_x = *x;
      out.click_y = *y;
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
  while (engine.IsLoading()) {
    if (engine.Advance() || engine.HasRunnableWork()) {
      continue;
    }
    microbrowser::util::WaitDescriptorList descriptors;
    engine.AppendWaitDescriptors(descriptors);
    if (descriptors.empty()) {
      break;  // nothing outstanding and nothing runnable: the load is stuck
    }
    const std::optional<std::uint32_t> deadline = engine.NextDeadlineMs();
    microbrowser::platform::WaitOnDescriptors(
        descriptors,
        deadline.has_value() ? static_cast<std::int32_t>(*deadline) : -1);
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
      microbrowser::gfx::IntSize{options.width, options.height}, 1.0f});
  channel.Ui().Send(microbrowser::ipc::NavigateMessage{options.url});
  engine.HandlePendingMessages();
  RunLoadToCompletion(engine);
  if (options.click_x >= 0 && options.click_y >= 0) {
    // Down then up, the way a real click arrives, so the engine sees the same
    // sequence the window would deliver.
    for (const auto kind : {microbrowser::ipc::PointerMessage::Kind::Down,
                            microbrowser::ipc::PointerMessage::Kind::Up}) {
      channel.Ui().Send(microbrowser::ipc::PointerMessage{
          kind, microbrowser::gfx::IntPoint{options.click_x, options.click_y}, 1});
    }
    engine.HandlePendingMessages();
    RunLoadToCompletion(engine);
  }
  if (options.scroll_y > 0) {
    channel.Ui().Send(microbrowser::ipc::ScrollMessage{0, options.scroll_y});
    engine.HandlePendingMessages();
  }

  // Keep the last frame. A navigation sends more than one -- resize, then load
  // -- and the last is the finished page.
  microbrowser::gfx::DisplayList display_list;
  std::string title;
  std::string url = options.url;
  bool painted = false;
  while (std::optional<microbrowser::ipc::EngineToUi> message = channel.Ui().TryReceive()) {
    if (auto* paint = std::get_if<microbrowser::ipc::PaintFrameMessage>(&*message)) {
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

  if (!WritePpm(canvas, options.output)) {
    std::fprintf(stderr, "could not write %s\n", options.output.c_str());
    return 1;
  }
  // MICROBROWSER_PERF_SUMMARY=1 and MICROBROWSER_STARTUP_SUMMARY=1 work here
  // exactly as they do in the browser. Without this they read as "no scopes
  // ran", which is the wrong answer to a question about where the time went.
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
  for (const std::string& error : engine.ScriptErrors()) {
    std::fprintf(stderr, "  script error: %s\n", error.c_str());
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

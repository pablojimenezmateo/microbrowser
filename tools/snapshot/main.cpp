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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Engine.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"
#include "gfx/Painter.h"
#include "gfx/TextRenderer.h"
#include "ipc/InProcessTransport.h"
#include "platform/SystemFonts.h"

namespace {

struct Options {
  std::string url;
  std::string output = "snapshot.ppm";
  int width = 1280;
  int height = 900;
  int scroll_y = 0;
};

const char* kUsage =
    "usage: microbrowser_snapshot <url> [-o out.ppm] [-w width] [-h height] [-y scroll]\n";

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

}  // namespace

int main(int argc, char** argv) {
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
  if (options.scroll_y > 0) {
    channel.Ui().Send(microbrowser::ipc::ScrollMessage{0, options.scroll_y});
    engine.HandlePendingMessages();
  }

  // Keep the last frame. A navigation sends more than one -- resize, then load
  // -- and the last is the finished page.
  microbrowser::gfx::DisplayList display_list;
  std::string title;
  bool painted = false;
  while (std::optional<microbrowser::ipc::EngineToUi> message = channel.Ui().TryReceive()) {
    if (auto* paint = std::get_if<microbrowser::ipc::PaintFrameMessage>(&*message)) {
      display_list = std::move(paint->display_list);
      painted = true;
    } else if (auto* changed = std::get_if<microbrowser::ipc::TitleChangedMessage>(&*message)) {
      title = changed->title;
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
  std::fprintf(stderr, "%s: %zu commands, %zu runs, %zu fonts, %zu images, title \"%s\" -> %s\n",
               options.url.c_str(), display_list.Size(), display_list.Texts().size(),
               display_list.Fonts().size(), display_list.Images().size(), title.c_str(),
               options.output.c_str());
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

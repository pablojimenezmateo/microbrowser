#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "gfx/Canvas.h"
#include "gfx/FontCatalog.h"
#include "gfx/Painter.h"
#include "gfx/TextRenderer.h"
#include "support/SyntheticFont.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/ReferenceImage.h"

namespace microbrowser::tests {

namespace {

using gfx::Canvas;
using gfx::Painter;

// Everything between "the engine decided what the page looks like" and "there
// are pixels", with the serialization step in the middle actually taken.
//
// The unit tests above each cover one link of this chain, and a chain of tested
// links is not a tested chain. This is also the only test that would notice the
// engine emitting a command the wire format cannot carry, which is precisely
// the failure the IPC seam exists to make impossible before the process split.
struct RenderedFrames {
  Canvas canvas;
  int frames = 0;
};

// A page with a background, a border and text on it: enough that a golden of
// the result would notice any of the three going missing.
//
// A `data:` URL rather than a server, because this test is about the chain from
// engine to pixels and a socket in the middle of it would be testing something
// else.
constexpr std::string_view kFixturePage =
    "data:text/html,<html><body style='background-color:%23ffffff'>"
    "<h1 style='background-color:%231f6feb;color:%23ffffff'>ABCD</h1>"
    "<p style='border:2px solid %23333333'>ABC ABCD ABC</p></body></html>";

RenderedFrames RenderEngineOutput(int width, int height) {
  RenderedFrames result;
  result.canvas.Resize(width, height);
  result.canvas.Clear(gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  Painter painter(result.canvas);

  // The synthetic font, not the system's. A golden rendered with whichever
  // version of DejaVu the machine ships would be a golden of the machine.
  gfx::FontLibrary library;
  gfx::FontCatalog fonts(library);
  Expect(fonts.Register("Test", 400, false, BuildSyntheticFont()), "the test font registered");
  Expect(fonts.Register("Test", 700, false, BuildSyntheticFont()), "and a bold to match <h1>");
  fonts.SetDefaultFamily("Test");
  fonts.SetGenericFamily("sans-serif", "Test");
  fonts.SetGenericFamily("monospace", "Test");
  gfx::TextRenderer text(fonts);

  ipc::InProcessChannel channel;
  engine::Engine engine(channel.Engine(), fonts);
  channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{width, height}, 1.0f});
  channel.Ui().Send(ipc::NavigateMessage{std::string(kFixturePage)});
  engine.HandlePendingMessages();

  while (const auto message = channel.Ui().TryReceive()) {
    const auto* paint = std::get_if<ipc::PaintFrameMessage>(&*message);
    if (paint == nullptr) {
      continue;
    }
    const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(ipc::EngineToUi{*paint}));
    Expect(decoded.has_value(), "a frame the engine produced must survive its own wire format");
    gfx::Execute(std::get<ipc::PaintFrameMessage>(*decoded).display_list, painter,
                 result.canvas.Bounds(), &text);
    ++result.frames;
  }
  return result;
}

}  // namespace

void RegisterPaintPipelineTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PaintPipeline/EngineOutputSurvivesTheWireAndReachesPixels", [] {
    const RenderedFrames rendered = RenderEngineOutput(320, 200);
    Expect(rendered.frames > 0, "navigating must produce at least one painted frame");

    // At least two distinct colors must have landed, or the whole chain could
    // be painting a uniform surface and every assertion above it would still
    // hold.
    const std::uint32_t first = rendered.canvas.Row(0)[0];
    std::size_t distinct = 0;
    for (const std::uint32_t pixel : rendered.canvas.Pixels()) {
      if (pixel != first) {
        ++distinct;
      }
    }
    Expect(distinct > 0, "the frame must contain content, not just a cleared surface");
  });

  AddTest(tests, "PaintPipeline/Golden/EngineFrame", [] {
    const RenderedFrames rendered = RenderEngineOutput(320, 200);
    const ComparisonResult result = CompareAgainstGolden(rendered.canvas, "pipeline/engine-frame");
    Expect(result.matches, result.message);
  });
}

}  // namespace microbrowser::tests

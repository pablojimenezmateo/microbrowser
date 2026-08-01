#include <variant>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "gfx/Canvas.h"
#include "gfx/Painter.h"
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

RenderedFrames RenderEngineOutput(int width, int height) {
  RenderedFrames result;
  result.canvas.Resize(width, height);
  result.canvas.Clear(gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
  Painter painter(result.canvas);

  ipc::InProcessChannel channel;
  engine::Engine engine(channel.Engine());
  channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{width, height}, 1.0f});
  channel.Ui().Send(ipc::NavigateMessage{"about:blank"});
  engine.HandlePendingMessages();

  while (const auto message = channel.Ui().TryReceive()) {
    const auto* paint = std::get_if<ipc::PaintFrameMessage>(&*message);
    if (paint == nullptr) {
      continue;
    }
    const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(ipc::EngineToUi{*paint}));
    Expect(decoded.has_value(), "a frame the engine produced must survive its own wire format");
    gfx::Execute(std::get<ipc::PaintFrameMessage>(*decoded).display_list, painter,
                 result.canvas.Bounds());
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

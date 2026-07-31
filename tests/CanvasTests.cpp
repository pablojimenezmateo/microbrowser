#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::IntRect;

namespace {

std::uint32_t PixelAt(const Canvas& canvas, int x, int y) {
  return canvas.Row(y)[x];
}

}  // namespace

void RegisterCanvasTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Canvas/ClearWritesEveryPixel", [] {
    Canvas canvas(4, 3);
    canvas.Clear(Color::Rgb(0x11, 0x22, 0x33));
    for (int y = 0; y < 3; ++y) {
      for (int x = 0; x < 4; ++x) {
        ExpectEqInt(PixelAt(canvas, x, y), 0xFF112233, "Clear must write every pixel");
      }
    }
  });

  AddTest(tests, "Canvas/FillRectIsClippedToBounds", [] {
    Canvas canvas(4, 4);
    canvas.Clear(Color::Rgb(0, 0, 0));
    // Deliberately overhangs on all four sides.
    canvas.FillRect(IntRect{-2, -2, 8, 8}, Color::Rgb(0xFF, 0, 0));
    ExpectEqInt(PixelAt(canvas, 0, 0), 0xFFFF0000, "in-bounds pixel must be filled");
    ExpectEqInt(PixelAt(canvas, 3, 3), 0xFFFF0000, "opposite corner must be filled");
  });

  AddTest(tests, "Canvas/ClipStackOnlyEverShrinks", [] {
    Canvas canvas(8, 8);
    canvas.Clear(Color::Rgb(0, 0, 0));
    canvas.PushClip(IntRect{2, 2, 4, 4});
    // A child clip that tries to escape its parent must not succeed.
    canvas.PushClip(IntRect{0, 0, 8, 8});
    canvas.FillRect(canvas.Bounds(), Color::Rgb(0, 0xFF, 0));

    ExpectEqInt(PixelAt(canvas, 0, 0), 0xFF000000, "pixel outside the parent clip must survive");
    ExpectEqInt(PixelAt(canvas, 3, 3), 0xFF00FF00, "pixel inside both clips must be painted");
    ExpectEqInt(PixelAt(canvas, 7, 7), 0xFF000000, "pixel outside the parent clip must survive");

    canvas.PopClip();
    canvas.PopClip();
    Expect(canvas.Clip() == canvas.Bounds(), "popping every clip restores full bounds");
  });

  AddTest(tests, "Canvas/PopClipOnEmptyStackIsSafe", [] {
    Canvas canvas(2, 2);
    canvas.PopClip();
    canvas.PopClip();
    Expect(canvas.Clip() == canvas.Bounds(), "an unbalanced pop must not corrupt the clip");
  });

  AddTest(tests, "Canvas/OpaqueFillReplacesRatherThanBlends", [] {
    Canvas canvas(2, 2);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    canvas.FillRect(canvas.Bounds(), Color::Rgb(0, 0, 0));
    ExpectEqInt(PixelAt(canvas, 0, 0), 0xFF000000, "an opaque fill must not blend");
  });

  AddTest(tests, "Canvas/HalfAlphaBlendIsExactlyMidway", [] {
    Canvas canvas(1, 1);
    canvas.Clear(Color::Rgb(0, 0, 0));
    // 128/255 over black gives round(255 * 128/255) = 128 on each channel.
    canvas.FillRect(canvas.Bounds(), Color::Rgba(0xFF, 0xFF, 0xFF, 0x80));
    ExpectEqInt(PixelAt(canvas, 0, 0), 0xFF808080,
                "src-over must round exactly, not truncate with >> 8");
  });

  AddTest(tests, "Canvas/FullyTransparentFillIsANoOp", [] {
    Canvas canvas(1, 1);
    canvas.Clear(Color::Rgb(0x12, 0x34, 0x56));
    canvas.FillRect(canvas.Bounds(), Color::Rgba(0xFF, 0, 0, 0));
    ExpectEqInt(PixelAt(canvas, 0, 0), 0xFF123456, "zero alpha must change nothing");
  });

  AddTest(tests, "Canvas/ResizeToZeroIsSafe", [] {
    Canvas canvas(4, 4);
    canvas.Resize(0, 0);
    Expect(canvas.IsEmpty(), "a zero-size canvas must report empty");
    Expect(canvas.Row(0) == nullptr, "row access on an empty canvas must return nullptr");
    canvas.FillRect(IntRect{0, 0, 10, 10}, Color::Rgb(0xFF, 0, 0));
    canvas.Clear(Color::Rgb(0, 0, 0));
  });

  AddTest(tests, "Canvas/ResizeToNegativeClampsToEmpty", [] {
    Canvas canvas(4, 4);
    canvas.Resize(-5, 10);
    Expect(canvas.IsEmpty(), "a negative extent must clamp to empty, not allocate wildly");
  });

  AddTest(tests, "Canvas/StrideMatchesWidth", [] {
    Canvas canvas(7, 2);
    ExpectEqInt(static_cast<long long>(canvas.StrideBytes()), 28,
                "pixels are tightly packed: stride is width * 4");
  });
}

}  // namespace microbrowser::tests

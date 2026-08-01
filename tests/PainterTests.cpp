#include <cmath>
#include <cstdint>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "support/ReferenceImage.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::FillRule;
using gfx::FloatPoint;
using gfx::FloatRect;
using gfx::IntRect;
using gfx::Painter;
using gfx::Path;

namespace {

constexpr Color kWhite = Color::Rgb(0xFF, 0xFF, 0xFF);
constexpr Color kBlack = Color::Rgb(0x00, 0x00, 0x00);
constexpr Color kBlue = Color::Rgb(0x1F, 0x6F, 0xEB);

int Alpha(std::uint32_t pixel) {
  return static_cast<int>((pixel >> 24) & 0xFFu);
}

int Red(std::uint32_t pixel) {
  return static_cast<int>((pixel >> 16) & 0xFFu);
}

}  // namespace

void RegisterPainterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Painter/FillsAnAlignedRectExactly", [] {
    Canvas canvas(8, 8);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    painter.FillRect(FloatRect{2.0f, 2.0f, 4.0f, 4.0f}, kBlack);

    ExpectEqInt(Red(canvas.Row(2)[2]), 0x00, "the top-left pixel of the rect is filled");
    ExpectEqInt(Red(canvas.Row(5)[5]), 0x00, "the bottom-right pixel of the rect is filled");
    ExpectEqInt(Red(canvas.Row(1)[2]), 0xFF, "the pixel above is untouched");
    ExpectEqInt(Red(canvas.Row(6)[2]), 0xFF, "the pixel below is untouched");
    ExpectEqInt(Red(canvas.Row(2)[1]), 0xFF, "the pixel left is untouched");
    ExpectEqInt(Red(canvas.Row(2)[6]), 0xFF, "the pixel right is untouched");
  });

  AddTest(tests, "Painter/FractionalEdgesAreBlendedRatherThanSnapped", [] {
    Canvas canvas(8, 8);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    painter.FillRect(FloatRect{2.5f, 2.0f, 3.0f, 3.0f}, kBlack);

    ExpectEqInt(Red(canvas.Row(2)[2]), 127,
                "a half-covered pixel is half-blended; snapping it here is what produces the "
                "classic one-pixel-heavier border. 127 rather than 128 because source-over with "
                "alpha 128 keeps 127/255 of the destination");
    ExpectEqInt(Red(canvas.Row(2)[3]), 0x00, "the fully covered interior is solid");
    ExpectEqInt(Red(canvas.Row(2)[5]), 127, "and the far edge splits the same way");
  });

  AddTest(tests, "Painter/HonorsTheCanvasClip", [] {
    Canvas canvas(8, 8);
    canvas.Clear(kWhite);
    canvas.PushClip(IntRect{4, 0, 4, 8});
    Painter painter(canvas);
    painter.FillRect(FloatRect{0.0f, 0.0f, 8.0f, 8.0f}, kBlack);
    canvas.PopClip();

    ExpectEqInt(Red(canvas.Row(0)[3]), 0xFF, "outside the clip is untouched");
    ExpectEqInt(Red(canvas.Row(0)[4]), 0x00, "inside the clip is filled");
  });

  AddTest(tests, "Painter/ATransparentColorDrawsNothing", [] {
    Canvas canvas(4, 4);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    painter.FillRect(FloatRect{0.0f, 0.0f, 4.0f, 4.0f}, Color::Transparent());
    ExpectEqInt(Red(canvas.Row(0)[0]), 0xFF, "a fully transparent fill is a no-op");
  });

  AddTest(tests, "Painter/CoverageAndSourceAlphaMultiply", [] {
    Canvas canvas(4, 4);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    // Half-covered pixel, half-opaque color: a quarter of the destination is
    // replaced. Getting this wrong shows up only where the two multiply.
    painter.FillRect(FloatRect{0.0f, 0.0f, 0.5f, 1.0f}, kBlack.WithAlpha(0x80));
    ExpectEqInt(Red(canvas.Row(0)[0]), 0xBF,
                "coverage 128 times alpha 128 is an effective alpha of 64, leaving 191/255 of "
                "the white destination");
    ExpectEqInt(Alpha(canvas.Row(0)[0]), 0xFF, "an opaque destination stays opaque");
  });

  AddTest(tests, "Painter/FillingOffCanvasWritesNothing", [] {
    Canvas canvas(4, 4);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    painter.FillRect(FloatRect{-100.0f, -100.0f, 50.0f, 50.0f}, kBlack);
    painter.FillRect(FloatRect{100.0f, 100.0f, 50.0f, 50.0f}, kBlack);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        ExpectEqInt(Red(canvas.Row(y)[x]), 0xFF, "nothing off-surface may land on the surface");
      }
    }
  });

  AddTest(tests, "Painter/FillingAnEmptyCanvasIsSafe", [] {
    Canvas canvas;
    Painter painter(canvas);
    painter.FillRect(FloatRect{0.0f, 0.0f, 10.0f, 10.0f}, kBlack);
    Expect(canvas.IsEmpty(), "a zero-size canvas stays empty and does not crash");
  });

  // --- Pixel goldens --------------------------------------------------------
  // The rasterizer is deterministic software, so these bytes are the same on
  // every machine. They are the regression net for every future change to
  // subdivision, coverage, or blending.

  AddTest(tests, "Painter/Golden/Circle", [] {
    Canvas canvas(64, 64);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    Path path;
    path.AddEllipse(FloatRect{6.5f, 6.5f, 51.0f, 51.0f});
    painter.FillPath(path, kBlue);

    const ComparisonResult result = CompareAgainstGolden(canvas, "path/circle");
    Expect(result.matches, result.message);
  });

  AddTest(tests, "Painter/Golden/RoundedRect", [] {
    Canvas canvas(64, 40);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    Path path;
    path.AddRoundedRect(FloatRect{4.25f, 4.5f, 55.0f, 31.0f}, 12.0f, 2.0f, 0.0f, 8.0f);
    painter.FillPath(path, kBlack);

    const ComparisonResult result = CompareAgainstGolden(canvas, "path/rounded-rect");
    Expect(result.matches, result.message);
  });

  AddTest(tests, "Painter/Golden/StarEvenOdd", [] {
    // A five-pointed star is the canonical fill-rule fixture: the pentagon in
    // the middle has winding 2, so the two rules render visibly different
    // shapes from identical geometry.
    const auto star = [](float cx) {
      Path path;
      const float cy = 32.0f;
      const float radius = 28.0f;
      for (int i = 0; i < 5; ++i) {
        // Two fifths of a turn per step is what makes the contour self-cross.
        const float angle = -1.57079633f + static_cast<float>(i) * 2.51327412f;
        const FloatPoint p{cx + radius * std::cos(angle), cy + radius * std::sin(angle)};
        if (i == 0) {
          path.MoveTo(p);
        } else {
          path.LineTo(p);
        }
      }
      path.Close();
      return path;
    };

    Canvas canvas(128, 64);
    canvas.Clear(kWhite);
    Painter painter(canvas);
    painter.FillPath(star(32.0f), kBlack, FillRule::NonZero);
    painter.FillPath(star(96.0f), kBlack, FillRule::EvenOdd);

    const ComparisonResult result = CompareAgainstGolden(canvas, "path/star-fill-rules");
    Expect(result.matches, result.message);
  });
}

}  // namespace microbrowser::tests

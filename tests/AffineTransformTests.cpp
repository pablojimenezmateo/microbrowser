#include <cmath>
#include <vector>

#include "TestSupport.h"
#include "gfx/AffineTransform.h"
#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "gfx/PathFlattener.h"
#include "gfx/Rasterizer.h"

namespace microbrowser::tests {

using gfx::AffineTransform;
using gfx::Canvas;
using gfx::Color;
using gfx::CoverageSpan;
using gfx::FillRule;
using gfx::FloatPoint;
using gfx::FloatRect;
using gfx::IntRect;
using gfx::Painter;
using gfx::Path;
using gfx::PathRasterizer;

namespace {

constexpr float kPi = 3.14159265358979f;

bool Near(float a, float b, float tolerance = 1e-4f) {
  return std::abs(a - b) <= tolerance;
}

bool NearPoint(FloatPoint a, FloatPoint b, float tolerance = 1e-3f) {
  return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance);
}

struct CountingSink {
  std::size_t points = 0;
  void BeginContour(FloatPoint) { ++points; }
  void LineTo(FloatPoint) { ++points; }
  void EndContour(bool) {}
};

double CoveredArea(const std::vector<CoverageSpan>& spans) {
  double total = 0.0;
  for (const CoverageSpan& span : spans) {
    total += static_cast<double>(span.length) * static_cast<double>(span.coverage) / 255.0;
  }
  return total;
}

}  // namespace

void RegisterAffineTransformTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AffineTransform/DefaultsToIdentity", [] {
    const AffineTransform identity;
    Expect(identity.IsIdentity(), "a default-constructed transform must change nothing");
    Expect(identity.MapPoint(FloatPoint{3.0f, -7.0f}) == (FloatPoint{3.0f, -7.0f}),
           "and must map a point to itself");
  });

  AddTest(tests, "AffineTransform/MatchesTheCssMatrixArgumentOrder", [] {
    // matrix(a, b, c, d, e, f) means x' = a*x + c*y + e. Getting this
    // transposed produces a transform that is correct for every symmetric case
    // and wrong for every skew, which is the worst possible failure mode.
    const AffineTransform t{2.0f, 3.0f, 5.0f, 7.0f, 11.0f, 13.0f};
    const FloatPoint mapped = t.MapPoint(FloatPoint{1.0f, 1.0f});
    Expect(NearPoint(mapped, FloatPoint{2.0f + 5.0f + 11.0f, 3.0f + 7.0f + 13.0f}),
           "x' = a*x + c*y + e and y' = b*x + d*y + f");
  });

  AddTest(tests, "AffineTransform/ThenAppliesLeftToRight", [] {
    // Scale then translate must move the already-scaled point by the full
    // translation; translate then scale must scale the translation too. If
    // Then() multiplied the other way these two would swap, and every nested
    // transform in a document would be subtly wrong.
    const AffineTransform scale = AffineTransform::Scaling(2.0f, 2.0f);
    const AffineTransform translate = AffineTransform::Translation(10.0f, 0.0f);

    Expect(NearPoint(scale.Then(translate).MapPoint(FloatPoint{1.0f, 0.0f}),
                     FloatPoint{12.0f, 0.0f}),
           "scale first: 1 becomes 2, then shifts by 10");
    Expect(NearPoint(translate.Then(scale).MapPoint(FloatPoint{1.0f, 0.0f}),
                     FloatPoint{22.0f, 0.0f}),
           "translate first: 1 becomes 11, then doubles");
  });

  AddTest(tests, "AffineTransform/RotationTurnsTowardPositiveYAtNinetyDegrees", [] {
    const FloatPoint mapped = AffineTransform::Rotation(kPi * 0.5f).MapPoint(FloatPoint{1.0f, 0.0f});
    Expect(NearPoint(mapped, FloatPoint{0.0f, 1.0f}),
           "with y downward on screen, a positive rotation is clockwise, as CSS defines it");
  });

  AddTest(tests, "AffineTransform/InverseUndoesTheTransform", [] {
    const AffineTransform t = AffineTransform::Scaling(3.0f, -2.0f)
                                  .Then(AffineTransform::Rotation(0.7f))
                                  .Then(AffineTransform::Translation(-4.0f, 9.0f));
    const auto inverse = t.Inverted();
    Expect(inverse.has_value(), "an invertible transform must invert");
    const FloatPoint p{5.5f, -3.25f};
    Expect(NearPoint(inverse->MapPoint(t.MapPoint(p)), p, 1e-2f),
           "the round trip must return the point it started from");
  });

  AddTest(tests, "AffineTransform/ACollapsingTransformHasNoInverse", [] {
    Expect(!AffineTransform::Scaling(0.0f, 5.0f).Inverted().has_value(),
           "a zero scale on one axis flattens the plane onto a line");
    Expect(!AffineTransform{1.0f, 2.0f, 2.0f, 4.0f, 0.0f, 0.0f}.Inverted().has_value(),
           "and so does any singular matrix, whether or not it looks like a scale");
  });

  AddTest(tests, "AffineTransform/MapRectIsTheBoundingBoxOfTheMappedShape", [] {
    const FloatRect unit{0.0f, 0.0f, 1.0f, 1.0f};
    const FloatRect rotated = AffineTransform::Rotation(kPi * 0.25f).MapRect(unit);
    // A unit square rotated 45 degrees has a bounding box of sqrt(2) a side.
    Expect(Near(rotated.width, 1.41421356f, 1e-4f), "the box must contain the rotated square");
    Expect(Near(rotated.height, 1.41421356f, 1e-4f), "on both axes");

    const FloatRect flipped = AffineTransform::Scaling(-2.0f, 1.0f).MapRect(unit);
    Expect(Near(flipped.x, -2.0f) && Near(flipped.width, 2.0f),
           "a negative scale must produce a normalized box, not a negative extent");
  });

  AddTest(tests, "AffineTransform/MaximumScaleReportsTheLargestStretch", [] {
    Expect(Near(AffineTransform{}.MaximumScale(), 1.0f), "identity stretches nothing");
    Expect(Near(AffineTransform::Scaling(3.0f, 0.5f).MaximumScale(), 3.0f),
           "a non-uniform scale stretches by its largest axis");
    Expect(Near(AffineTransform::Rotation(0.9f).MaximumScale(), 1.0f, 1e-3f),
           "a rotation stretches nothing");
    // The case the common "longest column" approximation gets wrong: a scale
    // applied along an axis the rotation has already turned.
    const AffineTransform rotated_scale =
        AffineTransform::Rotation(kPi * 0.25f).Then(AffineTransform::Scaling(4.0f, 1.0f));
    Expect(rotated_scale.MaximumScale() > 2.8f,
           "a rotated non-uniform scale still stretches by nearly its full factor, and a "
           "flattener that under-reports it subdivides too coarsely");
  });

  // --- Interaction with flattening -----------------------------------------

  AddTest(tests, "AffineTransform/CurvesAreSubdividedInDeviceSpaceNotLayoutSpace", [] {
    Path circle;
    circle.AddEllipse(FloatRect{0.0f, 0.0f, 10.0f, 10.0f});

    CountingSink plain;
    gfx::FlattenPath(circle, gfx::kFlattenTolerance, plain);

    CountingSink scaled;
    gfx::FlattenPath(circle, AffineTransform::Scaling(20.0f, 20.0f), gfx::kFlattenTolerance,
                     scaled);

    Expect(scaled.points > plain.points * 3,
           "a curve drawn twenty times larger needs far more segments to stay within a "
           "tolerance measured in device pixels; flattening before the transform would "
           "produce a visibly faceted circle at exactly the zoom somebody is inspecting");
  });

  AddTest(tests, "AffineTransform/RasterizingUnderATransformMatchesRasterizingTransformedInput", [] {
    // The same shape, reached two ways: scaled by the rasterizer, or written
    // out at the larger size. The pixels must agree, or a transform is a
    // second rendering path with its own bugs.
    Path small;
    small.AddRect(FloatRect{1.0f, 1.0f, 4.0f, 3.0f});
    Path large;
    large.AddRect(FloatRect{2.0f, 2.0f, 8.0f, 6.0f});

    PathRasterizer rasterizer;
    const std::vector<CoverageSpan> transformed = rasterizer.Rasterize(
        small, FillRule::NonZero, IntRect{0, 0, 16, 16}, AffineTransform::Scaling(2.0f, 2.0f));
    const std::vector<CoverageSpan> direct =
        rasterizer.Rasterize(large, FillRule::NonZero, IntRect{0, 0, 16, 16});
    Expect(transformed == direct, "the transform must not be a second rendering path");
  });

  AddTest(tests, "AffineTransform/AFillUnderARotationConservesArea", [] {
    Path square;
    square.AddRect(FloatRect{-10.0f, -10.0f, 20.0f, 20.0f});

    PathRasterizer rasterizer;
    const AffineTransform spin =
        AffineTransform::Rotation(0.4f).Then(AffineTransform::Translation(32.0f, 32.0f));
    const auto& spans = rasterizer.Rasterize(square, FillRule::NonZero, IntRect{0, 0, 64, 64}, spin);
    Expect(std::abs(CoveredArea(spans) - 400.0) < 2.0,
           "a rotation preserves area, so the coverage it produces must too");
  });

  AddTest(tests, "AffineTransform/ThePainterAppliesItsTransformToFillsAndStrokes", [] {
    Canvas canvas(32, 32);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    painter.SetTransform(AffineTransform::Translation(16.0f, 16.0f));
    painter.FillRect(FloatRect{0.0f, 0.0f, 8.0f, 8.0f}, Color::Rgb(0, 0, 0));

    ExpectEqInt(static_cast<int>((canvas.Row(20)[20] >> 16) & 0xFFu), 0x00,
                "the fill landed where the transform put it");
    ExpectEqInt(static_cast<int>((canvas.Row(4)[4] >> 16) & 0xFFu), 0xFF,
                "and not where it was written");
  });

  AddTest(tests, "AffineTransform/AStrokeIsTransformedAsAnOutlineNotWidenedInDeviceSpace", [] {
    // Under a non-uniform scale, a stroke has to be built in layout space and
    // then transformed. Widening in device space would give a horizontal and a
    // vertical edge the same thickness, when the correct answer is that the
    // scaled axis is thicker.
    Path line;
    line.MoveTo(FloatPoint{0.0f, 0.0f});
    line.LineTo(FloatPoint{0.0f, 10.0f});

    Canvas canvas(64, 64);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    painter.SetTransform(
        AffineTransform::Scaling(4.0f, 1.0f).Then(AffineTransform::Translation(16.0f, 20.0f)));
    gfx::StrokeStyle style;
    style.width = 2.0f;
    painter.StrokePath(line, style, Color::Rgb(0, 0, 0));

    int dark_pixels = 0;
    for (int x = 0; x < 64; ++x) {
      if (((canvas.Row(25)[x] >> 16) & 0xFFu) < 0x40) {
        ++dark_pixels;
      }
    }
    ExpectEqInt(dark_pixels, 8,
                "a vertical 2px stroke scaled 4x horizontally is 8 device pixels wide");
  });
}

}  // namespace microbrowser::tests

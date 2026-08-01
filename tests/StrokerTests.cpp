#include <cmath>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"
#include "gfx/Stroker.h"
#include "support/ReferenceImage.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::CoverageSpan;
using gfx::FillRule;
using gfx::FloatPoint;
using gfx::FloatRect;
using gfx::IntRect;
using gfx::LineCap;
using gfx::LineJoin;
using gfx::Painter;
using gfx::Path;
using gfx::PathRasterizer;
using gfx::StrokeStyle;

namespace {

constexpr Color kWhite = Color::Rgb(0xFF, 0xFF, 0xFF);
constexpr Color kBlack = Color::Rgb(0x00, 0x00, 0x00);

Path Stroked(const Path& path, const StrokeStyle& style) {
  Path out;
  gfx::StrokeToPath(path, style, out);
  return out;
}

Path Line(FloatPoint a, FloatPoint b) {
  Path path;
  path.MoveTo(a);
  path.LineTo(b);
  return path;
}

double StrokedArea(const Path& path, const StrokeStyle& style, IntRect clip) {
  PathRasterizer rasterizer;
  const auto& spans = rasterizer.Rasterize(Stroked(path, style), FillRule::NonZero, clip);
  double total = 0.0;
  for (const CoverageSpan& span : spans) {
    total += static_cast<double>(span.length) * static_cast<double>(span.coverage) / 255.0;
  }
  return total;
}

int CoverageAt(const std::vector<CoverageSpan>& spans, int x, int y) {
  for (const CoverageSpan& span : spans) {
    if (span.y == y && x >= span.x && x < span.x + span.length) {
      return span.coverage;
    }
  }
  return 0;
}

}  // namespace

void RegisterStrokerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Stroker/AHorizontalLineBecomesItsRectangle", [] {
    StrokeStyle style;
    style.width = 4.0f;
    const double area =
        StrokedArea(Line(FloatPoint{4.0f, 16.0f}, FloatPoint{28.0f, 16.0f}), style,
                    IntRect{0, 0, 32, 32});
    Expect(std::abs(area - 24.0 * 4.0) < 0.1, "length times width, with butt caps adding nothing");
  });

  AddTest(tests, "Stroker/CapsAddExactlyTheirGeometry", [] {
    const Path line = Line(FloatPoint{8.0f, 24.0f}, FloatPoint{40.0f, 24.0f});
    const IntRect clip{0, 0, 48, 48};

    StrokeStyle butt;
    butt.width = 6.0f;
    StrokeStyle square = butt;
    square.cap = LineCap::Square;
    StrokeStyle round = butt;
    round.cap = LineCap::Round;

    const double body = 32.0 * 6.0;
    Expect(std::abs(StrokedArea(line, butt, clip) - body) < 0.1, "butt caps add nothing");
    Expect(std::abs(StrokedArea(line, square, clip) - (body + 2.0 * 3.0 * 6.0)) < 0.2,
           "each square cap adds a half-width by full-width block");

    const double round_area = StrokedArea(line, round, clip);
    const double expected_round = body + 3.14159265 * 3.0 * 3.0;
    Expect(std::abs(round_area - expected_round) / expected_round < 0.01,
           "the two round caps add exactly one circle between them");
  });

  AddTest(tests, "Stroker/ZeroAndNegativeWidthsDrawNothing", [] {
    const Path line = Line(FloatPoint{0.0f, 4.0f}, FloatPoint{16.0f, 4.0f});
    StrokeStyle style;
    style.width = 0.0f;
    Expect(Stroked(line, style).IsEmpty(), "a zero-width stroke has no geometry");
    style.width = -3.0f;
    Expect(Stroked(line, style).IsEmpty(), "a negative width is not a wide stroke");
    style.width = std::numeric_limits<float>::quiet_NaN();
    Expect(Stroked(line, style).IsEmpty(), "and a NaN width is not one either");
  });

  // The reason the whole stroker is built as a union of pieces: if any one
  // piece were wound backwards, its overlap with a neighbour would cancel to
  // winding zero and punch a hole straight through the joint.
  AddTest(tests, "Stroker/OverlappingPiecesNeverCancelToAHole", [] {
    Path zigzag;
    zigzag.MoveTo(FloatPoint{6.0f, 6.0f});
    zigzag.LineTo(FloatPoint{26.0f, 26.0f});
    zigzag.LineTo(FloatPoint{6.0f, 46.0f});
    zigzag.LineTo(FloatPoint{26.0f, 66.0f});

    StrokeStyle style;
    style.width = 9.0f;
    PathRasterizer rasterizer;
    const auto& spans =
        rasterizer.Rasterize(Stroked(zigzag, style), FillRule::NonZero, IntRect{0, 0, 72, 72});

    // Every vertex is a place where two quads and a join overlap.
    ExpectEqInt(CoverageAt(spans, 26, 26), 255, "the first corner is solid, not hollow");
    ExpectEqInt(CoverageAt(spans, 6, 46), 255, "and so is the second");
  });

  AddTest(tests, "Stroker/AWideStrokeOnATightCornerStaysConnected", [] {
    // Segments much shorter than the stroke width are where offset-outline
    // strokers produce self-intersecting garbage.
    Path spike;
    spike.MoveTo(FloatPoint{10.0f, 20.0f});
    spike.LineTo(FloatPoint{20.0f, 20.0f});
    spike.LineTo(FloatPoint{20.5f, 20.5f});
    spike.LineTo(FloatPoint{10.0f, 21.0f});

    StrokeStyle style;
    style.width = 12.0f;
    PathRasterizer rasterizer;
    const auto& spans =
        rasterizer.Rasterize(Stroked(spike, style), FillRule::NonZero, IntRect{0, 0, 40, 40});
    for (int x = 12; x <= 18; ++x) {
      ExpectEqInt(CoverageAt(spans, x, 20), 255, "the stroke body must remain continuous");
    }
  });

  AddTest(tests, "Stroker/MiterFallsBackToBevelPastTheLimit", [] {
    // A very shallow turn has an unbounded miter. The limit is what stops it
    // from becoming a spike hundreds of pixels long.
    Path shallow;
    shallow.MoveTo(FloatPoint{0.0f, 40.0f});
    shallow.LineTo(FloatPoint{40.0f, 40.0f});
    shallow.LineTo(FloatPoint{0.0f, 41.0f});

    StrokeStyle style;
    style.width = 8.0f;
    style.join = LineJoin::Miter;
    style.miter_limit = 4.0f;
    const gfx::FloatRect bounds = Stroked(shallow, style).ControlBounds();
    Expect(bounds.Right() < 40.0f + 4.0f * 8.0f,
           "the miter limit bounds the spike at miter_limit times the stroke width");

    style.miter_limit = 1000.0f;
    const gfx::FloatRect unbounded = Stroked(shallow, style).ControlBounds();
    Expect(unbounded.Right() > bounds.Right(),
           "and raising the limit must actually let the spike grow, or the limit is inert");
  });

  AddTest(tests, "Stroker/JoinStyleChangesOnlyTheOutsideOfTheTurn", [] {
    Path corner;
    corner.MoveTo(FloatPoint{8.0f, 32.0f});
    corner.LineTo(FloatPoint{32.0f, 32.0f});
    corner.LineTo(FloatPoint{32.0f, 8.0f});

    StrokeStyle miter;
    miter.width = 10.0f;
    miter.join = LineJoin::Miter;
    StrokeStyle bevel = miter;
    bevel.join = LineJoin::Bevel;
    StrokeStyle round = miter;
    round.join = LineJoin::Round;

    const IntRect clip{0, 0, 48, 48};
    const double miter_area = StrokedArea(corner, miter, clip);
    const double bevel_area = StrokedArea(corner, bevel, clip);
    const double round_area = StrokedArea(corner, round, clip);

    Expect(miter_area > round_area && round_area > bevel_area,
           "a right-angle corner is largest mitered, smallest bevelled, round in between");
    // At a right angle the bevel is a triangle with two half-width legs and the
    // miter is the square that completes it, so the miter adds exactly half of
    // a half-width square: 5 * 5 / 2.
    Expect(std::abs((miter_area - bevel_area) - 12.5) < 0.5,
           "the miter completes the bevel triangle into a square, adding half its area again");
  });

  AddTest(tests, "Stroker/AClosedContourHasNoCapsAndAJoinAtItsStart", [] {
    Path rect;
    rect.AddRect(FloatRect{8.0f, 8.0f, 24.0f, 24.0f});

    StrokeStyle style;
    style.width = 4.0f;
    style.join = LineJoin::Miter;
    const IntRect clip{0, 0, 48, 48};

    // A mitered rectangle outline is exactly the difference of two rectangles.
    const double expected = (28.0 * 28.0) - (20.0 * 20.0);
    Expect(std::abs(StrokedArea(rect, style, clip) - expected) < 0.5,
           "the corner where the contour closes must be joined like every other one; "
           "a missing wrap-around join leaves a notch");
  });

  AddTest(tests, "Stroker/ADegenerateContourRendersAsACapShape", [] {
    Path dot;
    dot.MoveTo(FloatPoint{16.0f, 16.0f});
    dot.LineTo(FloatPoint{16.0f, 16.0f});
    const IntRect clip{0, 0, 32, 32};

    StrokeStyle style;
    style.width = 8.0f;
    style.cap = LineCap::Butt;
    Expect(StrokedArea(dot, style, clip) < 0.01, "a zero-length dash with butt caps is invisible");

    style.cap = LineCap::Round;
    const double disc = 3.14159265 * 16.0;
    const double measured = StrokedArea(dot, style, clip);
    // Same one-sided bound as any flattened circle: the polygon is inscribed,
    // so it is small by 4 * tolerance / (3 * radius) — 3.3% at radius 4, which
    // is 0.09 device pixels of inward error and invisible.
    Expect(measured <= disc, "a polygonal disc cannot be larger than its circle");
    Expect((disc - measured) / disc < 0.04,
           "with round caps it is a dot, which is what makes a dotted border work");

    style.cap = LineCap::Square;
    Expect(std::abs(StrokedArea(dot, style, clip) - 64.0) < 0.2,
           "and with square caps it is a square of the stroke width");
  });

  AddTest(tests, "Stroker/StrokingACurveFollowsIt", [] {
    Path arc;
    arc.MoveTo(FloatPoint{8.0f, 56.0f});
    arc.CubicTo(FloatPoint{8.0f, 8.0f}, FloatPoint{56.0f, 8.0f}, FloatPoint{56.0f, 56.0f});

    StrokeStyle style;
    style.width = 6.0f;
    style.join = LineJoin::Round;
    const double area = StrokedArea(arc, style, IntRect{0, 0, 64, 64});
    // Arc length of that cubic is ~101px; a stroke of width 6 covers about
    // length * width, less a little where the inside of the curve overlaps.
    Expect(area > 500.0 && area < 640.0,
           "the stroke area must track the curve's arc length, not its chord");
  });

  AddTest(tests, "Stroker/ATranslucentStrokeDoesNotDarkenWhereItOverlapsItself", [] {
    Canvas canvas(32, 32);
    canvas.Clear(kWhite);
    Painter painter(canvas);

    Path corner;
    corner.MoveTo(FloatPoint{4.0f, 16.0f});
    corner.LineTo(FloatPoint{16.0f, 16.0f});
    corner.LineTo(FloatPoint{16.0f, 28.0f});

    StrokeStyle style;
    style.width = 8.0f;
    painter.StrokePath(corner, style, kBlack.WithAlpha(0x80));

    // The pixel at the corner is covered by two segment quads and a join. A
    // stroker that painted each piece separately would blend three times and
    // show a dark knot exactly where a rounded rectangle's corners are.
    const int corner_pixel = static_cast<int>((canvas.Row(16)[16] >> 16) & 0xFFu);
    const int body_pixel = static_cast<int>((canvas.Row(16)[8] >> 16) & 0xFFu);
    ExpectEqInt(corner_pixel, body_pixel,
                "overlapping pieces of one stroke composite once, not once per piece");
  });

  AddTest(tests, "Stroker/Golden/JoinsAndCaps", [] {
    Canvas canvas(160, 120);
    canvas.Clear(kWhite);
    Painter painter(canvas);

    const auto chevron = [](float x, float y) {
      Path path;
      path.MoveTo(FloatPoint{x, y + 28.0f});
      path.LineTo(FloatPoint{x + 18.0f, y});
      path.LineTo(FloatPoint{x + 36.0f, y + 28.0f});
      return path;
    };

    StrokeStyle style;
    style.width = 11.0f;

    style.join = LineJoin::Miter;
    style.cap = LineCap::Butt;
    painter.StrokePath(chevron(10.0f, 12.0f), style, kBlack);
    style.join = LineJoin::Round;
    style.cap = LineCap::Round;
    painter.StrokePath(chevron(62.0f, 12.0f), style, kBlack);
    style.join = LineJoin::Bevel;
    style.cap = LineCap::Square;
    painter.StrokePath(chevron(114.0f, 12.0f), style, kBlack);

    Path circle;
    circle.AddEllipse(FloatRect{16.0f, 62.0f, 44.0f, 44.0f});
    style.join = LineJoin::Round;
    painter.StrokePath(circle, style, kBlack);

    Path rounded;
    rounded.AddRoundedRect(FloatRect{78.0f, 64.5f, 68.0f, 40.0f}, 14.0f, 14.0f, 2.0f, 2.0f);
    style.width = 5.0f;
    style.join = LineJoin::Miter;
    painter.StrokePath(rounded, style, kBlack);

    const ComparisonResult result = CompareAgainstGolden(canvas, "stroke/joins-and-caps");
    Expect(result.matches, result.message);
  });
}

}  // namespace microbrowser::tests

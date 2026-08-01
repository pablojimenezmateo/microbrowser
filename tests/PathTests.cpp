#include <cmath>
#include <limits>
#include <vector>

#include "TestSupport.h"
#include "gfx/Path.h"
#include "gfx/PathFlattener.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::tests {

using gfx::FloatPoint;
using gfx::FloatRect;
using gfx::Path;
using gfx::PathVerb;

namespace {

// Collects the flattener's output so a test can assert on the polyline rather
// than on pixels.
struct RecordingSink {
  std::vector<FloatPoint> points;
  std::vector<bool> closed;
  int contours = 0;

  void BeginContour(FloatPoint p) {
    ++contours;
    points.push_back(p);
  }
  void LineTo(FloatPoint p) { points.push_back(p); }
  void EndContour(bool was_closed) { closed.push_back(was_closed); }
};

double PolylineLength(const std::vector<FloatPoint>& points) {
  double total = 0.0;
  for (std::size_t i = 1; i < points.size(); ++i) {
    const double dx = static_cast<double>(points[i].x) - static_cast<double>(points[i - 1].x);
    const double dy = static_cast<double>(points[i].y) - static_cast<double>(points[i - 1].y);
    total += std::sqrt(dx * dx + dy * dy);
  }
  return total;
}

constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

}  // namespace

void RegisterPathTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Path/StartsEmpty", [] {
    Path path;
    Expect(path.IsEmpty(), "a default path has no verbs");
    Expect(path.ControlBounds() == FloatRect{}, "an empty path has empty bounds");
    Expect(path.CurrentPoint() == FloatPoint{}, "an empty path has no pen position");
  });

  AddTest(tests, "Path/RecordsVerbsAndPointsInOrder", [] {
    Path path;
    path.MoveTo(FloatPoint{1.0f, 2.0f});
    path.LineTo(FloatPoint{3.0f, 4.0f});
    path.QuadTo(FloatPoint{5.0f, 6.0f}, FloatPoint{7.0f, 8.0f});
    path.CubicTo(FloatPoint{9.0f, 10.0f}, FloatPoint{11.0f, 12.0f}, FloatPoint{13.0f, 14.0f});
    path.Close();

    ExpectEqInt(static_cast<long long>(path.VerbCount()), 5, "five verbs were recorded");
    ExpectEqInt(static_cast<long long>(path.Points().size()), 7,
                "points are 1 + 1 + 2 + 3, and Close carries none");
    Expect(path.Verbs()[2] == PathVerb::Quad, "the third verb is the quadratic");
  });

  // The single most useful degenerate case: layout hands the painter a curve
  // before it hands it a start point, and the wrong recovery draws a wedge from
  // the origin across the whole surface.
  AddTest(tests, "Path/LineWithNoContourStartsOneInsteadOfDrawingFromTheOrigin", [] {
    Path path;
    path.LineTo(FloatPoint{5.0f, 5.0f});
    ExpectEqInt(static_cast<long long>(path.VerbCount()), 1, "only the implied move is recorded");
    Expect(path.Verbs()[0] == PathVerb::Move, "the implied verb is a move, not a line");
    Expect(path.CurrentPoint() == FloatPoint{5.0f, 5.0f}, "the pen lands where the line asked");
  });

  AddTest(tests, "Path/CloseLeavesThePenAtTheContourStart", [] {
    Path path;
    path.MoveTo(FloatPoint{2.0f, 3.0f});
    path.LineTo(FloatPoint{8.0f, 9.0f});
    path.Close();
    Expect(path.CurrentPoint() == FloatPoint{2.0f, 3.0f},
           "a closed contour leaves the pen where the contour began, as `z` does in SVG");
  });

  AddTest(tests, "Path/CloseWithNoOpenContourIsIgnored", [] {
    Path path;
    path.Close();
    path.Close();
    Expect(path.IsEmpty(), "closing nothing records nothing");
  });

  // Non-finite coordinates are the security-relevant case: everything
  // downstream converts to fixed point, and a NaN there is undefined behavior
  // rather than a wrong pixel.
  AddTest(tests, "Path/NonFiniteCoordinatesNeverEnterThePath", [] {
    util::ResetPerformanceCounters();
    Path path;
    path.MoveTo(FloatPoint{kNaN, 0.0f});
    path.LineTo(FloatPoint{0.0f, kInfinity});
    path.QuadTo(FloatPoint{kNaN, kNaN}, FloatPoint{1.0f, 1.0f});
    path.CubicTo(FloatPoint{1.0f, 1.0f}, FloatPoint{2.0f, 2.0f}, FloatPoint{-kInfinity, 0.0f});
    Expect(path.IsEmpty(), "every command carrying a non-finite coordinate is dropped");
    ExpectEqInt(
        static_cast<long long>(util::ReadPerformanceCounter(
            util::PerfCounterId::GfxPathNonFiniteRejections)),
        4, "each rejection is counted, so a page producing them is visible rather than silent");

    path.MoveTo(FloatPoint{1.0f, 1.0f});
    Expect(!path.IsEmpty(), "a finite command after a rejected one still lands");
  });

  AddTest(tests, "Path/AddRectProducesAClosedFourSidedContour", [] {
    Path path;
    path.AddRect(FloatRect{1.0f, 2.0f, 3.0f, 4.0f});
    ExpectEqInt(static_cast<long long>(path.VerbCount()), 5, "move, three lines, close");
    Expect(path.Verbs().back() == PathVerb::Close, "the rect contour is closed");
    Expect(path.ControlBounds() == FloatRect{1.0f, 2.0f, 3.0f, 4.0f}, "bounds match the rect");
  });

  AddTest(tests, "Path/AddRectIgnoresAnEmptyRect", [] {
    Path path;
    path.AddRect(FloatRect{1.0f, 2.0f, 0.0f, 4.0f});
    path.AddRect(FloatRect{1.0f, 2.0f, 3.0f, -1.0f});
    Expect(path.IsEmpty(), "a rect with no area contributes no contour");
  });

  AddTest(tests, "Path/RoundedRectRadiiAreScaledDownWhenAdjacentCornersWouldOverlap", [] {
    // Radii summing to more than the side force a uniform scale-down; without
    // it the corner cubics cross and the contour self-intersects.
    Path path;
    path.AddRoundedRect(FloatRect{0.0f, 0.0f, 10.0f, 10.0f}, 8.0f, 8.0f, 8.0f, 8.0f);
    const FloatRect bounds = path.ControlBounds();
    Expect(bounds.x >= -0.01f && bounds.y >= -0.01f, "corner control points stay inside the rect");
    Expect(bounds.Right() <= 10.01f && bounds.Bottom() <= 10.01f,
           "a scaled-down radius cannot push the contour outside the rect it rounds");
  });

  AddTest(tests, "Path/RoundedRectWithZeroRadiiIsARect", [] {
    Path rounded;
    rounded.AddRoundedRect(FloatRect{0.0f, 0.0f, 4.0f, 4.0f}, 0.0f, 0.0f, 0.0f, 0.0f);
    ExpectEqInt(static_cast<long long>(rounded.VerbCount()), 6,
                "move plus four lines plus close; no cubics are emitted for a zero radius");
  });

  AddTest(tests, "Path/RoundedRectRejectsNonFiniteRadiiWithoutRejectingTheRect", [] {
    Path path;
    path.AddRoundedRect(FloatRect{0.0f, 0.0f, 10.0f, 10.0f}, kNaN, -5.0f, kInfinity, 2.0f);
    Expect(!path.IsEmpty(), "a bad radius must not lose the whole shape");
    const FloatRect bounds = path.ControlBounds();
    Expect(bounds == FloatRect{0.0f, 0.0f, 10.0f, 10.0f},
           "non-finite and negative radii are treated as square corners");
  });

  AddTest(tests, "Path/EllipseSpansItsBounds", [] {
    Path path;
    path.AddEllipse(FloatRect{0.0f, 0.0f, 20.0f, 10.0f});
    const FloatRect bounds = path.ControlBounds();
    Expect(bounds.x <= 0.01f && bounds.y <= 0.01f, "the ellipse touches its top-left bounds");
    Expect(bounds.Right() >= 19.99f && bounds.Bottom() >= 9.99f,
           "the ellipse touches its bottom-right bounds");
  });

  AddTest(tests, "Path/ClearResetsThePen", [] {
    Path path;
    path.AddRect(FloatRect{0.0f, 0.0f, 2.0f, 2.0f});
    path.Clear();
    Expect(path.IsEmpty(), "Clear drops every verb");
    Expect(path.CurrentPoint() == FloatPoint{}, "Clear drops the pen position too");
  });

  // --- Flattening -----------------------------------------------------------

  AddTest(tests, "PathFlattener/ReportsWhetherEachContourWasClosed", [] {
    Path path;
    path.MoveTo(FloatPoint{0.0f, 0.0f});
    path.LineTo(FloatPoint{1.0f, 0.0f});
    path.Close();
    path.MoveTo(FloatPoint{2.0f, 0.0f});
    path.LineTo(FloatPoint{3.0f, 0.0f});

    RecordingSink sink;
    gfx::FlattenPath(path, gfx::kFlattenTolerance, sink);
    ExpectEqInt(sink.contours, 2, "two contours were begun");
    ExpectEqInt(static_cast<long long>(sink.closed.size()), 2, "two contours were ended");
    Expect(sink.closed[0] && !sink.closed[1],
           "closure is reported rather than resolved; a stroker and a filler need it differently");
  });

  AddTest(tests, "PathFlattener/StraightLinesAreNotSubdivided", [] {
    Path path;
    path.MoveTo(FloatPoint{0.0f, 0.0f});
    path.LineTo(FloatPoint{100.0f, 100.0f});

    RecordingSink sink;
    gfx::FlattenPath(path, gfx::kFlattenTolerance, sink);
    ExpectEqInt(static_cast<long long>(sink.points.size()), 2,
                "a line is already flat, however long it is");
  });

  AddTest(tests, "PathFlattener/ADegenerateCurveCostsOneSegment", [] {
    // Control points on the chord: the curve *is* the line, and subdividing it
    // is pure waste in the most common case a font produces.
    Path path;
    path.MoveTo(FloatPoint{0.0f, 0.0f});
    path.CubicTo(FloatPoint{10.0f, 10.0f}, FloatPoint{20.0f, 20.0f}, FloatPoint{30.0f, 30.0f});

    RecordingSink sink;
    gfx::FlattenPath(path, gfx::kFlattenTolerance, sink);
    ExpectEqInt(static_cast<long long>(sink.points.size()), 2, "one segment for a flat cubic");
  });

  AddTest(tests, "PathFlattener/SegmentCountGrowsWithCurvature", [] {
    const auto flatten = [](float bulge) {
      Path path;
      path.MoveTo(FloatPoint{0.0f, 0.0f});
      path.QuadTo(FloatPoint{50.0f, bulge}, FloatPoint{100.0f, 0.0f});
      RecordingSink sink;
      gfx::FlattenPath(path, gfx::kFlattenTolerance, sink);
      return sink.points.size();
    };
    Expect(flatten(200.0f) > flatten(20.0f),
           "a tighter curve needs more segments to stay within tolerance");
  });

  AddTest(tests, "PathFlattener/ApproximatesACircleToWithinTolerance", [] {
    // Circumference is the sharpest single scalar check on the whole
    // subdivision path: it catches a wrong Bezier evaluation, a wrong segment
    // count, and a wrong control-point constant at once.
    Path path;
    path.AddEllipse(FloatRect{0.0f, 0.0f, 200.0f, 200.0f});
    RecordingSink sink;
    gfx::FlattenPath(path, gfx::kFlattenTolerance, sink);

    const double circumference = 2.0 * 3.14159265358979 * 100.0;
    const double measured = PolylineLength(sink.points);
    Expect(std::abs(measured - circumference) / circumference < 0.001,
           "the flattened circle's perimeter must be within 0.1% of 2*pi*r");
  });

  AddTest(tests, "PathFlattener/BoundsTheSegmentCountForAbsurdCurves", [] {
    // A control point 10^9 pixels away is one CSS transform from real. The
    // subdivision count must be bounded by a constant, not by the input.
    Path path;
    path.MoveTo(FloatPoint{0.0f, 0.0f});
    path.CubicTo(FloatPoint{1e9f, 1e9f}, FloatPoint{-1e9f, 1e9f}, FloatPoint{10.0f, 0.0f});

    RecordingSink sink;
    gfx::FlattenPath(path, gfx::kFlattenTolerance, sink);
    Expect(sink.points.size() <= 513,
           "subdivision is capped, so a hostile control point cannot allocate without bound");
  });
}

}  // namespace microbrowser::tests

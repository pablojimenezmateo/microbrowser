#include <cmath>
#include <vector>

#include "TestSupport.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"

namespace microbrowser::tests {

using gfx::CoverageSpan;
using gfx::FillRule;
using gfx::FloatPoint;
using gfx::FloatRect;
using gfx::IntRect;
using gfx::Path;
using gfx::PathRasterizer;

namespace {

int CoverageAt(const std::vector<CoverageSpan>& spans, int x, int y) {
  for (const CoverageSpan& span : spans) {
    if (span.y == y && x >= span.x && x < span.x + span.length) {
      return span.coverage;
    }
  }
  return 0;
}

// Total covered area in pixels, as the rasterizer sees it. Comparing this
// against the shape's analytic area is the strongest single assertion available
// for an antialiasing rasterizer: it is sensitive to every edge being in the
// right place and to coverage being conserved, and it does not care which
// pixels the coverage landed in.
double CoveredArea(const std::vector<CoverageSpan>& spans) {
  double total = 0.0;
  for (const CoverageSpan& span : spans) {
    total += static_cast<double>(span.length) * static_cast<double>(span.coverage) / 255.0;
  }
  return total;
}

Path RectPath(FloatRect rect) {
  Path path;
  path.AddRect(rect);
  return path;
}

Path TrianglePath(FloatPoint a, FloatPoint b, FloatPoint c) {
  Path path;
  path.MoveTo(a);
  path.LineTo(b);
  path.LineTo(c);
  path.Close();
  return path;
}

}  // namespace

void RegisterRasterizerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Rasterizer/EmptyInputsProduceNoSpans", [] {
    PathRasterizer rasterizer;
    Expect(rasterizer.Rasterize(Path{}, FillRule::NonZero, IntRect{0, 0, 8, 8}).empty(),
           "an empty path covers nothing");
    Expect(rasterizer.Rasterize(RectPath(FloatRect{0, 0, 4, 4}), FillRule::NonZero, IntRect{})
               .empty(),
           "an empty clip covers nothing");
  });

  AddTest(tests, "Rasterizer/PixelAlignedRectIsFullyCoveredAndNothingElseIs", [] {
    PathRasterizer rasterizer;
    const auto& spans =
        rasterizer.Rasterize(RectPath(FloatRect{1, 1, 2, 2}), FillRule::NonZero, IntRect{0, 0, 8, 8});

    ExpectEqInt(CoverageAt(spans, 1, 1), 255, "an integer-aligned interior pixel is fully covered");
    ExpectEqInt(CoverageAt(spans, 2, 2), 255, "and so is the far corner of the rect");
    ExpectEqInt(CoverageAt(spans, 0, 1), 0, "the pixel left of the rect is untouched");
    ExpectEqInt(CoverageAt(spans, 3, 1), 0, "the pixel right of the rect is untouched");
    ExpectEqInt(CoverageAt(spans, 1, 0), 0, "the pixel above the rect is untouched");
    ExpectEqInt(CoverageAt(spans, 1, 3), 0, "the pixel below the rect is untouched");
    Expect(std::abs(CoveredArea(spans) - 4.0) < 0.02, "total coverage is the rect's four pixels");
  });

  // Analytic AA means the fraction is exact, not sampled. Half a pixel is 128,
  // not 127 or 130, and a sampling rasterizer with 4x4 subsamples cannot
  // produce the quarter-pixel value at all.
  AddTest(tests, "Rasterizer/PartialPixelCoverageIsTheExactAreaFraction", [] {
    PathRasterizer rasterizer;
    const IntRect clip{0, 0, 4, 4};

    const auto& half =
        rasterizer.Rasterize(RectPath(FloatRect{0.0f, 0.0f, 0.5f, 1.0f}), FillRule::NonZero, clip);
    ExpectEqInt(CoverageAt(half, 0, 0), 128, "half a pixel wide is coverage 128");

    const auto& quarter =
        rasterizer.Rasterize(RectPath(FloatRect{0.0f, 0.0f, 0.5f, 0.5f}), FillRule::NonZero, clip);
    ExpectEqInt(CoverageAt(quarter, 0, 0), 64, "a quarter of a pixel is coverage 64");

    const auto& sliver =
        rasterizer.Rasterize(RectPath(FloatRect{0.0f, 0.0f, 1.0f, 0.125f}), FillRule::NonZero, clip);
    ExpectEqInt(CoverageAt(sliver, 0, 0), 32,
                "an eighth of a pixel high is coverage 32; a sub-sampling rasterizer would "
                "quantize this to a multiple of its sample count");
  });

  AddTest(tests, "Rasterizer/OffsetRectSplitsCoverageAcrossTheBoundary", [] {
    PathRasterizer rasterizer;
    const auto& spans = rasterizer.Rasterize(RectPath(FloatRect{0.5f, 0.0f, 1.0f, 1.0f}),
                                             FillRule::NonZero, IntRect{0, 0, 4, 4});
    ExpectEqInt(CoverageAt(spans, 0, 0), 128, "the left half-pixel");
    ExpectEqInt(CoverageAt(spans, 1, 0), 128, "the right half-pixel");
    ExpectEqInt(CoverageAt(spans, 2, 0), 0, "and nothing beyond");
  });

  AddTest(tests, "Rasterizer/TriangleCoverageMatchesItsAnalyticArea", [] {
    PathRasterizer rasterizer;
    const auto& spans =
        rasterizer.Rasterize(TrianglePath(FloatPoint{2.0f, 2.0f}, FloatPoint{38.0f, 6.0f},
                                          FloatPoint{10.0f, 34.0f}),
                             FillRule::NonZero, IntRect{0, 0, 48, 48});
    // Shoelace: |(x1(y2-y3) + x2(y3-y1) + x3(y1-y2))| / 2
    const double area = std::abs(2.0 * (6.0 - 34.0) + 38.0 * (34.0 - 2.0) + 10.0 * (2.0 - 6.0)) / 2.0;
    const double measured = CoveredArea(spans);
    Expect(std::abs(measured - area) / area < 0.01,
           "an antialiasing rasterizer must conserve area to well under a percent");
  });

  AddTest(tests, "Rasterizer/CircleCoverageMatchesPiRSquared", [] {
    Path path;
    path.AddEllipse(FloatRect{4.0f, 4.0f, 40.0f, 40.0f});
    PathRasterizer rasterizer;
    const auto& spans = rasterizer.Rasterize(path, FillRule::NonZero, IntRect{0, 0, 52, 52});

    const double area = 3.14159265358979 * 20.0 * 20.0;
    const double measured = CoveredArea(spans);
    // The bound is one-sided on purpose. A chord lies inside its arc, so a
    // flattened circle is always slightly small, by 4 * tolerance / (3 * r) —
    // 0.67% here. An *over*-estimate would mean coverage was being invented,
    // which is a different bug with a different cause, so the test must not
    // accept it.
    Expect(measured <= area, "a flattened curve can only under-fill, never over-fill");
    Expect((area - measured) / area < 0.01,
           "curve subdivision must not lose more area than the flattening tolerance allows");
  });

  AddTest(tests, "Rasterizer/WindingDirectionDoesNotChangeTheFill", [] {
    PathRasterizer rasterizer;
    const auto clockwise = rasterizer.Rasterize(
        TrianglePath(FloatPoint{0, 0}, FloatPoint{10, 0}, FloatPoint{0, 10}), FillRule::NonZero,
        IntRect{0, 0, 16, 16});
    const std::vector<CoverageSpan> saved = clockwise;
    const auto& counter_clockwise = rasterizer.Rasterize(
        TrianglePath(FloatPoint{0, 10}, FloatPoint{10, 0}, FloatPoint{0, 0}), FillRule::NonZero,
        IntRect{0, 0, 16, 16});
    Expect(saved == counter_clockwise,
           "a contour's direction selects nothing on its own; only overlap makes it matter");
  });

  // The one case where the two fill rules must disagree. A test that only draws
  // non-overlapping shapes passes with the rule ignored entirely.
  AddTest(tests, "Rasterizer/FillRulesDisagreeOnlyWhereAPathOverlapsItself", [] {
    Path path;
    path.AddRect(FloatRect{0.0f, 0.0f, 8.0f, 8.0f});
    path.AddRect(FloatRect{2.0f, 2.0f, 4.0f, 4.0f});  // same direction: winding 2
    PathRasterizer rasterizer;
    const IntRect clip{0, 0, 8, 8};

    const std::vector<CoverageSpan> nonzero = rasterizer.Rasterize(path, FillRule::NonZero, clip);
    const std::vector<CoverageSpan> evenodd = rasterizer.Rasterize(path, FillRule::EvenOdd, clip);

    ExpectEqInt(CoverageAt(nonzero, 4, 4), 255, "winding 2 is inside under the nonzero rule");
    ExpectEqInt(CoverageAt(evenodd, 4, 4), 0, "winding 2 is outside under the even-odd rule");
    ExpectEqInt(CoverageAt(nonzero, 0, 0), 255, "winding 1 is inside under both");
    ExpectEqInt(CoverageAt(evenodd, 0, 0), 255, "winding 1 is inside under both");
  });

  AddTest(tests, "Rasterizer/AReversedInnerContourCutsAHoleUnderBothRules", [] {
    Path path;
    path.AddRect(FloatRect{0.0f, 0.0f, 8.0f, 8.0f});
    // Counter-clockwise inner contour: winding 0 inside, so it is a hole under
    // the nonzero rule as well as the even-odd one.
    path.MoveTo(FloatPoint{2.0f, 2.0f});
    path.LineTo(FloatPoint{2.0f, 6.0f});
    path.LineTo(FloatPoint{6.0f, 6.0f});
    path.LineTo(FloatPoint{6.0f, 2.0f});
    path.Close();

    PathRasterizer rasterizer;
    for (const FillRule rule : {FillRule::NonZero, FillRule::EvenOdd}) {
      const auto& spans = rasterizer.Rasterize(path, rule, IntRect{0, 0, 8, 8});
      ExpectEqInt(CoverageAt(spans, 4, 4), 0, "the reversed contour is a hole");
      ExpectEqInt(CoverageAt(spans, 0, 0), 255, "the outer ring is still filled");
      Expect(std::abs(CoveredArea(spans) - (64.0 - 16.0)) < 0.5,
             "the hole removes exactly its area");
    }
  });

  // Clipping is where a rasterizer quietly goes wrong: the geometry that was
  // discarded still determined the winding of the geometry that was kept.
  AddTest(tests, "Rasterizer/GeometryLeftOfTheClipStillWindsThePixelsThatRemain", [] {
    PathRasterizer rasterizer;
    const auto& spans = rasterizer.Rasterize(RectPath(FloatRect{-1000.0f, 0.0f, 2000.0f, 4.0f}),
                                             FillRule::NonZero, IntRect{0, 0, 8, 8});
    for (int x = 0; x < 8; ++x) {
      ExpectEqInt(CoverageAt(spans, x, 0), 255,
                  "a shape that starts off the left edge must still fill what is visible; "
                  "dropping its winding leaves a hole rather than a missing sliver");
    }
    Expect(std::abs(CoveredArea(spans) - 32.0) < 0.5, "the visible area is the clip intersection");
  });

  AddTest(tests, "Rasterizer/ShapesEntirelyOutsideTheClipProduceNothing", [] {
    PathRasterizer rasterizer;
    const IntRect clip{0, 0, 8, 8};
    Expect(rasterizer.Rasterize(RectPath(FloatRect{20, 0, 4, 4}), FillRule::NonZero, clip).empty(),
           "a shape right of the clip is invisible");
    Expect(rasterizer.Rasterize(RectPath(FloatRect{-20, 0, 4, 4}), FillRule::NonZero, clip).empty(),
           "a shape left of the clip is invisible");
    Expect(rasterizer.Rasterize(RectPath(FloatRect{0, -20, 4, 4}), FillRule::NonZero, clip).empty(),
           "a shape above the clip is invisible");
    Expect(rasterizer.Rasterize(RectPath(FloatRect{0, 20, 4, 4}), FillRule::NonZero, clip).empty(),
           "a shape below the clip is invisible");
  });

  AddTest(tests, "Rasterizer/SpansStayInsideTheClipAndAreOrdered", [] {
    Path path;
    path.AddEllipse(FloatRect{-30.0f, -30.0f, 120.0f, 120.0f});
    PathRasterizer rasterizer;
    const IntRect clip{2, 3, 20, 15};
    const auto& spans = rasterizer.Rasterize(path, FillRule::NonZero, clip);

    Expect(!spans.empty(), "a circle covering the clip must produce spans");
    int previous_y = clip.Top() - 1;
    int previous_end = 0;
    for (const CoverageSpan& span : spans) {
      Expect(span.x >= clip.Left() && span.x + span.length <= clip.Right(),
             "a span outside the clip is a heap write in the blitter");
      Expect(span.y >= clip.Top() && span.y < clip.Bottom(), "a span row outside the clip");
      Expect(span.length > 0 && span.coverage > 0, "empty and invisible spans are not emitted");
      Expect(span.y > previous_y || (span.y == previous_y && span.x >= previous_end),
             "spans arrive top-to-bottom, left-to-right, and never overlap");
      if (span.y != previous_y) {
        previous_y = span.y;
        previous_end = clip.Left();
      }
      previous_end = span.x + span.length;
    }
  });

  AddTest(tests, "Rasterizer/AClipOffsetFromTheOriginRendersTheSamePixels", [] {
    Path path;
    path.AddEllipse(FloatRect{4.0f, 4.0f, 24.0f, 24.0f});
    PathRasterizer rasterizer;
    const std::vector<CoverageSpan> full =
        rasterizer.Rasterize(path, FillRule::NonZero, IntRect{0, 0, 32, 32});
    const std::vector<CoverageSpan> windowed =
        rasterizer.Rasterize(path, FillRule::NonZero, IntRect{10, 10, 6, 6});

    for (const CoverageSpan& span : windowed) {
      for (int x = span.x; x < span.x + span.length; ++x) {
        ExpectEqInt(span.coverage, CoverageAt(full, x, span.y),
                    "a narrowed clip must change which pixels are reported, never their value; "
                    "otherwise a partial repaint does not match a full one");
      }
    }
  });

  AddTest(tests, "Rasterizer/IsDeterministic", [] {
    Path path;
    path.AddRoundedRect(FloatRect{1.5f, 2.25f, 27.0f, 19.0f}, 4.0f, 8.0f, 0.0f, 3.5f);
    PathRasterizer first;
    PathRasterizer second;
    const std::vector<CoverageSpan> a =
        first.Rasterize(path, FillRule::NonZero, IntRect{0, 0, 32, 24});
    const std::vector<CoverageSpan> b =
        second.Rasterize(path, FillRule::NonZero, IntRect{0, 0, 32, 24});
    Expect(a == b, "golden reference tests depend on this and nothing else guarantees it");
  });

  AddTest(tests, "Rasterizer/ReusesItsArenaAcrossCalls", [] {
    Path path;
    path.AddEllipse(FloatRect{0.0f, 0.0f, 64.0f, 64.0f});
    PathRasterizer rasterizer;
    rasterizer.Rasterize(path, FillRule::NonZero, IntRect{0, 0, 64, 64});
    const std::size_t arena = rasterizer.ArenaBytes();
    Expect(arena > 0, "the first fill allocates");
    for (int i = 0; i < 8; ++i) {
      rasterizer.Rasterize(path, FillRule::NonZero, IntRect{0, 0, 64, 64});
    }
    ExpectEqInt(static_cast<long long>(rasterizer.ArenaBytes()), static_cast<long long>(arena),
                "steady-state painting must not keep allocating");
  });

  // Hostile geometry. None of these should be slow, and none should read or
  // write outside the clip. The assertion is that they terminate at all with a
  // bounded result — a coordinate walk driven by the input rather than by the
  // clip would hang here.
  AddTest(tests, "Rasterizer/AbsurdCoordinatesStayBoundedByTheClipRatherThanTheInput", [] {
    PathRasterizer rasterizer;
    const IntRect clip{0, 0, 16, 16};

    const auto& huge = rasterizer.Rasterize(RectPath(FloatRect{-1e30f, -1e30f, 2e30f, 2e30f}),
                                            FillRule::NonZero, clip);
    Expect(std::abs(CoveredArea(huge) - 256.0) < 1.0,
           "a rect covering the universe covers exactly the clip");

    Path steep;
    steep.MoveTo(FloatPoint{-1e20f, -1e20f});
    steep.LineTo(FloatPoint{1e20f, 1e20f});
    steep.LineTo(FloatPoint{8.0f, 0.0f});
    steep.Close();
    const auto& spans = rasterizer.Rasterize(steep, FillRule::NonZero, clip);
    for (const CoverageSpan& span : spans) {
      Expect(span.x >= 0 && span.x + span.length <= 16 && span.y >= 0 && span.y < 16,
             "extreme coordinates must not escape the clip");
    }
  });

  AddTest(tests, "Rasterizer/SubPixelShapesStillProduceCoverage", [] {
    // A hairline thinner than a pixel must not vanish: this is the case that
    // separates a coverage rasterizer from a scan-conversion one, and it is
    // what a 1px CSS border becomes at a fractional device scale.
    PathRasterizer rasterizer;
    const auto& spans = rasterizer.Rasterize(RectPath(FloatRect{2.0f, 2.0f, 8.0f, 0.05f}),
                                             FillRule::NonZero, IntRect{0, 0, 16, 16});
    Expect(!spans.empty(), "a 0.05px-tall rect is faint, not absent");
    Expect(std::abs(CoveredArea(spans) - 0.4) < 0.05, "and its coverage is its area");
  });
}

}  // namespace microbrowser::tests

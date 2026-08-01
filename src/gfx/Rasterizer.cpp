#include "gfx/Rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gfx/PathFlattener.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// 24.8 fixed point. Eight subpixel bits is what ftgrays uses and what 8-bit
// coverage can distinguish: the accumulated area is exact in this grid, so the
// only quantization in the output is the final 0..255 range.
constexpr int kPixelBits = 8;
constexpr int kOnePixel = 1 << kPixelBits;

int ToFixed(double v) {
  return static_cast<int>(std::lround(v * static_cast<double>(kOnePixel)));
}

// Clipping runs in double, before the fixed-point conversion, and that ordering
// is the point rather than an accident.
//
// A path coordinate is finite (Path guarantees it) but may be enormous — 1e30
// is one CSS `transform: scale()` away. Converting first and clipping after
// would overflow the fixed-point grid; clamping the coordinate first would tilt
// the segment, which moves the parts of it that *are* on screen. Interpolating
// in double and clipping to the target box leaves every surviving coordinate
// inside the surface, where the conversion is exact and the walk below is
// bounded by the clip rather than by the input.
struct DPoint {
  double x = 0.0;
  double y = 0.0;
};

double XAtY(DPoint a, DPoint b, double y) {
  return a.x + (b.x - a.x) * ((y - a.y) / (b.y - a.y));
}

double YAtX(DPoint a, DPoint b, double x) {
  return a.y + (b.y - a.y) * ((x - a.x) / (b.x - a.x));
}

// Turns the flattener's polyline into accumulation cells.
//
// Fills every contour whether or not it was closed: an unclosed contour has no
// meaning for an area, so the closing edge is implied. Only the stroker cares
// about the difference.
class CellAccumulator {
 public:
  CellAccumulator(std::vector<RasterCell>& cells, const IntRect& clip)
      : cells_(cells), clip_(clip) {}

  void BeginContour(FloatPoint p) {
    start_ = ToDouble(p);
    current_ = start_;
  }

  void LineTo(FloatPoint p) {
    const DPoint next = ToDouble(p);
    ClipAndEmit(current_, next);
    current_ = next;
    ++segments_;
  }

  void EndContour(bool) {
    ClipAndEmit(current_, start_);
    current_ = start_;
    ++segments_;
  }

  std::uint64_t Segments() const { return segments_; }

 private:
  static DPoint ToDouble(FloatPoint p) {
    return DPoint{static_cast<double>(p.x), static_cast<double>(p.y)};
  }

  double Left() const { return static_cast<double>(clip_.Left()); }
  double Top() const { return static_cast<double>(clip_.Top()); }
  double Right() const { return static_cast<double>(clip_.Right()); }
  double Bottom() const { return static_cast<double>(clip_.Bottom()); }

  void ClipAndEmit(DPoint a, DPoint b) {
    // A horizontal edge crosses no scanline, so it contributes no winding and
    // no area. Dropping it here is not an optimization: the walk below divides
    // by the vertical extent.
    if (a.y == b.y) {
      return;
    }
    if ((a.y <= Top() && b.y <= Top()) || (a.y >= Bottom() && b.y >= Bottom())) {
      return;
    }

    DPoint p0 = a;
    DPoint p1 = b;
    if (p0.y < Top()) {
      p0 = DPoint{XAtY(a, b, Top()), Top()};
    } else if (p0.y > Bottom()) {
      p0 = DPoint{XAtY(a, b, Bottom()), Bottom()};
    }
    if (p1.y < Top()) {
      p1 = DPoint{XAtY(a, b, Top()), Top()};
    } else if (p1.y > Bottom()) {
      p1 = DPoint{XAtY(a, b, Bottom()), Bottom()};
    }
    if (p0.y == p1.y) {
      return;
    }
    ClipInX(p0, p1);
  }

  // Horizontal clipping is asymmetric, and both halves matter.
  //
  // Geometry left of the clip is *projected* onto the left edge rather than
  // dropped: the sweep accumulates winding left to right, so discarding it
  // would leave every visible pixel of a scrolled-in shape with the wrong
  // winding number — a hole, not a missing sliver. Geometry right of the clip
  // is dropped, because winding there can only affect pixels further right,
  // and there are none.
  void ClipInX(DPoint p0, DPoint p1) {
    if (p0.x == p1.x) {
      double x = p0.x;
      if (x >= Right()) {
        return;
      }
      x = std::max(x, Left());
      EmitLine(x, p0.y, x, p1.y);
      return;
    }

    if (p0.x < p1.x) {
      if (p1.x <= Left()) {
        EmitLine(Left(), p0.y, Left(), p1.y);
        return;
      }
      if (p0.x >= Right()) {
        return;
      }
      DPoint s = p0;
      DPoint e = p1;
      if (s.x < Left()) {
        const double y_left = YAtX(p0, p1, Left());
        EmitLine(Left(), s.y, Left(), y_left);
        s = DPoint{Left(), y_left};
      }
      if (e.x > Right()) {
        e = DPoint{Right(), YAtX(p0, p1, Right())};
      }
      EmitLine(s.x, s.y, e.x, e.y);
      return;
    }

    if (p0.x <= Left()) {
      EmitLine(Left(), p0.y, Left(), p1.y);
      return;
    }
    if (p1.x >= Right()) {
      return;
    }
    DPoint s = p0;
    DPoint e = p1;
    if (s.x > Right()) {
      s = DPoint{Right(), YAtX(p0, p1, Right())};
    }
    if (e.x < Left()) {
      e = DPoint{Left(), YAtX(p0, p1, Left())};
    }
    EmitLine(s.x, s.y, e.x, e.y);
    if (p1.x < Left()) {
      EmitLine(Left(), e.y, Left(), p1.y);
    }
  }

  void EmitLine(double x0, double y0, double x1, double y1) {
    const int fixed_y0 = ToFixed(y0);
    const int fixed_y1 = ToFixed(y1);
    if (fixed_y0 == fixed_y1) {
      return;
    }
    WalkScanlines(ToFixed(x0), fixed_y0, ToFixed(x1), fixed_y1);
  }

  // Splits the segment at every horizontal pixel boundary it crosses, so each
  // piece handed to EmitScanline lies within one pixel row.
  void WalkScanlines(int x0, int y0, int x1, int y1) {
    const bool down = y1 > y0;
    const int step = down ? 1 : -1;
    int current_x = x0;
    int current_y = y0;
    // Moving up out of a point that sits exactly on a boundary puts us in the
    // row above it, not the row it names.
    int row = down ? (y0 >> kPixelBits) : ((y0 - 1) >> kPixelBits);

    while (current_y != y1) {
      const int boundary = (down ? row + 1 : row) * kOnePixel;
      int next_x = x1;
      int next_y = y1;
      if (down ? (boundary < y1) : (boundary > y1)) {
        next_y = boundary;
        next_x = x0 + static_cast<int>(static_cast<std::int64_t>(x1 - x0) * (next_y - y0) /
                                       (y1 - y0));
      }
      EmitScanline(row, current_x, current_y - row * kOnePixel, next_x, next_y - row * kOnePixel);
      current_x = next_x;
      current_y = next_y;
      row += step;
    }
  }

  // One pixel row. `fy0`/`fy1` are the fractional heights within the row; x is
  // still absolute so the pixel column falls out of a shift.
  void EmitScanline(int ey, int x0, int fy0, int x1, int fy1) {
    const int dy = fy1 - fy0;
    if (dy == 0) {
      return;
    }
    const int ex0 = x0 >> kPixelBits;
    const int ex1 = x1 >> kPixelBits;
    if (ex0 == ex1) {
      AddCell(ex0, ey, dy, ((x0 - ex0 * kOnePixel) + (x1 - ex0 * kOnePixel)) * dy);
      return;
    }

    const int step = ex1 > ex0 ? 1 : -1;
    const int exit_fx = step > 0 ? kOnePixel : 0;
    int current_x = x0;
    int current_y = fy0;
    for (int ex = ex0; ex != ex1; ex += step) {
      const int boundary = (step > 0 ? ex + 1 : ex) * kOnePixel;
      const int next_y =
          fy0 + static_cast<int>(static_cast<std::int64_t>(dy) * (boundary - x0) / (x1 - x0));
      AddCell(ex, ey, next_y - current_y,
              ((current_x - ex * kOnePixel) + exit_fx) * (next_y - current_y));
      current_x = boundary;
      current_y = next_y;
    }
    AddCell(ex1, ey, fy1 - current_y,
            ((current_x - ex1 * kOnePixel) + (x1 - ex1 * kOnePixel)) * (fy1 - current_y));
  }

  void AddCell(int ex, int ey, int cover, int area) {
    if (cover == 0 && area == 0) {
      return;
    }
    if (ex >= clip_.Right()) {
      return;
    }
    if (ex < clip_.Left()) {
      // Folding an off-left cell onto the clip edge with zero area is exactly
      // the projection described above: the edge sits at the pixel's left
      // boundary, so it covers all of it.
      ex = clip_.Left();
      area = 0;
    }
    cells_.push_back(RasterCell{ex, ey, cover, area});
  }

  std::vector<RasterCell>& cells_;
  IntRect clip_;
  DPoint start_;
  DPoint current_;
  std::uint64_t segments_ = 0;
};

// Accumulated area (in 2 * kOnePixel^2 units) to an 0..255 coverage value under
// the requested fill rule.
std::uint8_t CoverageOf(std::int64_t area, FillRule rule) {
  std::int64_t coverage = area >> (kPixelBits * 2 + 1 - 8);
  if (coverage < 0) {
    coverage = -coverage;
  }
  if (rule == FillRule::EvenOdd) {
    // Winding 2 must read as outside, 3 as inside, and so on: the accumulated
    // value folds back on itself every two turns.
    coverage &= 511;
    if (coverage > 256) {
      coverage = 512 - coverage;
    } else if (coverage == 256) {
      coverage = 255;
    }
  } else if (coverage >= 256) {
    coverage = 255;
  }
  return static_cast<std::uint8_t>(coverage);
}

}  // namespace

const std::vector<CoverageSpan>& PathRasterizer::Rasterize(const Path& path, FillRule rule,
                                                           const IntRect& clip,
                                                           const AffineTransform& transform,
                                                           float tolerance) {
  AddPerformanceCounter(PerfCounterId::GfxPathFills);
  spans_.clear();
  cells_.clear();
  if (path.IsEmpty() || clip.IsEmpty()) {
    return spans_;
  }

  CellAccumulator accumulator(cells_, clip);
  FlattenPath(path, transform, tolerance, accumulator);
  AddPerformanceCounter(PerfCounterId::GfxPathSegments, accumulator.Segments());
  AddPerformanceCounter(PerfCounterId::GfxPathCells, cells_.size());
  if (cells_.empty()) {
    return spans_;
  }

  std::sort(cells_.begin(), cells_.end(), [](const RasterCell& a, const RasterCell& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });

  const auto emit = [this](std::int32_t x, std::int32_t y, std::int32_t length,
                           std::uint8_t coverage) {
    if (coverage == 0 || length <= 0) {
      return;
    }
    // Merging with the previous span matters more than it looks: the common
    // shape is an antialiased edge pixel followed by a solid interior run, and
    // an unmerged boundary would double the blitter's per-span overhead on
    // every scanline of every fill.
    if (!spans_.empty()) {
      CoverageSpan& last = spans_.back();
      if (last.y == y && last.coverage == coverage && last.x + last.length == x) {
        last.length += length;
        return;
      }
    }
    spans_.push_back(CoverageSpan{x, y, length, coverage});
  };

  const std::size_t cell_count = cells_.size();
  std::size_t index = 0;
  while (index < cell_count) {
    const std::int32_t y = cells_[index].y;
    std::int64_t winding = 0;
    std::int32_t x = clip.Left();

    while (index < cell_count && cells_[index].y == y) {
      const std::int32_t cell_x = cells_[index].x;
      std::int64_t cell_cover = 0;
      std::int64_t cell_area = 0;
      while (index < cell_count && cells_[index].y == y && cells_[index].x == cell_x) {
        cell_cover += cells_[index].cover;
        cell_area += cells_[index].area;
        ++index;
      }

      if (cell_x > x && winding != 0) {
        emit(x, y, cell_x - x, CoverageOf(winding * (kOnePixel * 2), rule));
      }
      winding += cell_cover;
      emit(cell_x, y, 1, CoverageOf(winding * (kOnePixel * 2) - cell_area, rule));
      x = cell_x + 1;
    }

    // A shape whose right edge is outside the clip has no closing cell, so the
    // run to the clip edge is the whole visible remainder of the interior.
    if (winding != 0 && x < clip.Right()) {
      emit(x, y, clip.Right() - x, CoverageOf(winding * (kOnePixel * 2), rule));
    }
  }

  AddPerformanceCounter(PerfCounterId::GfxPathSpans, spans_.size());
  for (const CoverageSpan& span : spans_) {
    AddPerformanceCounter(PerfCounterId::GfxPathSpanPixels,
                          static_cast<std::uint64_t>(span.length));
  }
  return spans_;
}

std::size_t PathRasterizer::ArenaBytes() const {
  return cells_.capacity() * sizeof(RasterCell) + spans_.capacity() * sizeof(CoverageSpan);
}

}  // namespace microbrowser::gfx

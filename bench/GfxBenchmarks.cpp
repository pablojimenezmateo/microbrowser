#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "BenchSupport.h"
#include "gfx/Blitter.h"
#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"

namespace microbrowser::bench {

namespace {

using gfx::Canvas;
using gfx::Color;
using gfx::FillRule;
using gfx::FloatRect;
using gfx::IntRect;
using gfx::Painter;
using gfx::Path;
using gfx::PathRasterizer;

constexpr int kSurfaceWidth = 1280;
constexpr int kSurfaceHeight = 800;

// The shape a page of text blocks has before there is any text: forty
// full-width rounded boxes. Chosen because it is what the engine actually
// emits, not because it flatters the rasterizer.
std::vector<Path> PageOfBoxes() {
  std::vector<Path> boxes;
  for (int i = 0; i < 40; ++i) {
    Path box;
    box.AddRoundedRect(
        FloatRect{32.0f, 40.0f + static_cast<float>(i) * 19.0f, 1216.0f, 18.0f},
        4.0f, 4.0f, 4.0f, 4.0f);
    boxes.push_back(std::move(box));
  }
  return boxes;
}

Path LargeCircle() {
  Path circle;
  circle.AddEllipse(FloatRect{20.0f, 20.0f, 760.0f, 760.0f});
  return circle;
}

// Long-lived state, so the benchmark bodies stay closures over stable
// references rather than rebuilding a canvas per iteration.
struct GfxFixtures {
  std::vector<Path> boxes = PageOfBoxes();
  Path circle = LargeCircle();
  Canvas canvas{kSurfaceWidth, kSurfaceHeight};
  Painter painter{canvas};
  PathRasterizer rasterizer;
  std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(1u << 20, 0xFF203040u);
  IntRect clip{0, 0, kSurfaceWidth, kSurfaceHeight};
};

GfxFixtures& Fixtures() {
  static GfxFixtures fixtures;
  return fixtures;
}

// Coverage of the large circle, so per-pixel costs can be reported against the
// number of pixels the fill actually touches.
std::size_t CirclePixels() {
  GfxFixtures& f = Fixtures();
  const auto& spans = f.rasterizer.Rasterize(f.circle, FillRule::NonZero, f.clip);
  std::size_t total = 0;
  for (const gfx::CoverageSpan& span : spans) {
    total += static_cast<std::size_t>(span.length);
  }
  return total;
}

}  // namespace

void RegisterGfxBenchmarks(std::vector<Benchmark>& benchmarks) {
  GfxFixtures& f = Fixtures();
  const std::size_t circle_pixels = CirclePixels();
  const std::size_t span_pixels = f.pixels.size();

  AddBenchmark(benchmarks, "raster/page-of-40-rounded-boxes", 0, "", [&f] {
    for (const Path& box : f.boxes) {
      f.rasterizer.Rasterize(box, FillRule::NonZero, f.clip);
    }
  });

  AddBenchmark(benchmarks, "raster/circle-760px", circle_pixels, "px", [&f] {
    f.rasterizer.Rasterize(f.circle, FillRule::NonZero, f.clip);
  });

  AddBenchmark(benchmarks, "paint/page-of-40-rounded-boxes-translucent", 0, "", [&f] {
    for (const Path& box : f.boxes) {
      f.painter.FillPath(box, Color::Rgba(0x20, 0x20, 0x28, 0x40));
    }
  });

  AddBenchmark(benchmarks, "paint/circle-760px-opaque", circle_pixels, "px", [&f] {
    f.painter.FillPath(f.circle, Color::Rgb(0x20, 0x20, 0x28));
  });

  AddBenchmark(benchmarks, "paint/circle-760px-translucent", circle_pixels, "px", [&f] {
    f.painter.FillPath(f.circle, Color::Rgba(0x20, 0x20, 0x28, 0x80));
  });

  AddBenchmark(benchmarks, "paint/circle-760px-stroked", 0, "", [&f] {
    gfx::StrokeStyle style;
    style.width = 6.0f;
    style.join = gfx::LineJoin::Round;
    f.painter.StrokePath(f.circle, style, Color::Rgb(0x20, 0x20, 0x28));
  });

  // The page-level A/B the performance notes quote. Both variants rasterize the
  // same geometry and differ only in which blitter consumes the spans, so the
  // difference between them is the blend and nothing else. Without this pair
  // the claim "blending was most of the frame" would rest on subtracting two
  // benchmarks that were never run against each other.
  const auto paint_page = [&f](bool vector) {
    for (const Path& box : f.boxes) {
      const auto& spans = f.rasterizer.Rasterize(box, FillRule::NonZero, f.clip);
      for (const gfx::CoverageSpan& span : spans) {
        const Color source = Color::Rgba(0x20, 0x20, 0x28, 0x40);
        const Color effective = source.WithAlpha(
            static_cast<std::uint8_t>(gfx::MulDiv255(source.Alpha(), span.coverage)));
        std::uint32_t* row = f.canvas.Row(span.y) + span.x;
        if (vector) {
          gfx::BlendSpanSrcOver(row, static_cast<std::size_t>(span.length), effective);
        } else {
          gfx::BlendSpanSrcOverScalar(row, static_cast<std::size_t>(span.length), effective);
        }
      }
    }
  };

  AddBenchmark(benchmarks, "paint/page-blend-vector", 0, "",
               [paint_page] { paint_page(true); });
  AddBenchmark(benchmarks, "paint/page-blend-scalar", 0, "",
               [paint_page] { paint_page(false); });

  // The two implementations side by side. Keeping the scalar one in the report
  // is what makes a regression in the vector path visible as a ratio rather
  // than as a number nobody remembers.
  AddBenchmark(benchmarks, "blit/span-srcover-vector", span_pixels, "px", [&f] {
    gfx::BlendSpanSrcOver(f.pixels.data(), f.pixels.size(), Color::Rgba(0x10, 0x80, 0xF0, 0x80));
  });

  AddBenchmark(benchmarks, "blit/span-srcover-scalar", span_pixels, "px", [&f] {
    gfx::BlendSpanSrcOverScalar(f.pixels.data(), f.pixels.size(),
                                Color::Rgba(0x10, 0x80, 0xF0, 0x80));
  });

  AddBenchmark(benchmarks, "blit/span-srcover-opaque-source", span_pixels, "px", [&f] {
    gfx::BlendSpanSrcOver(f.pixels.data(), f.pixels.size(), Color::Rgb(0x10, 0x80, 0xF0));
  });
}

}  // namespace microbrowser::bench

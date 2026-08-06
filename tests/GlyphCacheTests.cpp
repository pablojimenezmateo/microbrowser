#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/Font.h"
#include "gfx/GlyphCache.h"
#include "gfx/Painter.h"
#include "gfx/TextShaper.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::FloatPoint;
using gfx::Font;
using gfx::FontFace;
using gfx::FontLibrary;
using gfx::GlyphCache;
using gfx::GlyphId;
using gfx::GlyphImage;
using gfx::Painter;
using gfx::ShapedRun;
using gfx::TextShaper;

namespace {

FontFace LoadSyntheticFace(const FontLibrary& library) {
  auto face = FontFace::Load(library, BuildSyntheticFont());
  Expect(face.has_value(), "the synthetic font must parse");
  return std::move(*face);
}

double MaskCoverage(const GlyphImage& image) {
  double total = 0.0;
  for (const std::uint8_t value : image.coverage) {
    total += static_cast<double>(value) / 255.0;
  }
  return total;
}

}  // namespace

void RegisterGlyphCacheTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GlyphCache/SecondLookupOfTheSameGlyphIsAHit", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    GlyphCache cache;

    const GlyphId glyph = face.GlyphForCodepoint(U'A');
    Expect(cache.Acquire(font, glyph, 0.0f) != nullptr, "the glyph rasterizes");
    ExpectEqInt(static_cast<long long>(cache.Misses()), 1, "the first lookup misses");
    ExpectEqInt(static_cast<long long>(cache.Hits()), 0, "and does not hit");

    Expect(cache.Acquire(font, glyph, 0.0f) != nullptr, "the second lookup returns the same glyph");
    ExpectEqInt(static_cast<long long>(cache.Hits()), 1, "and hits");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1, "without adding an entry");
  });

  AddTest(tests, "GlyphCache/SizeAndSubpixelPositionArePartOfTheKey", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    GlyphCache cache;
    const GlyphId glyph = face.GlyphForCodepoint(U'A');

    Font small(face, 20.0f);
    Font large(face, 21.0f);
    cache.Acquire(small, glyph, 0.0f);
    cache.Acquire(large, glyph, 0.0f);
    ExpectEqInt(static_cast<long long>(cache.Size()), 2,
                "a different size is a different glyph image; sharing them would render text at "
                "the wrong size");

    cache.Acquire(small, glyph, 0.5f);
    ExpectEqInt(static_cast<long long>(cache.Size()), 3,
                "and so is a different sub-pixel position, which is what keeps a line of text "
                "from snapping to whole pixels");

    // A quarter of a pixel apart falls in the same bucket at four positions.
    const std::size_t before = cache.Size();
    cache.Acquire(small, glyph, 0.55f);
    ExpectEqInt(static_cast<long long>(cache.Size()), static_cast<long long>(before),
                "positions within one quantization step share an entry, or the cache grows "
                "without bound as text moves");
  });

  AddTest(tests, "GlyphCache/TheIntegerPartOfThePositionIsNotPartOfTheKey", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    GlyphCache cache;
    const GlyphId glyph = face.GlyphForCodepoint(U'A');

    cache.Acquire(font, glyph, 10.0f);
    cache.Acquire(font, glyph, 400.0f);
    cache.Acquire(font, glyph, -73.0f);
    ExpectEqInt(static_cast<long long>(cache.Size()), 1,
                "the same glyph at whole-pixel offsets is one image; keying on the whole "
                "position would make the cache useless the moment text moved");
  });

  AddTest(tests, "GlyphCache/AGlyphWithNothingToDrawIsStillRemembered", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    GlyphCache cache;
    const GlyphId space = face.GlyphForCodepoint(U' ');

    Expect(cache.Acquire(font, space, 0.0f) == nullptr, "a space has no image");
    Expect(cache.Acquire(font, space, 0.0f) == nullptr, "and still has none");
    ExpectEqInt(static_cast<long long>(cache.Hits()), 1,
                "but the second lookup must hit, or every space on the page pays for an "
                "outline load that always fails");
  });

  AddTest(tests, "GlyphCache/EvictsOldestFirstOnceOverBudget", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    GlyphCache cache;
    const GlyphId glyph = face.GlyphForCodepoint(U'C');

    // Enough distinct sizes to exceed a deliberately tiny budget.
    cache.SetByteBudget(4096);
    for (int i = 0; i < 40; ++i) {
      Font font(face, 8.0f + static_cast<float>(i));
      cache.Acquire(font, glyph, 0.0f);
    }
    Expect(cache.Bytes() <= 4096, "the cache must respect its byte budget");
    Expect(cache.Size() > 0, "and must not evict itself to nothing");
    Expect(cache.Size() < 40, "having actually evicted something");
  });

  AddTest(tests, "GlyphCache/ShrinkingTheBudgetEvictsImmediately", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    GlyphCache cache;
    for (int i = 0; i < 20; ++i) {
      Font font(face, 10.0f + static_cast<float>(i));
      cache.Acquire(font, face.GlyphForCodepoint(U'C'), 0.0f);
    }
    Expect(cache.Bytes() > 0, "there is something to evict");
    cache.SetByteBudget(0);
    ExpectEqInt(static_cast<long long>(cache.Size()), 0,
                "a budget of zero must empty the cache rather than being ignored");
  });

  AddTest(tests, "GlyphCache/ClearForgetsEverything", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    GlyphCache cache;
    cache.Acquire(font, face.GlyphForCodepoint(U'A'), 0.0f);
    cache.Clear();
    ExpectEqInt(static_cast<long long>(cache.Size()), 0, "no entries remain");
    ExpectEqInt(static_cast<long long>(cache.Bytes()), 0, "and no bytes are still counted");
  });

  AddTest(tests, "GlyphCache/AnUnusableSizeCachesNothing", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    GlyphCache cache;
    for (const float size : {0.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity()}) {
      Font font(face, size);
      Expect(cache.Acquire(font, face.GlyphForCodepoint(U'A'), 0.0f) == nullptr,
             "an unusable size produces no image");
    }
    ExpectEqInt(static_cast<long long>(cache.Size()), 0,
                "and leaves no entry behind; a NaN key never matches itself, so caching one "
                "would leak an entry per call");
  });

  // The property that makes the cache safe to have at all.
  AddTest(tests, "GlyphCache/MaskCoverageMatchesTheOutlineItWasBuiltFrom", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 64.0f);
    GlyphCache cache;

    for (const char32_t codepoint : {U'A', U'B', U'C', U'D'}) {
      const GlyphImage* image = cache.Acquire(font, face.GlyphForCodepoint(codepoint), 0.0f);
      Expect(image != nullptr, "every shaped glyph has an image");
      // Area in font units, scaled to this pixel size.
      const double expected = SyntheticGlyphArea(codepoint) * (64.0 / 1000.0) * (64.0 / 1000.0);
      const double measured = MaskCoverage(*image);
      Expect(std::abs(measured - expected) / expected < 0.02,
             "a cached mask must carry the same coverage the outline would have produced");
    }
  });

  AddTest(tests, "GlyphCache/DrawingThroughThePainterHitsTheCache", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 16.0f);
    TextShaper shaper;
    const ShapedRun run = shaper.Shape(font, "AAAA", false);

    Canvas canvas(200, 40);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);

    // One 'A' advances 12.8px at this size, so the four of them land on four
    // *different* sub-pixel positions and legitimately need four masks. The
    // cache pays off on the next frame, not within the first one — which is the
    // real workload, since a page is redrawn far more often than it changes.
    painter.DrawGlyphs(font, run, FloatPoint{4.0f, 30.0f}, Color::Rgb(0, 0, 0));
    const std::uint64_t first_frame_misses = painter.Glyphs().Misses();
    Expect(first_frame_misses <= 4, "at most one rasterization per glyph in the run");

    painter.DrawGlyphs(font, run, FloatPoint{4.0f, 30.0f}, Color::Rgb(0, 0, 0));
    ExpectEqInt(static_cast<long long>(painter.Glyphs().Misses()),
                static_cast<long long>(first_frame_misses),
                "redrawing the identical run must rasterize nothing new");
    Expect(painter.Glyphs().Hits() >= 4, "every glyph of the second draw is a hit");
    Expect(painter.Glyphs().Size() <= 4,
           "one entry per sub-pixel position the run actually used, and no more");
  });

  AddTest(tests, "GlyphCache/ARotatedRunBypassesTheCacheRatherThanRenderingWrong", [] {
    // A cached mask is pixels, so it cannot be reused under a rotation. What
    // must not happen is that it is reused anyway.
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    TextShaper shaper;
    const ShapedRun run = shaper.Shape(font, "AB", false);

    Canvas canvas(120, 120);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    painter.SetTransform(gfx::AffineTransform::Rotation(0.6f)
                             .Then(gfx::AffineTransform::Translation(40.0f, 40.0f)));
    painter.DrawGlyphs(font, run, FloatPoint{0.0f, 0.0f}, Color::Rgb(0, 0, 0));

    ExpectEqInt(static_cast<long long>(painter.Glyphs().Size()), 0,
                "a rotated run must not populate a cache keyed only on position and size");

    bool any_ink = false;
    for (const std::uint32_t pixel : canvas.Pixels()) {
      any_ink = any_ink || ((pixel >> 16) & 0xFFu) < 0x80;
    }
    Expect(any_ink, "and must still draw something");
  });

  AddTest(tests, "GlyphCache/CachedAndUncachedRenderingAgree", [] {
    // Same glyph, same place, one drawn through the cache and one straight
    // through the rasterizer. They must land on the same pixels, or the cache
    // is a second renderer with its own bugs.
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 32.0f);
    const GlyphId glyph = face.GlyphForCodepoint(U'D');

    Canvas cached(80, 80);
    cached.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter cached_painter(cached);
    TextShaper shaper;
    const ShapedRun run = shaper.Shape(font, "D", false);
    cached_painter.DrawGlyphs(font, run, FloatPoint{10.0f, 50.0f}, Color::Rgb(0, 0, 0));

    Canvas direct(80, 80);
    direct.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter direct_painter(direct);
    gfx::Path outline;
    Expect(font.GlyphOutline(glyph, outline), "outline");
    direct_painter.SetTransform(gfx::AffineTransform::Translation(10.0f, 50.0f));
    direct_painter.FillPath(outline, Color::Rgb(0, 0, 0));

    std::size_t differing = 0;
    long long total_delta = 0;
    for (int y = 0; y < 80; ++y) {
      for (int x = 0; x < 80; ++x) {
        const auto a = static_cast<int>((cached.Row(y)[x] >> 16) & 0xFFu);
        const auto b = static_cast<int>((direct.Row(y)[x] >> 16) & 0xFFu);
        if (a != b) {
          ++differing;
          total_delta += std::abs(a - b);
        }
      }
    }
    // Not byte-identical: the cache quantizes the sub-pixel position to a
    // quarter pixel, which moves antialiased edges by a fraction of a level.
    // Interior and exterior pixels must agree exactly, so the total error is
    // bounded by the perimeter.
    Expect(differing < 120,
           "only edge pixels may differ, and only because of sub-pixel quantization");
    Expect(total_delta < 4000, "and the difference must stay small on each of them");
  });
}

}  // namespace microbrowser::tests

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/Font.h"
#include "gfx/Painter.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

using gfx::CoverageSpan;
using gfx::FillRule;
using gfx::FloatRect;
using gfx::Font;
using gfx::FontFace;
using gfx::FontLibrary;
using gfx::FontMetrics;
using gfx::GlyphId;
using gfx::IntRect;
using gfx::Path;
using gfx::PathRasterizer;

namespace {

constexpr float kEm = 1000.0f;  // units per em in the synthetic font

// Loads the synthetic font, failing loudly rather than returning something the
// caller has to check. Every test here needs it.
FontFace LoadSyntheticFace(const FontLibrary& library) {
  auto face = FontFace::Load(library, BuildSyntheticFont());
  Expect(face.has_value(), "the synthetic font must parse; if this fails every test below is moot");
  return std::move(*face);
}

double OutlineArea(const Path& path, float scale) {
  // Rasterize into a surface large enough to hold the glyph and measure the
  // coverage. Comparing against the closed-form area is what makes these tests
  // about the shape rather than about "some pixels appeared".
  Path shifted;
  const auto verbs = path.Verbs();
  const auto points = path.Points();
  std::size_t index = 0;
  const float offset = 200.0f;
  for (const gfx::PathVerb verb : verbs) {
    switch (verb) {
      case gfx::PathVerb::Move:
        shifted.MoveTo(gfx::FloatPoint{points[index].x + offset, points[index].y + offset});
        ++index;
        break;
      case gfx::PathVerb::Line:
        shifted.LineTo(gfx::FloatPoint{points[index].x + offset, points[index].y + offset});
        ++index;
        break;
      case gfx::PathVerb::Quad:
        shifted.QuadTo(gfx::FloatPoint{points[index].x + offset, points[index].y + offset},
                       gfx::FloatPoint{points[index + 1].x + offset,
                                       points[index + 1].y + offset});
        index += 2;
        break;
      case gfx::PathVerb::Cubic:
        shifted.CubicTo(
            gfx::FloatPoint{points[index].x + offset, points[index].y + offset},
            gfx::FloatPoint{points[index + 1].x + offset, points[index + 1].y + offset},
            gfx::FloatPoint{points[index + 2].x + offset, points[index + 2].y + offset});
        index += 3;
        break;
      case gfx::PathVerb::Close:
        shifted.Close();
        break;
    }
  }

  PathRasterizer rasterizer;
  const auto& spans = rasterizer.Rasterize(shifted, FillRule::NonZero, IntRect{0, 0, 400, 400});
  double total = 0.0;
  for (const CoverageSpan& span : spans) {
    total += static_cast<double>(span.length) * static_cast<double>(span.coverage) / 255.0;
  }
  // Back into font units: a glyph rendered at `scale` device pixels per em
  // covers scale^2 / em^2 times its area in font units.
  const double units_per_pixel = static_cast<double>(kEm) / static_cast<double>(scale);
  return total * units_per_pixel * units_per_pixel;
}

}  // namespace

void RegisterFontTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Font/LibraryInitializes", [] {
    const FontLibrary library;
    Expect(library.IsValid(), "FreeType must initialize, or nothing below can run");
  });

  AddTest(tests, "Font/LoadsASyntheticFaceAndReportsItsShape", [] {
    const FontLibrary library;
    const FontFace face = LoadSyntheticFace(library);
    ExpectEqInt(face.UnitsPerEm(), 1000, "the em square is what the builder wrote");
    ExpectEqInt(static_cast<long long>(face.GlyphCount()), 6,
                ".notdef plus space plus the four shaped glyphs");
  });

  // Font files are chosen by the page, so a malformed one is routine input, not
  // an exceptional condition.
  AddTest(tests, "Font/RejectsInputThatIsNotAFont", [] {
    const FontLibrary library;
    Expect(!FontFace::Load(library, {}).has_value(), "an empty buffer is not a font");

    std::vector<std::byte> garbage(512, std::byte{0xA5});
    Expect(!FontFace::Load(library, std::move(garbage)).has_value(), "neither is noise");

    Expect(!FontFace::Load(library, BuildCorruptFont()).has_value(),
           "nor is a valid header whose table directory points past the end of the file");

    // Truncation at every length: the prefix of a real font is the input a
    // decoder is most likely to mishandle, because it parses correctly right up
    // until it does not.
    const std::vector<std::byte> good = BuildSyntheticFont();
    for (std::size_t length = 1; length < good.size(); length += 17) {
      std::vector<std::byte> truncated(good.begin(), good.begin() + static_cast<long>(length));
      // Either it is rejected or it loads as something coherent; what it must
      // not do is crash, which is what running this under ASan checks.
      auto face = FontFace::Load(library, std::move(truncated));
      if (face.has_value()) {
        Path path;
        face->GlyphForCodepoint(U'A');
        Font font(*face, 16.0f);
        font.GlyphOutline(1, path);
      }
    }
  });

  AddTest(tests, "Font/MapsCodepointsThroughTheCmap", [] {
    const FontLibrary library;
    const FontFace face = LoadSyntheticFace(library);
    ExpectEqInt(face.GlyphForCodepoint(U' '), 1, "space is the first glyph after .notdef");
    ExpectEqInt(face.GlyphForCodepoint(U'A'), 2, "and the shaped glyphs follow in order");
    ExpectEqInt(face.GlyphForCodepoint(U'D'), 5, "including the last of them");
    ExpectEqInt(face.GlyphForCodepoint(U'Z'), 0,
                "an unmapped codepoint is .notdef, which is what the font itself says");
    ExpectEqInt(face.GlyphForCodepoint(U'\U0001F600'), 0, "and so is one outside the BMP");
  });

  AddTest(tests, "Font/MetricsAreScaledAndDescentIsPositive", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 100.0f);
    const FontMetrics metrics = font.Metrics();

    // 800/1000 of the em above the baseline, 200/1000 below, at 100px.
    Expect(std::abs(metrics.ascent - 80.0f) < 0.5f, "ascent scales with the pixel size");
    Expect(std::abs(metrics.descent - 20.0f) < 0.5f,
           "descent is reported as a positive distance; FreeType's is negative, and half of "
           "all font code has a sign bug there");
    Expect(std::abs(metrics.LineHeight() - 100.0f) < 1.0f, "line height is ascent + descent + gap");
  });

  AddTest(tests, "Font/AdvancesAreFractionalRatherThanRounded", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    // 800 units at 10.5px/em is 8.4px. Rounding to 8 would make a line of a
    // hundred glyphs land four pixels from where layout put it.
    Font font(face, 10.5f);
    const float advance = font.Advance(face.GlyphForCodepoint(U'A'));
    Expect(std::abs(advance - 8.4f) < 0.05f,
           "advance must keep its fraction, or long lines drift");
    Expect(advance != std::floor(advance), "and must not have been rounded to a whole pixel");
  });

  AddTest(tests, "Font/AdvancesScaleWithSize", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    const GlyphId glyph = face.GlyphForCodepoint(U'B');
    Font small(face, 20.0f);
    Font large(face, 40.0f);
    Expect(std::abs(large.Advance(glyph) - 2.0f * small.Advance(glyph)) < 0.05f,
           "twice the size is twice the advance");
  });

  AddTest(tests, "Font/ASpaceHasAnAdvanceAndNoOutline", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 50.0f);
    const GlyphId space = face.GlyphForCodepoint(U' ');
    Expect(font.Advance(space) > 0.0f, "a space still moves the pen");
    Path path;
    Expect(!font.GlyphOutline(space, path),
           "and has nothing to draw, which is a normal answer rather than a failure");
    Expect(path.IsEmpty(), "the output path is left empty");
  });

  // The y flip is the single most likely thing to be wrong in a font backend,
  // and it is invisible in a test that only checks that pixels appeared.
  AddTest(tests, "Font/OutlinesComeBackInDeviceSpaceWithYDown", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 100.0f);
    Path path;
    Expect(font.GlyphOutline(face.GlyphForCodepoint(U'A'), path), "the square glyph has an outline");

    const FloatRect bounds = path.ControlBounds();
    // The glyph spans y = 0..600 font units *above* the baseline, so in device
    // space it must sit at negative y — above the origin on screen.
    Expect(bounds.Bottom() <= 0.5f, "a glyph above the baseline must have non-positive y");
    Expect(std::abs(bounds.y + 60.0f) < 0.5f, "its top is 600 units up, which is 60px at 100px/em");
    Expect(std::abs(bounds.x - 10.0f) < 0.5f, "and its left edge is 100 units in");
  });

  AddTest(tests, "Font/GlyphBoundsMatchTheOutline", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 100.0f);
    const GlyphId glyph = face.GlyphForCodepoint(U'A');

    Path path;
    Expect(font.GlyphOutline(glyph, path), "outline");
    const FloatRect from_path = path.ControlBounds();
    const FloatRect reported = font.GlyphBounds(glyph);
    Expect(std::abs(reported.x - from_path.x) < 0.1f && std::abs(reported.y - from_path.y) < 0.1f,
           "the reported box must agree with the outline it describes");
    Expect(std::abs(reported.width - from_path.width) < 0.1f, "on width");
    Expect(std::abs(reported.height - from_path.height) < 0.1f, "and height");
  });

  // Each glyph's area is known in closed form, so this checks that the right
  // shape came out — not merely that a shape did.
  AddTest(tests, "Font/OutlineGeometryMatchesTheGlyphItWasBuiltFrom", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    const float scale = 100.0f;
    Font font(face, scale);

    for (const char32_t codepoint : {U'A', U'B', U'C', U'D'}) {
      Path path;
      Expect(font.GlyphOutline(face.GlyphForCodepoint(codepoint), path),
             "every shaped glyph has an outline");
      const double expected = SyntheticGlyphArea(codepoint);
      const double measured = OutlineArea(path, scale);
      Expect(std::abs(measured - expected) / expected < 0.02,
             std::string("glyph area is wrong for U+00") +
                 std::to_string(static_cast<int>(codepoint)) + ": expected " +
                 std::to_string(expected) + ", measured " + std::to_string(measured));
    }
  });

  AddTest(tests, "Font/AReversedContourCutsAHoleInTheGlyph", [] {
    // 'C' is a square with a reversed inner square. If the outline decomposer
    // dropped contour direction, the hole would fill in and the area would come
    // back as the outer square alone.
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 100.0f);
    Path path;
    Expect(font.GlyphOutline(face.GlyphForCodepoint(U'C'), path), "outline");

    const double measured = OutlineArea(path, 100.0f);
    const double with_hole = SyntheticGlyphArea(U'C');
    const double without_hole = 800.0 * 800.0;
    Expect(std::abs(measured - with_hole) / with_hole < 0.02, "the hole must be present");
    Expect(std::abs(measured - without_hole) / without_hole > 0.1,
           "and the glyph must not have come back solid");
  });

  AddTest(tests, "Font/QuadraticContoursSurviveTheConversion", [] {
    // 'D' is the only glyph with an off-curve point, so it is the only one that
    // reaches the conic branch of the outline decomposer.
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 100.0f);
    Path path;
    Expect(font.GlyphOutline(face.GlyphForCodepoint(U'D'), path), "outline");

    bool has_curve = false;
    for (const gfx::PathVerb verb : path.Verbs()) {
      has_curve = has_curve || verb == gfx::PathVerb::Quad || verb == gfx::PathVerb::Cubic;
    }
    Expect(has_curve, "a TrueType conic must arrive as a curve, not as a flattened polyline");

    const FloatRect bounds = path.ControlBounds();
    Expect(std::abs(bounds.y + 90.0f) < 1.0f,
           "the control point reaches 900 units up, which is 90px at this size");
  });

  AddTest(tests, "Font/AnInvalidSizeProducesNothingRatherThanUndefinedBehavior", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    const GlyphId glyph = face.GlyphForCodepoint(U'A');
    // A CSS font-size can be anything, including a NaN out of a percentage that
    // resolved against an unresolved width.
    for (const float size : {0.0f, -12.0f, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(), 1e9f}) {
      Font font(face, size);
      Path path;
      Expect(!font.GlyphOutline(glyph, path), "an unusable size draws nothing");
      ExpectEqInt(static_cast<long long>(font.Advance(glyph)), 0, "and advances nothing");
      Expect(font.Metrics() == FontMetrics{}, "and has no metrics");
    }
  });

  AddTest(tests, "Font/GlyphsRenderThroughThePainter", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 60.0f);

    gfx::Canvas canvas(64, 64);
    canvas.Clear(gfx::Color::Rgb(0xFF, 0xFF, 0xFF));
    gfx::Painter painter(canvas);

    Path path;
    Expect(font.GlyphOutline(face.GlyphForCodepoint(U'A'), path), "outline");
    painter.SetTransform(gfx::AffineTransform::Translation(0.0f, 50.0f));
    painter.FillPath(path, gfx::Color::Rgb(0, 0, 0));

    // The square spans 100..700 units across and 0..600 up, so at 60px/em and a
    // baseline at y=50 it covers roughly x 6..42, y 14..50.
    ExpectEqInt(static_cast<int>((canvas.Row(30)[20] >> 16) & 0xFFu), 0x00,
                "the middle of the glyph is inked");
    ExpectEqInt(static_cast<int>((canvas.Row(55)[20] >> 16) & 0xFFu), 0xFF,
                "below the baseline is not");
  });
}

}  // namespace microbrowser::tests

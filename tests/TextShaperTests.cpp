#include <cmath>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/Font.h"
#include "gfx/Painter.h"
#include "gfx/TextShaper.h"
#include "support/ReferenceImage.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::FloatPoint;
using gfx::Font;
using gfx::FontFace;
using gfx::FontLibrary;
using gfx::Painter;
using gfx::PositionedGlyph;
using gfx::ShapedRun;
using gfx::TextShaper;

namespace {

FontFace LoadSyntheticFace(const FontLibrary& library) {
  auto face = FontFace::Load(library, BuildSyntheticFont());
  Expect(face.has_value(), "the synthetic font must parse");
  return std::move(*face);
}

}  // namespace

void RegisterTextShaperTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TextShaper/EmptyTextProducesNoGlyphs", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    TextShaper shaper;
    const ShapedRun& run = shaper.Shape(font, "");
    Expect(run.glyphs.empty(), "nothing in, nothing out");
    Expect(run.width == 0.0f, "and no width");
  });

  AddTest(tests, "TextShaper/MapsCharactersToTheGlyphsTheFontDefines", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 40.0f);
    TextShaper shaper;
    const ShapedRun& run = shaper.Shape(font, "ABCD");

    ExpectEqInt(static_cast<long long>(run.glyphs.size()), 4, "four characters, four glyphs");
    for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
      const char32_t codepoint = static_cast<char32_t>(U'A' + i);
      ExpectEqInt(run.glyphs[i].glyph, face.GlyphForCodepoint(codepoint),
                  "each glyph must be the one the cmap names");
    }
  });

  // The check that catches a wrong fixed-point scale, which is the classic
  // HarfBuzz-plus-FreeType integration bug: a factor of 64 makes text either
  // invisible or enormous, and both look like something else went wrong.
  AddTest(tests, "TextShaper/ShapedAdvancesAgreeWithTheFontsOwnAdvances", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 32.0f);
    TextShaper shaper;
    const ShapedRun& run = shaper.Shape(font, "ABCD");

    float expected_width = 0.0f;
    for (const PositionedGlyph& glyph : run.glyphs) {
      const float from_font = font.Advance(glyph.glyph);
      Expect(std::abs(glyph.x_advance - from_font) < 0.05f,
             "a shaped advance must equal the advance the font reports for the same glyph; "
             "if these differ by a factor of 64 the fixed-point scale is wrong");
      expected_width += from_font;
    }
    Expect(std::abs(run.width - expected_width) < 0.05f, "the run width is the sum of advances");
  });

  AddTest(tests, "TextShaper/AdvancesScaleWithTheFontSize", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    TextShaper shaper;

    Font small(face, 16.0f);
    const float small_width = shaper.Shape(small, "ABCD").width;
    Font large(face, 48.0f);
    const float large_width = shaper.Shape(large, "ABCD").width;

    Expect(std::abs(large_width - 3.0f * small_width) < 0.1f,
           "three times the size is three times the width; a stale cached shaping font would "
           "return the previous size here");
  });

  // Clusters are what selection, caret placement, and hit testing are built on.
  // A shaper that discards them produces text that renders and cannot be used.
  AddTest(tests, "TextShaper/ClustersPointBackIntoTheSourceBytes", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 24.0f);
    TextShaper shaper;
    const ShapedRun& run = shaper.Shape(font, "A B");

    ExpectEqInt(static_cast<long long>(run.glyphs.size()), 3, "two letters and a space");
    ExpectEqInt(static_cast<long long>(run.glyphs[0].cluster), 0, "'A' starts at byte 0");
    ExpectEqInt(static_cast<long long>(run.glyphs[1].cluster), 1, "the space at byte 1");
    ExpectEqInt(static_cast<long long>(run.glyphs[2].cluster), 2, "and 'B' at byte 2");
  });

  AddTest(tests, "TextShaper/UnmappedCharactersBecomeNotdefRatherThanVanishing", [] {
    // A missing glyph must occupy space and be visible as a missing glyph.
    // Silently dropping it makes a page look subtly wrong with no clue why.
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 24.0f);
    TextShaper shaper;
    const ShapedRun& run = shaper.Shape(font, "AZA");

    ExpectEqInt(static_cast<long long>(run.glyphs.size()), 3, "three characters, three glyphs");
    ExpectEqInt(run.glyphs[1].glyph, 0, "the unmapped one is .notdef");
  });

  AddTest(tests, "TextShaper/MultiByteCharactersKeepTheirByteOffsets", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 24.0f);
    TextShaper shaper;
    // U+00E9 is two bytes in UTF-8, so the following 'A' is at byte 2, not 1.
    const ShapedRun& run = shaper.Shape(font, "\xC3\xA9" "A");
    ExpectEqInt(static_cast<long long>(run.glyphs.size()), 2, "two characters");
    ExpectEqInt(static_cast<long long>(run.glyphs[0].cluster), 0, "the first starts at byte 0");
    ExpectEqInt(static_cast<long long>(run.glyphs[1].cluster), 2,
                "and the second at byte 2, because clusters index bytes rather than characters");
  });

  AddTest(tests, "TextShaper/MalformedUtf8DoesNotCrashOrHang", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    TextShaper shaper;
    // Text comes from the network. Every one of these is a byte sequence a page
    // can serve, and none may do anything but shape to something harmless.
    for (const std::string& text : {std::string("\xFF\xFE\xFD"), std::string("\xC3"),
                                    std::string("A\x80\x80" "B"), std::string("\xED\xA0\x80"),
                                    std::string(1000, '\xF4')}) {
      const ShapedRun& run = shaper.Shape(font, text);
      for (const PositionedGlyph& glyph : run.glyphs) {
        Expect(glyph.cluster <= text.size(),
               "a cluster must index the text it came from, whatever that text was");
      }
    }
  });

  AddTest(tests, "TextShaper/AnUnusableFontSizeShapesToNothing", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    TextShaper shaper;
    for (const float size : {0.0f, -8.0f, std::numeric_limits<float>::quiet_NaN()}) {
      Font font(face, size);
      Expect(shaper.Shape(font, "ABCD").glyphs.empty(),
             "a size the font cannot be set to must shape to nothing, not to garbage");
    }
  });

  AddTest(tests, "TextShaper/ReusingTheShaperDoesNotLeakStateBetweenRuns", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 24.0f);
    TextShaper shaper;

    shaper.Shape(font, "ABCD");
    const ShapedRun& second = shaper.Shape(font, "A");
    ExpectEqInt(static_cast<long long>(second.glyphs.size()), 1,
                "the buffer must be cleared between runs, not appended to");
    Expect(std::abs(second.width - font.Advance(second.glyphs[0].glyph)) < 0.05f,
           "and the width must be recomputed rather than accumulated");
  });

  // --- Drawing --------------------------------------------------------------

  AddTest(tests, "Painter/GlyphsAdvanceAlongTheBaseline", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 40.0f);
    TextShaper shaper;
    const ShapedRun run = shaper.Shape(font, "AA");

    Canvas canvas(120, 60);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    painter.DrawGlyphs(font, run, FloatPoint{5.0f, 50.0f}, Color::Rgb(0, 0, 0));

    // 'A' is a square spanning 100..700 units of a 1000 em, so at 40px it inks
    // x 4..28 relative to the pen. The second copy is one advance (32px) later.
    const auto inked = [&canvas](int x, int y) {
      return ((canvas.Row(y)[x] >> 16) & 0xFFu) < 0x80;
    };
    Expect(inked(15, 40), "the first glyph is drawn");
    Expect(inked(47, 40), "the second is drawn one advance to the right");
    Expect(!inked(35, 40), "and the gap between them is not inked");
  });

  AddTest(tests, "Painter/GlyphsSitOnTheBaselineRatherThanBelowIt", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 40.0f);
    TextShaper shaper;
    const ShapedRun run = shaper.Shape(font, "A");

    Canvas canvas(60, 60);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    const float baseline = 40.0f;
    painter.DrawGlyphs(font, run, FloatPoint{5.0f, baseline}, Color::Rgb(0, 0, 0));

    const auto inked = [&canvas](int x, int y) {
      return ((canvas.Row(y)[x] >> 16) & 0xFFu) < 0x80;
    };
    Expect(inked(15, 38), "just above the baseline is inked");
    Expect(!inked(15, 45),
           "below it is not; a y-flip error puts the whole line under its own baseline");
  });

  AddTest(tests, "Painter/DrawingRestoresTheTransformItWasGiven", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Font font(face, 20.0f);
    TextShaper shaper;
    const ShapedRun run = shaper.Shape(font, "AB");

    Canvas canvas(64, 64);
    Painter painter(canvas);
    const gfx::AffineTransform original = gfx::AffineTransform::Scaling(2.0f, 2.0f);
    painter.SetTransform(original);
    painter.DrawGlyphs(font, run, FloatPoint{0.0f, 30.0f}, Color::Rgb(0, 0, 0));
    Expect(painter.Transform() == original,
           "a glyph run walks the pen with the transform, and must put it back");
  });

  AddTest(tests, "Painter/Golden/TextRun", [] {
    const FontLibrary library;
    FontFace face = LoadSyntheticFace(library);
    Canvas canvas(220, 100);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    TextShaper shaper;

    Font large(face, 44.0f);
    painter.DrawGlyphs(large, shaper.Shape(large, "ABCD"), FloatPoint{8.0f, 46.0f},
                       Color::Rgb(0x1F, 0x6F, 0xEB));

    Font small(face, 22.0f);
    painter.DrawGlyphs(small, shaper.Shape(small, "AB CD"), FloatPoint{8.0f, 82.0f},
                       Color::Rgb(0, 0, 0));

    const ComparisonResult result = CompareAgainstGolden(canvas, "text/run");
    Expect(result.matches, result.message);
  });
}

}  // namespace microbrowser::tests

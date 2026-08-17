#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "wpt/Reftest.h"

// Fuzzy reftest matching: the tolerance parser, the pixel comparison, and the
// pass rule (plan task F2).
//
// Compiled into the test binary rather than exercised through a suite run, for
// the reason `Handlers.cpp` is: 20,998 reftest files -- 48% of the checkout --
// are decided by these three functions, and running the suite to find out that
// a range was parsed backwards takes a day. The pass rule in particular is a
// **transcription** of wptrunner's `RefTestExecutor.is_pass`, and a transcription
// is exactly the kind of code that reads correct and is not: its two escape
// hatches for an exact match look redundant and are load-bearing.

namespace microbrowser::tests {

namespace {

using wpt::FuzzyAllowance;
using wpt::FuzzyAnnotation;
using wpt::ImageDifference;

gfx::Canvas Filled(std::uint32_t argb, int width = 4, int height = 4) {
  gfx::Canvas canvas{width, height};
  for (int y = 0; y < height; ++y) {
    std::uint32_t* row = canvas.Row(y);
    for (int x = 0; x < width; ++x) {
      row[x] = argb;
    }
  }
  return canvas;
}

}  // namespace

void RegisterWptReftestTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptReftest/ParsesBothSpellingsOfATolerance", [] {
    // The two forms upstream accepts, both present in the pinned checkout:
    // of 678 annotations, 607 name their halves and 65 are positional. (The
    // other six are the `{{ fuzzy }}` templates the next test covers.)
    FuzzyAllowance bare;
    Expect(wpt::ParseFuzzyRanges("0-15;0-200", &bare), "the positional form parses");
    ExpectEqInt(bare.max_difference.high, 15, "the first range is maxDifference");
    ExpectEqInt(bare.total_pixels.high, 200, "the second is totalPixels");

    FuzzyAllowance named;
    Expect(wpt::ParseFuzzyRanges("maxDifference=0-2;totalPixels=0-50", &named),
           "the named form parses");
    ExpectEqInt(named.max_difference.high, 2, "maxDifference by name");
    ExpectEqInt(named.total_pixels.high, 50, "totalPixels by name");

    FuzzyAllowance reversed;
    Expect(wpt::ParseFuzzyRanges("totalPixels=0-50; maxDifference=0-2", &reversed),
           "named halves may be given in either order, and whitespace is skipped");
    ExpectEqInt(reversed.max_difference.high, 2, "maxDifference found second");
    ExpectEqInt(reversed.total_pixels.high, 50, "totalPixels found first");

    FuzzyAllowance single;
    Expect(wpt::ParseFuzzyRanges("3;7", &single), "a bare number is the range n-n");
    ExpectEqInt(single.max_difference.low, 3, "low bound of a single value");
    ExpectEqInt(single.max_difference.high, 3, "high bound of a single value");
  });

  AddTest(tests, "WptReftest/RefusesContentItCannotRead", [] {
    // Six files in the checkout say `content="{{ fuzzy }}"` -- a substitution
    // this server does not perform. A skipped annotation is an exact
    // comparison, which fails visibly; a guessed one is a pass nobody chose.
    FuzzyAllowance allowance;
    Expect(!wpt::ParseFuzzyRanges("{{ fuzzy }}", &allowance), "a template is not a tolerance");
    Expect(!wpt::ParseFuzzyRanges("0-15", &allowance), "one range is not two");
    Expect(!wpt::ParseFuzzyRanges("0-x;0-2", &allowance), "a non-number is not a bound");
    Expect(allowance.IsExact(), "and a refused parse leaves the allowance untouched");
  });

  AddTest(tests, "WptReftest/ReadsTheMetaTagAndItsReferencePrefix", [] {
    const std::vector<FuzzyAnnotation> plain =
        wpt::ParseFuzzy("<meta name=fuzzy content=\"maxDifference=0-1;totalPixels=0-10\">");
    ExpectEqInt(static_cast<long long>(plain.size()), 1, "one annotation");
    Expect(plain[0].reference.empty(), "with no reference, it applies to every reference");
    ExpectEqInt(plain[0].allowance.total_pixels.high, 10, "and its ranges are read");

    // `transform3d-matrix3d-001` is the one file in the checkout that scopes a
    // tolerance to a named reference.
    const std::vector<FuzzyAnnotation> scoped =
        wpt::ParseFuzzy("<meta name=fuzzy content=\"foo-ref.html:0-100;0-980\">");
    ExpectEqInt(static_cast<long long>(scoped.size()), 1, "the prefixed form parses");
    ExpectEqString(scoped[0].reference, "foo-ref.html", "and names its reference");
    ExpectEqInt(scoped[0].allowance.total_pixels.high, 980, "with the ranges after the colon");

    const std::vector<FuzzyAnnotation> pair =
        wpt::ParseFuzzy("<meta name=fuzzy content=\"a.html==b.html:0-1;0-2\">");
    ExpectEqString(pair[0].reference, "b.html",
                   "a `test==ref` prefix names the reference half of the pair");

    Expect(wpt::ParseFuzzy("<meta name=viewport content=\"0-1;0-2\">").empty(),
           "a meta that is not name=fuzzy is not a tolerance");
    Expect(wpt::ParseFuzzy("<title>fuzzy</title>").empty(),
           "and neither is the word appearing in the document");
  });

  AddTest(tests, "WptReftest/SerializesBackIntoWhatItParses", [] {
    // The manifest cache stores the tolerance as a string and reads it back
    // with the same parser, which is what stops the cache and the meta tag from
    // drifting apart.
    FuzzyAllowance allowance;
    Expect(wpt::ParseFuzzyRanges("1-15;2-200", &allowance), "parses");
    const std::string text = wpt::SerializeFuzzy(allowance);
    ExpectEqString(text, "1-15;2-200", "round-trips through the cache format");
    FuzzyAllowance again;
    Expect(wpt::ParseFuzzyRanges(text, &again), "and back");
    ExpectEqInt(again.total_pixels.low, 2, "with both bounds intact");
    Expect(wpt::SerializeFuzzy(FuzzyAllowance{}).empty(),
           "an exact comparison serializes to nothing, so the cache line stays short");
  });

  AddTest(tests, "WptReftest/CountsPixelsAndTheWorstChannel", [] {
    gfx::Canvas actual = Filled(0xFF808080u);
    const gfx::Canvas expected = Filled(0xFF808080u);
    ImageDifference difference = wpt::CompareCanvases(actual, expected);
    ExpectEqInt(static_cast<long long>(difference.pixels_different), 0, "identical is zero");

    actual.Row(1)[2] = 0xFF8380FFu;  // red +3, blue +127
    difference = wpt::CompareCanvases(actual, expected);
    ExpectEqInt(static_cast<long long>(difference.pixels_different), 1, "one pixel differs");
    ExpectEqInt(difference.max_per_channel, 127, "and the worst channel is the largest of the three");

    // Alpha is not a visible difference: both canvases were cleared to opaque
    // white before the display list ran, and a pixel that differs only there is
    // the same picture.
    actual.Row(1)[2] = 0xFF808080u;
    actual.Row(0)[0] = 0x00808080u;
    difference = wpt::CompareCanvases(actual, expected);
    ExpectEqInt(static_cast<long long>(difference.pixels_different), 0,
                "an alpha-only difference is not one");

    const gfx::Canvas taller = Filled(0xFF808080u, 4, 8);
    difference = wpt::CompareCanvases(taller, expected);
    ExpectEqInt(difference.max_per_channel, 255,
                "a reference that laid out to another size is maximally different, not compared "
                "over the overlap");
  });

  AddTest(tests, "WptReftest/AppliesTheToleranceWptRunnerApplies", [] {
    FuzzyAllowance allowance;
    Expect(wpt::ParseFuzzyRanges("0-1;0-10", &allowance), "parses");

    Expect(wpt::FuzzyAllows(ImageDifference{1, 10}, allowance), "at both bounds, it passes");
    Expect(!wpt::FuzzyAllows(ImageDifference{2, 10}, allowance),
           "one level too much on a channel fails");
    Expect(!wpt::FuzzyAllows(ImageDifference{1, 11}, allowance), "one pixel too many fails");
    Expect(wpt::FuzzyAllows(ImageDifference{0, 0}, allowance), "and no difference at all passes");

    // The escape hatches. A tolerance is what *one* engine needed; another
    // engine getting the pair exactly right must not be a failure, so a zero
    // lower bound admits a perfect match even though the range would not.
    FuzzyAllowance floor;
    Expect(wpt::ParseFuzzyRanges("1-5;1-20", &floor), "a tolerance with a non-zero floor");
    Expect(!wpt::FuzzyAllows(ImageDifference{0, 0}, floor),
           "which does not admit a perfect match, because neither floor is zero");
    FuzzyAllowance half;
    Expect(wpt::ParseFuzzyRanges("1-5;0-20", &half), "with one floor at zero");
    Expect(wpt::FuzzyAllows(ImageDifference{0, 0}, half), "it does");

    const FuzzyAllowance exact;
    Expect(wpt::FuzzyAllows(ImageDifference{0, 0}, exact), "no annotation is an exact comparison");
    Expect(!wpt::FuzzyAllows(ImageDifference{1, 1}, exact), "which one pixel fails");
  });

  AddTest(tests, "WptReftest/DrawsWhereTheTwoDisagree", [] {
    gfx::Canvas actual = Filled(0xFFFFFFFFu);
    const gfx::Canvas expected = Filled(0xFFFFFFFFu);
    actual.Row(2)[3] = 0xFFFFFFFEu;  // one level, on one channel
    actual.Row(0)[0] = 0xFF000000u;  // 255 levels

    const gfx::Canvas diff = wpt::DifferenceImage(actual, expected);
    ExpectEqInt(diff.Width(), 4, "the diff is the size of the pair");
    // Yellow at one level, red at 255. The magnitude has to be *in the image*,
    // because that is the whole difference between the two failures a 25-file
    // sample found on 2026-08-17: 304 pixels on a line-height test, which is a
    // tolerance, and 20,237 on a run-in test, which is a missing feature.
    ExpectEqInt(diff.Row(2)[3] & 0x00FFFFFFu, 0x00FFFE00, "a one-level difference is yellow");
    ExpectEqInt(diff.Row(0)[0] & 0x00FFFFFFu, 0x00FF0000, "a 255-level difference is red");
    Expect((diff.Row(1)[1] & 0x00FFFFFFu) == 0x00FFFFFF,
           "and an agreeing pixel keeps the reference, washed out");
  });

  AddTest(tests, "WptReftest/KnowsWhenThereWasNothingToCompare", [] {
    // Two blank pages agree exactly, so a reference that failed to load passes
    // against any test at all. That is the one way the reftest number task F9
    // brought into the measurement can be inflated by the browser getting
    // *worse*, and the run counts it beside the passes rather than deducting it
    // -- wptrunner compares screenshots and does not ask what is on them, and a
    // rule of our own would make the number incomparable with Firefox's.
    Expect(wpt::IsBlank(Filled(0xFFFFFFFFu)), "the colour both canvases are cleared to");
    Expect(wpt::IsBlank(Filled(0x00FFFFFFu)),
           "and alpha is not part of it: the comparison does not look at alpha either");
    gfx::Canvas one_pixel = Filled(0xFFFFFFFFu);
    one_pixel.Row(3)[3] = 0xFFFFFFFEu;
    Expect(!wpt::IsBlank(one_pixel), "one level on one channel in one corner is something drawn");
    Expect(wpt::IsBlank(gfx::Canvas{}), "a page that produced no surface produced no picture");
  });

  AddTest(tests, "WptReftest/WritesAPpmThatNamesItsSize", [] {
    TemporaryDirectory directory;
    const std::string path = (directory.Path() / "diff.ppm").string();
    Expect(wpt::WritePpm(Filled(0xFF102030u, 3, 2), path), "writes");
    const std::string content = ReadFile(path);
    Expect(content.rfind("P6\n3 2\n255\n", 0) == 0, "P6 with the canvas size");
    ExpectEqInt(static_cast<long long>(content.size()), 11 + 3 * 2 * 3,
                "header plus one RGB triple per pixel, alpha dropped");
    ExpectEqInt(static_cast<unsigned char>(content[11]), 0x10, "red first");
    ExpectEqInt(static_cast<unsigned char>(content[13]), 0x30, "blue third");
  });

  AddTest(tests, "WptReftest/FlattensATestPathIntoOneFilename", [] {
    ExpectEqString(wpt::ArtifactStem("css/CSS2/linebox/line-height-095.xht"),
                   "css_CSS2_linebox_line-height-095.xht", "slashes become underscores");
    ExpectEqString(wpt::ArtifactStem("x/y.html?a=b&c"), "x_y.html_a_b_c",
                   "and so does everything a shell would have to quote");
  });
}

}  // namespace microbrowser::tests

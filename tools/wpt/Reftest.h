#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/Canvas.h"

namespace microbrowser::wpt {

// Fuzzy reftest matching, and the three images a failure leaves behind.
//
// A reftest renders two pages and compares the pixels. Comparing them *exactly*
// is what this runner did until plan task F2, and it does not work: the test and
// the reference are laid out differently on purpose -- that is what makes the
// comparison worth anything -- so the same glyph lands on a different subpixel
// phase in each and the analytic-AA rasterizer produces two edges that differ by
// one level. A suite whose failures are one-level antialiasing noise is a suite
// nobody reads, and 20,998 files -- 48% of the checkout -- were in that state.
//
// Upstream's answer is `<meta name=fuzzy>`, an author's statement of how much
// noise this particular pair is allowed to produce. 678 files in the pinned
// checkout carry one. Every rule below is transcribed from wptrunner
// (`tools/wptrunner/wptrunner/executors/base.py`, `RefTestExecutor.is_pass`),
// because a tolerance we invented would make our numbers incomparable with
// Firefox's -- which is the entire reason docs/wpt-firefox-gap.md exists.

// One inclusive range, written `low-high` in a fuzzy annotation. A bare number
// means `n-n`.
struct FuzzyRange {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
};

// What a `<meta name=fuzzy>` permits. The default -- no annotation -- is
// `0-0;0-0`, which is an exact comparison.
struct FuzzyAllowance {
  // Per colour channel, over R, G and B.
  FuzzyRange max_difference;
  // How many pixels may differ at all.
  FuzzyRange total_pixels;

  bool IsExact() const {
    return max_difference.low == 0 && max_difference.high == 0 && total_pixels.low == 0 &&
           total_pixels.high == 0;
  }
};

// A parsed annotation, with the reference it was scoped to.
//
// `content` may carry a `ref:` or `test==ref:` prefix naming which reference the
// tolerance applies to, for a test with several. Exactly one file in the pinned
// checkout uses it (`transform3d-matrix3d-001`), and it is parsed anyway because
// an unrecognised prefix would otherwise be read as a range and silently drop
// the tolerance.
struct FuzzyAnnotation {
  // The href as written, un-resolved. Empty means "every reference".
  std::string reference;
  FuzzyAllowance allowance;
};

// Every `<meta name=fuzzy content=...>` in `head`, in document order.
//
// Malformed content is skipped rather than guessed at: a tolerance nobody wrote
// is an exact comparison, which fails loudly, and a tolerance we invented from a
// half-parsed string is a pass nobody chose.
std::vector<FuzzyAnnotation> ParseFuzzy(std::string_view head);

// `"0-15;0-200"` or `"maxDifference=0-15;totalPixels=0-200"`, with no prefix.
// False when the string is not one of those, leaving `out` untouched.
bool ParseFuzzyRanges(std::string_view content, FuzzyAllowance* out);

// The serialization the manifest cache stores: `low-high;low-high`, or the
// empty string for an exact comparison. `ParseFuzzyRanges` reads it back, which
// is the point -- the cache and the meta tag cannot drift apart if they are the
// same two functions.
std::string SerializeFuzzy(const FuzzyAllowance& allowance);

// What separates two renderings, in the two quantities `<meta name=fuzzy>` is
// written in.
struct ImageDifference {
  // The largest single-channel difference anywhere, 0-255.
  std::uint32_t max_per_channel = 0;
  // Pixels differing in any of R, G, B.
  std::uint64_t pixels_different = 0;
};

// Compares R, G and B. Alpha is not compared because there is none to compare:
// both canvases were cleared to opaque white before the display list ran, which
// is what the window does.
//
// Canvases of different sizes are reported as maximally different rather than
// compared over their intersection -- a reference that laid out to a different
// size is a real failure, and comparing the overlap would hide it.
ImageDifference CompareCanvases(const gfx::Canvas& actual, const gfx::Canvas& expected);

// wptrunner's rule, transcribed. Note the two escape hatches in the middle: a
// pair that matched *exactly* passes an annotation whose lower bound is zero,
// even though a range like `1-5` would otherwise require at least one differing
// pixel. Upstream has them because a fuzzy annotation records what one engine
// needed, and another engine getting it exactly right must not be a failure.
bool FuzzyAllows(const ImageDifference& difference, const FuzzyAllowance& allowance);

// A human-readable image of where the two disagree: the reference washed out to
// a quarter of its contrast, with every differing pixel painted in a ramp from
// yellow (one level) to red (255 levels).
//
// The magnitude has to be *in* the image rather than only in the report,
// because that is the whole difference between the two failures a 25-file
// sample found on 2026-08-17 -- 304 pixels on a line-height test, which is a
// tolerance, and 20,237 on a `run-in` test, which is a missing feature. Told
// apart by eye in one second and by a pixel count not at all.
gfx::Canvas DifferenceImage(const gfx::Canvas& actual, const gfx::Canvas& expected);

// P6, the same eight lines as tools/snapshot and tests/support/ReferenceImage.
// Neither of those is reachable from here: one is a `main`, the other links the
// test harness.
bool WritePpm(const gfx::Canvas& canvas, const std::string& path);

// A test's url path as one filename: `css/foo/bar.html?x` becomes
// `css_foo_bar.html_x`. Only for artifacts, and only so the three files a
// failure leaves sort next to each other.
std::string ArtifactStem(std::string_view url_path);

}  // namespace microbrowser::wpt

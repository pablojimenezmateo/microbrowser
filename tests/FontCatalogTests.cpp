#include <limits>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/FontCatalog.h"
#include "gfx/TextRenderer.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

using gfx::Font;
using gfx::FontCatalog;
using gfx::FontLibrary;
using gfx::FontRequest;
using gfx::TextRenderer;

namespace {

// Two fonts whose advances differ, so a test can tell which one answered.
// Halving the em doubles every advance in pixels at a fixed size.
std::vector<std::byte> WideFont() {
  return BuildSyntheticFont(SyntheticFontSpec{1000, 800, 200, 0});
}

std::vector<std::byte> NarrowFont() {
  return BuildSyntheticFont(SyntheticFontSpec{2000, 1600, 400, 0});
}

}  // namespace

void RegisterFontCatalogTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FontCatalog/PicksTheNearestWeightWithinAFamily", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Test", 400, false, WideFont()), "the regular registered");
    Expect(catalog.Register("Test", 700, false, WideFont()), "and the bold");

    // 500 is nearer 400 than 700, which is what a family shipping only two
    // weights should do with `font-weight: 500`.
    const FontRequest five_hundred{{"Test"}, 16.0f, 500, false};
    Expect(FontCatalog::MatchDistance(400, false, five_hundred, 0) <
               FontCatalog::MatchDistance(700, false, five_hundred, 0),
           "500 is nearer 400 than 700");
    const FontRequest six_hundred{{"Test"}, 16.0f, 600, false};
    Expect(FontCatalog::MatchDistance(700, false, six_hundred, 0) <
               FontCatalog::MatchDistance(400, false, six_hundred, 0),
           "and 600 is nearer 700");
  });

  AddTest(tests, "FontCatalog/SlantOutranksWeight", [] {
    // An upright face at the requested weight is a worse answer than an italic
    // one at the wrong weight, because the wrong slant is the visible error.
    const FontRequest italic{{"Test"}, 16.0f, 400, true};
    Expect(FontCatalog::MatchDistance(900, true, italic, 0) <
               FontCatalog::MatchDistance(400, false, italic, 0),
           "any italic beats any upright for an italic request");
  });

  AddTest(tests, "FontCatalog/FamilyPositionOutranksEverything", [] {
    const FontRequest request{{"Test", "Other"}, 16.0f, 400, false};
    Expect(FontCatalog::MatchDistance(900, true, request, 0) <
               FontCatalog::MatchDistance(400, false, request, 1),
           "the page's second choice is never a better answer than its first, however much "
           "better its weight and slant match");
    Expect(FontCatalog::MatchDistance(400, false, request, 1) <
               FontCatalog::MatchDistance(400, false, request, -1),
           "and a family the page did not name at all is worse than any it did");
  });

  AddTest(tests, "FontCatalog/CandidatesAreTheListThenTheDefault", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    catalog.SetDefaultFamily("Fallback");
    catalog.SetGenericFamily("sans-serif", "Fallback Sans");

    const std::vector<std::string> candidates =
        catalog.FamilyCandidates(FontRequest{{"Verdana", "Geneva", "sans-serif"}, 16.0f, 400,
                                             false});
    ExpectEqInt(static_cast<long long>(candidates.size()), 4, "three named, then the default");
    Expect(candidates[0] == "verdana" && candidates[1] == "geneva", "in the order written");
    Expect(candidates[2] == "fallback sans", "with the generic resolved to a real family");
    Expect(candidates.back() == "fallback",
           "and the default last, so a stack of families this machine lacks still renders");
  });

  AddTest(tests, "FontCatalog/TheFirstInstalledFamilyOnTheListWins", [] {
    // The bug this guards: `font-family: Verdana, Geneva, sans-serif` was
    // matched as one literal family name called "verdana, geneva, sans-serif",
    // which matches nothing, so no face loaded and the page rendered no text
    // at all. Every real stylesheet writes a list.
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Wide", 400, false, WideFont()), "wide registered");
    Expect(catalog.Register("Narrow", 400, false, NarrowFont()), "narrow registered");
    catalog.SetDefaultFamily("Wide");

    const Font* second_choice =
        catalog.FontFor(FontRequest{{"Nonexistent", "Narrow", "Wide"}, 16.0f, 400, false});
    Expect(second_choice != nullptr && second_choice == catalog.FontFor(FontRequest{
                                           {"Narrow"}, 16.0f, 400, false}),
           "the first name that exists wins, and a missing first choice is skipped rather "
           "than failing the whole list");
  });

  AddTest(tests, "FontCatalog/AnEarlierFamilyBeatsANearerWeight", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Wide", 400, false, WideFont()), "wide regular registered");
    Expect(catalog.Register("Narrow", 700, false, NarrowFont()), "narrow bold registered");
    catalog.SetDefaultFamily("Narrow");

    const Font* bold = catalog.FontFor(FontRequest{{"Wide", "Narrow"}, 16.0f, 700, false});
    Expect(bold != nullptr && bold == catalog.FontFor(FontRequest{{"Wide"}, 16.0f, 400, false}),
           "a bold face from the second family is still the wrong font; the right family in "
           "the wrong weight is the answer");
  });

  AddTest(tests, "FontCatalog/MatchingIsCaseAndWhitespaceInsensitive", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("DejaVu Test", 400, false, WideFont()), "registered");
    Expect(catalog.FontFor(FontRequest{{"  dejavu test "}, 16.0f, 400, false}) != nullptr,
           "CSS family names compare case-insensitively, ignoring surrounding space");
  });

  AddTest(tests, "FontCatalog/AnUnmatchedFamilyFallsBackToTheDefault", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Test", 400, false, WideFont()), "registered");
    catalog.SetDefaultFamily("Test");
    Expect(catalog.FontFor(FontRequest{{"Nonexistent"}, 16.0f, 400, false}) != nullptr,
           "a page naming a family this machine does not have must still render text");
  });

  AddTest(tests, "FontCatalog/GenericFamiliesResolveToRealOnes", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Wide", 400, false, WideFont()), "wide registered");
    Expect(catalog.Register("Narrow", 400, false, NarrowFont()), "narrow registered");
    catalog.SetDefaultFamily("Wide");
    catalog.SetGenericFamily("monospace", "Narrow");

    const Font* mono = catalog.FontFor(FontRequest{{"monospace"}, 16.0f, 400, false});
    const Font* narrow = catalog.FontFor(FontRequest{{"Narrow"}, 16.0f, 400, false});
    const Font* wide = catalog.FontFor(FontRequest{{"Wide"}, 16.0f, 400, false});
    Expect(mono != nullptr && mono == narrow,
           "`monospace` is not a font; resolving it to the default sans renders code samples "
           "in a proportional face");
    Expect(wide != mono, "and the default is still a different face");
  });

  AddTest(tests, "FontCatalog/RejectsBytesThatAreNotAFont", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(!catalog.Register("Bad", 400, false, BuildCorruptFont()),
           "a font file is attacker controlled once @font-face exists, so this is a routine "
           "false rather than a crash");
    ExpectEqInt(static_cast<long long>(catalog.FaceCount()), 0, "and nothing was registered");
  });

  AddTest(tests, "FontCatalog/RejectsANonsensicalSize", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Test", 400, false, WideFont()), "registered");
    for (const float size : {0.0f, -12.0f, std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN()}) {
      Expect(catalog.FontFor(FontRequest{{"Test"}, size, 400, false}) == nullptr,
             "a size can arrive from a stylesheet or an IPC frame and be anything");
    }
  });

  AddTest(tests, "FontCatalog/AFaceDescribesItself", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register(WideFont()),
           "a font database registers by what the face claims, because a filename says 'Bold' "
           "only by convention");
    ExpectEqInt(static_cast<long long>(catalog.FaceCount()), 1, "one face");
  });

  // --- The shaped-run cache -------------------------------------------------

  AddTest(tests, "TextRenderer/ShapesEachRunOnce", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Test", 400, false, WideFont()), "registered");
    TextRenderer text(catalog);

    const FontRequest request{{"Test"}, 16.0f, 400, false};
    const float first = text.MeasureRun("AAAA", request);
    ExpectEqInt(static_cast<long long>(text.CachedRuns()), 1, "one run cached");
    const float second = text.MeasureRun("AAAA", request);
    Expect(first == second && first > 0.0f, "and the second measurement agrees");
    ExpectEqInt(static_cast<long long>(text.CachedRuns()), 1,
                "shaping is the most expensive step in the text stack; measuring twice must "
                "not shape twice");
  });

  AddTest(tests, "TextRenderer/RegisteringAFaceDoesNotCorruptCachedRuns", [] {
    // Found by rendering a page: `monospace` measured as the default sans.
    // The cache was keyed on a Font*, and registering a face destroyed every
    // Font the catalog had handed out. The allocator then handed the same
    // address back for a different face, and the stale entry hit.
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Wide", 400, false, WideFont()), "wide registered");
    TextRenderer text(catalog);

    const FontRequest wide{{"Wide"}, 16.0f, 400, false};
    const float before = text.MeasureRun("AAAA", wide);
    Expect(before > 0.0f, "the wide font measures something");

    Expect(catalog.Register("Narrow", 400, false, NarrowFont()), "narrow registered");
    const float after = text.MeasureRun("AAAA", wide);
    ExpectEqInt(static_cast<long long>(after * 100.0f), static_cast<long long>(before * 100.0f),
                "registering another face must not change what the first one measures");

    const float narrow = text.MeasureRun("AAAA", FontRequest{{"Narrow"}, 16.0f, 400, false});
    Expect(narrow != before,
           "and the new face must measure as itself rather than hitting the other one's "
           "cached run");
  });

  AddTest(tests, "TextRenderer/EvictsLeastRecentlyUsed", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Test", 400, false, WideFont()), "registered");
    TextRenderer text(catalog);
    text.SetCapacity(2);

    const FontRequest request{{"Test"}, 16.0f, 400, false};
    text.MeasureRun("A", request);
    text.MeasureRun("AA", request);
    text.MeasureRun("A", request);    // makes "AA" the least recently used
    text.MeasureRun("AAA", request);  // evicts it
    ExpectEqInt(static_cast<long long>(text.CachedRuns()), 2,
                "a page that animates text must not grow the cache without bound");
  });

  AddTest(tests, "TextRenderer/MeasuresNothingWithoutAFont", [] {
    FontLibrary library;
    FontCatalog catalog(library);  // nothing registered
    TextRenderer text(catalog);
    Expect(text.MeasureRun("A", FontRequest{{"Test"}, 16.0f, 400, false}) == 0.0f,
           "no font is a legitimate outcome, not a crash");
    Expect(text.MetricsFor(FontRequest{{"Test"}, 16.0f, 400, false}).ascent == 0.0f,
           "and its metrics are zero rather than garbage");
  });

  AddTest(tests, "TextRenderer/AZeroCapacityStillShapes", [] {
    FontLibrary library;
    FontCatalog catalog(library);
    Expect(catalog.Register("Test", 400, false, WideFont()), "registered");
    TextRenderer text(catalog);
    text.SetCapacity(0);
    Expect(text.MeasureRun("AAAA", FontRequest{{"Test"}, 16.0f, 400, false}) > 0.0f,
           "turning the cache off must disable caching, not measurement");
    ExpectEqInt(static_cast<long long>(text.CachedRuns()), 0, "and cache nothing");
  });
}

}  // namespace microbrowser::tests

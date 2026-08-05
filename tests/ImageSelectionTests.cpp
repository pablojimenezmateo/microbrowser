#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "css/MediaQuery.h"
#include "dom/Node.h"
#include "engine/ImageSelection.h"
#include "html/TreeBuilder.h"

namespace microbrowser::tests {

using css::MediaContext;
using engine::ImageCandidate;
using engine::ImageTypeIsSupported;
using engine::ParseSizes;
using engine::ParseSrcset;
using engine::SelectImageSource;

namespace {

// A viewport wide enough that `100vw` is a number nothing else in a test could
// be mistaken for.
MediaContext Viewport(float width, float density) {
  MediaContext context;
  context.viewport_width = width;
  context.viewport_height = 800.0f;
  context.device_pixel_ratio = density;
  return context;
}

// The URL the first <img> in `html` selects.
std::string Selected(std::string_view html, const MediaContext& context) {
  const std::unique_ptr<dom::Document> document = html::ParseDocument(html);
  const dom::Element* image = document->FirstElementByTagName("img");
  Expect(image != nullptr, "the fixture has no <img>");
  return SelectImageSource(*image, context);
}

}  // namespace

void RegisterImageSelectionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Srcset/SplitsOnWhitespaceRatherThanCommas", [] {
    // The failure this catches is the obvious implementation: split on commas,
    // then split each piece on whitespace. reddit's preview URLs carry commas
    // inside a query string, and that implementation turns one of them into two
    // candidates and fetches neither.
    const std::vector<ImageCandidate> candidates =
        ParseSrcset("https://i.example/a.png?crop=1,2,3&x=1 2x, b.png 1x");
    ExpectEqInt(static_cast<long long>(candidates.size()), 2, "candidate count");
    ExpectEqString(candidates[0].url, "https://i.example/a.png?crop=1,2,3&x=1", "first url");
    Expect(candidates[0].density == 2.0f, "first density");
    ExpectEqString(candidates[1].url, "b.png", "second url");
  });

  AddTest(tests, "Srcset/AUrlEndingInACommaHasNoDescriptor", [] {
    // Whitespace is what separates a URL from its descriptor, so a comma with
    // no space after it is *part of the URL* -- `a.png,b.png 2x` is one
    // candidate at a URL with a comma in it, which is what the spec says and
    // what the previous test relies on. A trailing comma is different: it ends
    // the candidate, and the descriptor is empty.
    ExpectEqInt(static_cast<long long>(ParseSrcset("a.png,b.png 2x").size()), 1,
                "a comma inside a URL does not separate");
    const std::vector<ImageCandidate> candidates = ParseSrcset("a.png,, b.png 2x");
    ExpectEqInt(static_cast<long long>(candidates.size()), 2, "candidate count");
    ExpectEqString(candidates[0].url, "a.png", "first url");
    Expect(candidates[0].density == 1.0f, "no descriptor is one x");
    ExpectEqString(candidates[1].url, "b.png", "second url");
    Expect(candidates[1].density == 2.0f, "second density");
  });

  AddTest(tests, "Srcset/DropsACandidateWithAnUnparsableDescriptor", [] {
    const std::vector<ImageCandidate> candidates = ParseSrcset("a.png 2q, b.png 3x, c.png -1x");
    ExpectEqInt(static_cast<long long>(candidates.size()), 1, "only the valid candidate survives");
    ExpectEqString(candidates[0].url, "b.png", "surviving url");
  });

  AddTest(tests, "Srcset/ReadsWidthDescriptors", [] {
    const std::vector<ImageCandidate> candidates = ParseSrcset("a.png 640w, b.png 1280w");
    ExpectEqInt(static_cast<long long>(candidates.size()), 2, "candidate count");
    Expect(candidates[0].has_width && candidates[0].width == 640.0f, "first width");
    Expect(candidates[1].has_width && candidates[1].width == 1280.0f, "second width");
  });

  AddTest(tests, "Srcset/IsBounded", [] {
    // An attribute is attacker-controlled and every candidate in it is a URL
    // this browser may be asked to fetch.
    std::string srcset;
    for (int i = 0; i < 500; ++i) {
      srcset += "a" + std::to_string(i) + ".png 1x, ";
    }
    Expect(ParseSrcset(srcset).size() <= 64, "candidate count is bounded");
  });

  AddTest(tests, "Sizes/DefaultsToTheViewportWidth", [] {
    Expect(ParseSizes("", Viewport(900.0f, 1.0f)) == 900.0f, "no sizes attribute is 100vw");
  });

  AddTest(tests, "Sizes/TakesTheFirstEntryWhoseConditionMatches", [] {
    const MediaContext narrow = Viewport(500.0f, 1.0f);
    const MediaContext wide = Viewport(1000.0f, 1.0f);
    constexpr std::string_view kSizes = "(min-width: 600px) 300px, 100px";
    Expect(ParseSizes(kSizes, narrow) == 100.0f, "narrow takes the unconditional entry");
    Expect(ParseSizes(kSizes, wide) == 300.0f, "wide takes the conditional one");
  });

  AddTest(tests, "Sizes/ResolvesViewportUnits", [] {
    Expect(ParseSizes("50vw", Viewport(1000.0f, 1.0f)) == 500.0f, "50vw of a 1000px viewport");
  });

  AddTest(tests, "Sizes/AnEntryItCannotResolveFallsThroughRatherThanGuessing", [] {
    // `calc()` is not resolvable here. The next entry answers, and if there is
    // none the 100vw default does -- a guessed number would scale the density
    // of every candidate.
    Expect(ParseSizes("calc(100vw - 20px), 200px", Viewport(1000.0f, 1.0f)) == 200.0f,
           "the unresolvable entry is skipped");
    Expect(ParseSizes("calc(100vw - 20px)", Viewport(1000.0f, 1.0f)) == 1000.0f,
           "with nothing after it, the default answers");
  });

  AddTest(tests, "SelectImageSource/PicksTheLowestDensityThatReachesTheDevice", [] {
    constexpr std::string_view kHtml = R"HTML(<img srcset="a.png 1x, b.png 2x, c.png 3x">)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "a.png", "1x device");
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.5f)), "b.png", "1.5x device rounds up");
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 2.0f)), "b.png", "2x device");
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 4.0f)), "c.png", "past the top, the largest");
  });

  AddTest(tests, "SelectImageSource/SrcIsTheOneTimesCandidateWhenSrcsetNamesNone", [] {
    constexpr std::string_view kHtml = R"HTML(<img src="a.png" srcset="b.png 2x">)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "a.png", "src answers at 1x");
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 2.0f)), "b.png", "srcset answers at 2x");
  });

  AddTest(tests, "SelectImageSource/SrcIsIgnoredBesideWidthDescriptors", [] {
    // An author who writes both means `src` for a browser with no srcset. This
    // one has a srcset, and adding `src` as a 1x candidate beside a set of `w`
    // descriptors would usually win and always be the wrong resolution.
    constexpr std::string_view kHtml = R"HTML(<img src="fallback.png" srcset="a.png 400w, b.png 800w">)HTML";
    ExpectEqString(Selected(kHtml, Viewport(400.0f, 1.0f)), "a.png", "400px source size at 1x");
    ExpectEqString(Selected(kHtml, Viewport(400.0f, 2.0f)), "b.png", "400px source size at 2x");
  });

  AddTest(tests, "SelectImageSource/AWidthDescriptorBecomesADensityThroughSizes", [] {
    // 800w against a 400px source size is a 2x image; against an 800px one it
    // is a 1x image. Same markup, same device, different answer -- which is the
    // whole reason `sizes` exists.
    constexpr std::string_view kHtml =
        R"HTML(<img sizes="(min-width: 900px) 800px, 400px" srcset="a.png 400w, b.png 800w">)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "b.png", "800px source size");
    ExpectEqString(Selected(kHtml, Viewport(500.0f, 1.0f)), "a.png", "400px source size");
  });

  AddTest(tests, "SelectImageSource/NoSrcAndNoSrcsetSelectsNothing", [] {
    ExpectEqString(Selected("<img alt=nothing>", Viewport(1000.0f, 1.0f)), "", "nothing selected");
  });

  AddTest(tests, "Picture/ADeclinedTypeFallsThroughToTheNextSource", [] {
    // The honest-absence rule at the markup layer: claiming WebP would pick a
    // source that renders as an empty box, where declining it picks the one
    // the author wrote for exactly this browser.
    constexpr std::string_view kHtml = R"HTML(<picture>
        <source type="image/webp" srcset="a.webp">
        <source type="image/jpeg" srcset="b.jpg">
        <img src="c.png"></picture>)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "b.jpg", "the jpeg source wins");
  });

  AddTest(tests, "Picture/AMediaConditionChoosesBetweenSources", [] {
    constexpr std::string_view kHtml = R"HTML(<picture>
        <source media="(min-width: 900px)" srcset="wide.png">
        <source media="(max-width: 899px)" srcset="narrow.png">
        <img src="fallback.png"></picture>)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "wide.png", "wide viewport");
    ExpectEqString(Selected(kHtml, Viewport(500.0f, 1.0f)), "narrow.png", "narrow viewport");
  });

  AddTest(tests, "Picture/FallsBackToTheImgWhenNoSourceMatches", [] {
    constexpr std::string_view kHtml = R"HTML(<picture>
        <source media="(min-width: 900px)" type="image/webp" srcset="a.webp">
        <img src="fallback.png"></picture>)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "fallback.png", "the img answers");
  });

  AddTest(tests, "Picture/OnlySourcesBeforeTheImgAreConsidered", [] {
    // The spec walks the picture's children up to the img and stops. A source
    // after it is markup the author put in the wrong place, and honouring it
    // would make the fallback unreachable.
    constexpr std::string_view kHtml = R"HTML(<picture>
        <img src="fallback.png"><source srcset="after.png"></picture>)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "fallback.png", "the img answers");
  });

  AddTest(tests, "Picture/ASourceWinsOverTheImgsOwnSrcset", [] {
    constexpr std::string_view kHtml = R"HTML(<picture>
        <source srcset="source.png">
        <img src="fallback.png" srcset="own.png 1x"></picture>)HTML";
    ExpectEqString(Selected(kHtml, Viewport(1000.0f, 1.0f)), "source.png", "the source answers");
  });

  AddTest(tests, "ImageTypeIsSupported/NamesExactlyWhatGfxDecodes", [] {
    // Deliberately an exhaustive assertion rather than a spot check. This list
    // and the magic-number sniffing in Engine::DecodePendingImages are the same
    // fact stated twice, so when a decoder lands this test fails and points at
    // the other copy. ADR 0023 §5: GIF is next, then WebP.
    Expect(ImageTypeIsSupported("image/png"), "png");
    Expect(ImageTypeIsSupported("image/jpeg"), "jpeg");
    Expect(ImageTypeIsSupported("IMAGE/SVG+XML"), "svg, case-insensitively");
    Expect(!ImageTypeIsSupported("image/gif"), "gif is not decoded yet");
    Expect(!ImageTypeIsSupported("image/webp"), "webp is not decoded");
    Expect(!ImageTypeIsSupported("image/avif"), "avif is not decoded");
    Expect(!ImageTypeIsSupported(""), "an empty type names nothing");
  });
}

}  // namespace microbrowser::tests

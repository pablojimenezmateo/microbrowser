#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "css/MediaQuery.h"
#include "dom/Node.h"
#include "engine/ImageSelection.h"
#include "html/TreeBuilder.h"

// `srcset`, `sizes`, `<source media>`, and the media query grammar under all
// three, fed arbitrary bytes.
//
// Every one of these is attribute text from a page, and the URL that comes out
// the far end is one this browser will fetch. There is no failure mode to
// check for beyond termination and a bound: the property is that no input
// makes the parse loop, recurse without limit, or produce more candidates than
// the input could have named.
//
// The same bytes drive three entry points rather than one, because the three
// share a grammar at different depths: a `sizes` entry contains a media
// condition, a media condition contains a length, and `srcset` contains
// neither. Feeding the corpus to all of them is what lets a coverage-guided
// input found for one reach the others.
namespace {

using microbrowser::css::MediaContext;

MediaContext Viewport() {
  MediaContext context;
  context.viewport_width = 1000.0f;
  context.viewport_height = 800.0f;
  context.device_pixel_ratio = 2.0f;
  return context;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const MediaContext viewport = Viewport();

  const auto candidates = microbrowser::engine::ParseSrcset(input);
  if (candidates.size() > 64) {
    __builtin_trap();  // past the bound the parser declares
  }
  for (const auto& candidate : candidates) {
    if (candidate.url.size() > size) {
      __builtin_trap();  // a URL longer than the attribute it came from
    }
  }

  // A source size is a number the whole selection is scaled by, so it must be
  // finite and non-negative for every input -- a NaN here silently makes every
  // density comparison false and picks the last candidate every time.
  const float source_size = microbrowser::engine::ParseSizes(input, viewport);
  if (!(source_size >= 0.0f)) {
    __builtin_trap();
  }

  (void)microbrowser::css::MediaQueryListMatches(input, viewport);
  (void)microbrowser::css::ResolveAbsoluteLength(input, viewport);
  (void)microbrowser::engine::ImageTypeIsSupported(input);

  // And the whole algorithm, over a document built from the same bytes: the
  // `<picture>` walk is reachable only through a tree, and it is the half that
  // dereferences rather than the half that parses.
  std::string html = "<picture><source srcset=\"";
  html += std::string(input);
  html += "\" sizes=\"";
  html += std::string(input);
  html += "\"><img src=\"a.png\" srcset=\"";
  html += std::string(input);
  html += "\"></picture>";
  const std::unique_ptr<microbrowser::dom::Document> document =
      microbrowser::html::ParseDocument(html);
  if (const microbrowser::dom::Element* image = document->FirstElementByTagName("img");
      image != nullptr) {
    (void)microbrowser::engine::SelectImageSource(*image, viewport);
  }
  return 0;
}

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "css/MediaQuery.h"
#include "dom/Node.h"

namespace microbrowser::engine {

// HTML's image selection: `srcset`, `sizes` and `<picture>`.
//
// Here rather than in src/html for the same reason FormAlgorithms is: it is an
// HTML algorithm that needs more than src/html is allowed to see. Choosing a
// candidate means evaluating a media condition and a CSS length, and src/html
// may name only util, url and dom. Putting it here keeps one CSS grammar in
// src/css instead of a second, smaller, differently-wrong copy of it.
//
// Everything below is a pure function of an element and an environment. No
// fetching, no caching, no state: the caller decides what to do with the URL,
// which is what lets the page ask the same question at collection time and at
// layout time and get the same answer.

// One entry of a `srcset` attribute, before normalisation.
//
// A candidate carries the descriptor it was written with rather than a
// resolved density, because a `w` descriptor cannot be resolved without the
// source size -- and the source size comes from `sizes`, which belongs to the
// element rather than to the candidate.
struct ImageCandidate {
  std::string url;
  float density = 1.0f;
  float width = 0.0f;
  bool has_width = false;
};

// Parses a `srcset` attribute. Written from the spec's algorithm rather than by
// splitting on commas: a URL may contain a comma, so `a.png,b.png 2x` is one
// candidate and `a.png, b.png 2x` is two. Splitting first gets that backwards.
//
// A candidate whose descriptor is not a valid `w` or `x` is dropped, and the
// number of candidates is bounded -- an attribute is attacker-controlled text
// and this is a parser.
std::vector<ImageCandidate> ParseSrcset(std::string_view srcset);

// The source size in CSS pixels a `sizes` attribute names, or the viewport
// width when it names none. That default is the spec's `100vw`, and it is why a
// `w`-descriptor srcset with no `sizes` still selects sensibly.
float ParseSizes(std::string_view sizes, const css::MediaContext& context);

// Whether a `<source type>` names an image format this browser can decode.
//
// The honest-absence rule of ADR 0012 at the markup layer: claiming a format we
// cannot decode makes `<picture>` pick a source that renders as an empty box,
// where declining it picks the next source or the `<img>` fallback -- which is
// exactly what the author wrote it for.
bool ImageTypeIsSupported(std::string_view mime_type);

// The URL an `<img>` should load, exactly as written in the markup -- resolving
// it against the document is the loader's job. Empty when the element names no
// image at all.
std::string SelectImageSource(const dom::Element& image, const css::MediaContext& context);

}  // namespace microbrowser::engine

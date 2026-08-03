#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "gfx/Image.h"

namespace microbrowser::gfx {

// SVG rasterization, for the subset a page's artwork is actually written in.
//
// SVG is a document format, not a bitmap format, so this is a renderer rather
// than a decoder: it parses shapes and draws them through the same rasterizer
// the rest of this module uses, then hands back the pixels. That is why it
// lives here and not in a decoder of its own -- the machinery it needs is
// Path, Painter and Canvas, all of which are already next to it.
//
// **Supported**: `<svg>` with `width`, `height` and `viewBox`; `<g>` for
// grouping and inherited presentation; `<path>` with the full path grammar
// (M L H V C S Q T A Z, absolute and relative); `<rect>`, `<circle>`,
// `<ellipse>`, `<line>`, `<polygon>`, `<polyline>`; `fill`, `fill-rule`,
// `fill-opacity`, `stroke`, `stroke-width`, `stroke-opacity`, `opacity` and
// `transform` with translate/scale/matrix. Presentation attributes and the
// equivalent properties inside a `style` attribute both work.
//
// **Not supported, and refused rather than approximated**: text, gradients and
// patterns, filters, clip paths and masks, `<use>` and `<image>`, animation,
// and stylesheets. A gradient is not "a bit like a flat fill" -- a logo drawn
// with the wrong one of these is worse than a logo that did not draw.
//
// Every byte is attacker-controlled, and the shape of the danger here is not
// PNG's. There is no decompression to bomb and no pixel buffer to overflow;
// what an SVG can do instead is nest, recurse and iterate. The implementation
// bounds element depth, total element count, path command count, and the
// output size -- all before allocating anything proportional to them.

struct SvgDecodeResult {
  Image image;
  // Empty on success. A short reason on failure, for a log line and a test
  // message -- never shown to a user, and never derived from the input bytes.
  const char* error = nullptr;

  bool Ok() const { return error == nullptr && image.IsValid(); }
};

// Renders `bytes` at `width` x `height` device pixels.
//
// The size is a parameter rather than read from the document because that is
// what a vector format is for: the caller has already done layout and knows the
// box the artwork goes in. A non-positive size falls back to the document's own
// width and height, and to its viewBox after that.
SvgDecodeResult DecodeSvg(std::span<const std::byte> bytes, int width, int height);

// True when `bytes` looks like SVG: an XML declaration or a comment, then an
// `<svg` element. Sniffed rather than trusted from a Content-Type header, for
// the reason every browser sniffs -- the header is a claim by the server.
bool LooksLikeSvg(std::span<const std::byte> bytes);

// Bounds on what one document may contain, applied before any allocation
// proportional to them. Named here so a test can state them rather than
// rediscover them.
inline constexpr std::size_t kMaxSvgElements = 4096;
inline constexpr std::size_t kMaxSvgDepth = 32;
inline constexpr std::size_t kMaxSvgPathCommands = 65536;
inline constexpr int kMaxSvgEdge = 4096;

}  // namespace microbrowser::gfx

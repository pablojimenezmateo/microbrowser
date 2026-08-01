#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace microbrowser::tests {

// PNG files built in memory, for tests.
//
// Same reasoning as the synthetic font: a decoder needs inputs whose exact
// contents are known, including inputs that are deliberately wrong, and neither
// a checked-in corpus of images nor a dependency on a system encoder provides
// that.
//
// The image data is compressed with *stored* DEFLATE blocks — which is to say,
// not compressed at all. That is a feature: it means these fixtures exercise
// the PNG layer without the result depending on how well some compressor did,
// and a failure points at one decoder rather than two. Huffman-coded streams
// are covered separately, against data zlib itself produced.

struct PngSpec {
  int width = 0;
  int height = 0;
  int bit_depth = 8;
  // 0 grayscale, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA.
  int color_type = 6;
  bool interlaced = false;
  // RGB triples. Only used for colour type 3.
  std::vector<std::uint8_t> palette;
  // Per-palette-entry alpha, shorter than or equal to the palette.
  std::vector<std::uint8_t> transparency;
  // Unfiltered scanlines, packed, *without* the leading filter byte per row.
  std::vector<std::uint8_t> rows;
  // Filter to apply to every row, 0..4. The builder applies it, so the decoder
  // has to reverse it.
  std::uint8_t filter = 0;
};

std::vector<std::byte> BuildPng(const PngSpec& spec);

// Solid-colour RGBA rows, for the common case.
std::vector<std::uint8_t> SolidRgbaRows(int width, int height, std::uint8_t r, std::uint8_t g,
                                        std::uint8_t b, std::uint8_t a);

// A real PNG produced by an independent encoder, Huffman-coded rather than
// stored: 8x8 RGBA, a red-to-blue horizontal gradient with a vertical alpha
// ramp. Proves the decoder handles a stream it did not help construct.
std::vector<std::byte> ReferenceGradientPng();

// What ReferenceGradientPng contains, computed rather than remembered.
std::uint32_t ReferenceGradientPixel(int x, int y);

}  // namespace microbrowser::tests

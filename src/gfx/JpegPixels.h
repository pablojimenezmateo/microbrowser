#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microbrowser::gfx {

// The second half of JPEG decoding: coefficients to pixels.
//
// Private to gfx and split out of JpegDecoder.cpp because the two halves have
// different threat models. Everything on the other side of this header is a
// parser over attacker-chosen bytes, where a missed bound is a memory-safety
// bug. Everything on this side is arithmetic over values that have already been
// bounded — the sizes are the decoder's own, the coefficients are whatever fits
// in an int16, and the only way to get it wrong is to get the picture wrong.
// Keeping them in one file made both harder to read and neither easier to
// check, and the module's line cap is not the reason for the split.

// One component's coefficient plane, in natural (not zigzag) order, alongside
// the quantisation table its blocks were coded against.
struct JpegPlane {
  int blocks_per_line = 0;
  int blocks_per_column = 0;
  std::span<const std::int16_t> coefficients;
  const std::array<std::uint16_t, 64>* quantization = nullptr;
};

// Dequantises and inverse-transforms every block. The result is
// `blocks_per_line * 8` wide by `blocks_per_column * 8` tall, which is the
// padded size: the frame's own width and height are applied later, when the
// components are combined and the padding falls off the edge.
std::vector<std::uint8_t> RenderJpegPlane(const JpegPlane& plane);

// One rendered component, as ComposeJpegPixels reads it. `width` and `height`
// are the component's own size — the frame's, scaled by its sampling factors —
// which is smaller than the padded plane whenever the image does not fill its
// last block, and is the size the upsampling filter must clamp against. A
// filter that clamped against the padding would blend the edge of the picture
// with whatever the encoder put past it.
struct JpegSamplePlane {
  std::span<const std::uint8_t> samples;
  std::size_t stride = 0;
  int width = 0;
  int height = 0;
  int h = 1;
  int v = 1;
};

// Upsamples the components to the frame size and converts to ARGB8888.
// `count` is 1 (greyscale) or 3; `rgb` selects the Adobe RGB interpretation of
// three components over the usual YCbCr. Returns false if the output does not
// fit, which is the last place a size derived from the file is used.
bool ComposeJpegPixels(const std::array<JpegSamplePlane, 3>& planes, int count, int width,
                       int height, int max_h, int max_v, bool rgb,
                       std::vector<std::uint32_t>& out);

}  // namespace microbrowser::gfx

#pragma once

#include <cstddef>
#include <span>

#include "gfx/Image.h"

namespace microbrowser::gfx {

// JPEG decoding (ITU-T T.81 / ISO 10918-1), baseline and progressive.
//
// Ours rather than a vendored decoder, for the reason PngDecoder.h gives and
// ADR 0023 §1 restates: JPEG is Huffman, dequantise, IDCT, upsample, colour
// convert, with no compression-format dependency underneath it, so writing it
// means the bounds are ours to enforce rather than ours to trust. It is also
// the single highest-value decoder on the list — every photograph on the web —
// and every byte of it is attacker-controlled.
//
// The rules the implementation follows, which are the rules that matter:
//
//   * Dimensions are validated and the pixel count is bounded *before* any
//     allocation, against kMaxImagePixels. A 65535x65535 JPEG is four lines of
//     header and 17GB of output.
//   * Every size computed from the input is computed in 64 bits and checked.
//     `width * height * 4` in an int is the canonical image decoder heap
//     overflow, and this decoder multiplies more things than PNG does:
//     sampling factors multiply the block grid by up to four in each axis.
//   * A truncated entropy stream feeds zero bits rather than reading past the
//     end, so a file cut in half decodes to a partial picture instead of a
//     crash. That is what every decoder does and what the format expects.
//   * Every table index out of a scan header — quantisation table, Huffman
//     table, component selector — is range-checked at the point it is read,
//     because a scan is a second parser over state the frame header set up.
//
// Supported: 8-bit precision, baseline sequential (SOF0), extended sequential
// Huffman (SOF1) and progressive (SOF2); one or three components; sampling
// factors 1..4 in each axis; restart intervals.
//
// Not supported, and rejected rather than guessed at:
//
//   * Arithmetic coding (SOF9/SOF10) and lossless/hierarchical modes. Nothing
//     on the web is coded this way; the patent that caused that is expired and
//     the encoders never came back.
//   * 12-bit precision, and four-component CMYK/YCCK. A four-component JPEG is
//     an Adobe print asset whose ink polarity depends on a marker convention
//     that half the encoders in the world get wrong; drawing one wrong is worse
//     than drawing nothing, which is ADR 0012's rule applied to pixels.
//   * EXIF orientation. It is metadata about a decoded image rather than part
//     of decoding it, and the rotation belongs where the image is placed.

struct JpegDecodeResult {
  Image image;
  // Empty on success. A short reason on failure, for a log line and a test
  // message — never shown to a user, and never derived from the input bytes.
  const char* error = nullptr;

  bool Ok() const { return error == nullptr && image.IsValid(); }
};

JpegDecodeResult DecodeJpeg(std::span<const std::byte> bytes);

// True when `bytes` starts with SOI followed by a marker. Cheap, and the right
// way to pick a decoder: a Content-Type header is a claim by the server, and
// sniffing the bytes is what every browser actually does.
bool LooksLikeJpeg(std::span<const std::byte> bytes);

}  // namespace microbrowser::gfx

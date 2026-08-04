#pragma once

#include <cstddef>
#include <span>

#include "gfx/Image.h"

namespace microbrowser::gfx {

// PNG decoding (RFC 2083 / the W3C PNG specification).
//
// Ours rather than a vendored decoder, because image decoding is browser work
// and because image decoders are historically the most productive source of
// browser remote code execution — ADR 0004 isolates them in their own process
// for exactly that reason. A surface that dangerous is one to own, understand,
// and fuzz, not one to inherit.
//
// Every byte here is attacker-controlled. The rules the implementation follows,
// which are the rules that matter:
//
//   * Dimensions are validated and the pixel count is bounded *before* any
//     allocation. `width * height * 4` in an int is the canonical image decoder
//     heap overflow; every such product is computed in 64 bits and checked.
//   * The decompressed size is bounded by what IHDR says the image needs, so a
//     zip bomb in an IDAT fails instead of expanding.
//   * Chunk lengths are checked against the bytes that remain, never trusted.
//   * A CRC mismatch fails the decode. A corrupt chunk rendered anyway is a
//     picture of the corruption.
//
// Supported: bit depths 1, 2, 4, 8 and 16; all five colour types; palettes with
// tRNS; Adam7 interlacing. Not supported, and rejected rather than guessed at:
// APNG animation, and colour management beyond ignoring the relevant chunks.

struct PngDecodeResult {
  Image image;
  // Empty on success. A short reason on failure, for a log line and a test
  // message — never shown to a user, and never derived from the input bytes.
  const char* error = nullptr;

  bool Ok() const { return error == nullptr && image.IsValid(); }
};

// The bound on decoded pixels this decoder enforces before allocating lives in
// gfx/Image.h, because there is more than one decoder and one of them would
// drift.

PngDecodeResult DecodePng(std::span<const std::byte> bytes);

// True when `bytes` starts with the PNG signature. Cheap, and the right way to
// pick a decoder: a Content-Type header is a claim by the server, and sniffing
// the bytes is what every browser actually does.
bool LooksLikePng(std::span<const std::byte> bytes);

}  // namespace microbrowser::gfx

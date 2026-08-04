#include <cstddef>
#include <cstdint>
#include <span>

#include "gfx/JpegDecoder.h"

// The JPEG decoder, fed arbitrary bytes.
//
// Image decoders are historically the most productive source of browser remote
// code execution, which is why ADR 0004 gives them their own sandboxed process
// and ADR 0023 §2 requires this file to land on the same commit as the decoder.
// Until that process exists this fuzzer is the containment.
//
// JPEG has more places to get this wrong than PNG does. There is no CRC, so
// nothing rejects a corrupt segment; the sampling factors multiply the block
// grid by up to four in each axis; a progressive file reads back coefficients a
// previous scan wrote; and the entropy stream ends wherever a 0xFF says it
// does, which an attacker chooses.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
  const microbrowser::gfx::JpegDecodeResult result = microbrowser::gfx::DecodeJpeg(input);
  if (result.Ok()) {
    // Touch every pixel, so a decode that lied about its dimensions is caught
    // by the sanitizer rather than by a later reader.
    volatile std::uint32_t sink = 0;
    for (int y = 0; y < result.image.Height(); ++y) {
      const std::uint32_t* row = result.image.Row(y);
      for (int x = 0; x < result.image.Width(); ++x) {
        sink = sink ^ row[x];
      }
    }
    (void)sink;
  }
  return 0;
}

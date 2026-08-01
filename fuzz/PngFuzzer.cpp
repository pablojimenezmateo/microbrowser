#include <cstddef>
#include <cstdint>
#include <span>

#include "gfx/PngDecoder.h"

// The PNG decoder, fed arbitrary bytes.
//
// Image decoders are historically the most productive source of browser remote
// code execution, which is why ADR 0004 gives them their own sandboxed process.
// Until that process exists this fuzzer is the containment.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
  const microbrowser::gfx::PngDecodeResult result = microbrowser::gfx::DecodePng(input);
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

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gfx/SvgDecoder.h"
#include "gfx/SvgPath.h"

// The SVG renderer, fed arbitrary bytes.
//
// The danger here is not PNG's. There is no decompression to bomb and no pixel
// buffer whose size comes from the input; what a hostile SVG can do instead is
// nest, iterate and recurse, and drive a rasterizer with coordinates chosen to
// find every edge case in its fixed-point conversion. Both the document walk
// and the path grammar are reachable, so both are fuzzed -- the path parser
// directly as well, because a `d` attribute is where the real grammar is and
// reaching it through a document costs the fuzzer most of its budget in XML.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);

  // The size is derived from the input so that the fuzzer explores the scaling
  // arithmetic too, and bounded so it does not spend its whole budget
  // rasterizing one enormous surface.
  const int width = size == 0 ? 16 : 1 + (data[0] % 64);
  const int height = size < 2 ? 16 : 1 + (data[1] % 64);

  microbrowser::gfx::SvgDecodeResult result = microbrowser::gfx::DecodeSvg(input, width, height);
  if (result.Ok()) {
    // Touch every pixel, so a surface that lied about its dimensions is caught
    // by the sanitizer here rather than by a later reader.
    volatile std::uint32_t sink = 0;
    for (int y = 0; y < result.image.Height(); ++y) {
      const std::uint32_t* row = result.image.Row(y);
      for (int x = 0; x < result.image.Width(); ++x) {
        sink = sink ^ row[x];
      }
    }
    (void)sink;
  }

  (void)microbrowser::gfx::LooksLikeSvg(input);

  microbrowser::gfx::Path path;
  microbrowser::gfx::ParseSvgPathData(
      std::string_view(reinterpret_cast<const char*>(data), size), path,
      microbrowser::gfx::kMaxSvgPathCommands);
  return 0;
}

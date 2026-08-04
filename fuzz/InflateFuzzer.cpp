#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "util/Inflate.h"

// DEFLATE and its zlib wrapper, fed arbitrary bytes.
//
// This is the decompressor for PNG image data and for HTTP Content-Encoding, so
// every byte it ever sees was written by a stranger. The bound passed here is
// deliberately generous but finite: the property being fuzzed is that no input
// reads out of bounds or fails to terminate, not that a particular limit holds.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);

  std::vector<std::byte> out;
  microbrowser::util::Inflate(input, 1u << 20, out);

  out.clear();
  microbrowser::util::ZlibInflate(input, 1u << 20, out);

  // The gzip member header is the newest hostile surface here: two optional
  // NUL-terminated strings, a length-driven extra field and a header checksum,
  // all of it walked before a single bit of the deflate stream is read.
  out.clear();
  microbrowser::util::GzipInflate(input, 1u << 20, out);

  microbrowser::util::Adler32(input);
  microbrowser::util::Crc32(input);
  return 0;
}

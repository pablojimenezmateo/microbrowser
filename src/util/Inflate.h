#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microbrowser::util {

// DEFLATE (RFC 1951) and its zlib wrapper (RFC 1950).
//
// Ours rather than zlib's, for two reasons that are both about this project
// rather than about zlib. It is the decompressor for PNG *and* for HTTP
// Content-Encoding, so it sits on two different streams of bytes written by
// strangers; owning it means the fuzzing surface is ours to cover rather than a
// dependency's to have covered. And it lives in util, where both gfx and net
// can reach it — a copy in gfx would make the network stack depend on the
// rasterizer to read a gzip response.
//
// Written for auditability over speed, closely following the structure of
// zlib's own `puff.c` reference decoder: symbols are decoded a bit at a time
// against a canonical Huffman table rather than through a multi-level lookup.
// That is several times slower than production zlib and it is the right
// starting point, because the first requirement of a decoder fed hostile input
// is that a reader can convince themselves it is correct.
//
// **`max_output` bounds the result before anything is allocated.** Decompression
// is the canonical amplification attack: a few hundred bytes expand to
// gigabytes, and a decoder that grows its buffer until it succeeds is a
// denial-of-service primitive. Every caller knows its own bound — a PNG knows
// its dimensions from IHDR — and passing one is not optional.
//
// Returns false for malformed input, for output that would exceed `max_output`,
// and for a truncated stream. `out` is cleared first and its contents are
// unspecified on failure: a partial inflate is not a result.

bool Inflate(std::span<const std::byte> input, std::size_t max_output,
             std::vector<std::byte>& out);

// Same, with the two-byte zlib header and the Adler-32 trailer. The checksum is
// verified: a PNG whose IDAT does not check out is corrupt, and rendering it
// anyway means rendering whatever the corruption produced.
bool ZlibInflate(std::span<const std::byte> input, std::size_t max_output,
                 std::vector<std::byte>& out);

// Adler-32 (RFC 1950). Exposed because PNG's zlib trailer is not the only place
// it appears and a second implementation would be a second thing to get wrong.
std::uint32_t Adler32(std::span<const std::byte> data);

}  // namespace microbrowser::util

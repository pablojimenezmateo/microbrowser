#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace microbrowser::gfx {

// WOFF2, unwrapped back into the TrueType/OpenType file a font rasterizer can
// read.
//
// ADR 0024. Written here rather than taken as a dependency, because the container
// is a *parser* and this repository's rule is that a parser over bytes a stranger
// wrote is ours, bounds-checked, with a fuzz target on the same commit. The one
// piece that is not ours is the brotli stream inside it, and that is the decoder
// ADR 0024 sanctions.
//
// The shape, because it is unlike every other container here: a WOFF2 file is a
// header, a table *directory* of known tags and lengths, and then **one brotli
// stream holding every table concatenated**. There is no per-table compression
// and no per-table offset -- a table's position is the sum of the lengths before
// it, which means a directory that lies about a length moves every table after it.
// That is the property every bound in the implementation is protecting.
//
// **The transformed half.** WOFF2 may also store `glyf` *transformed* -- the
// outlines re-encoded into seven parallel substreams -- and drop `loca` entirely,
// and that is not an exotic option: every face fonts.gstatic.com serves is
// transformed, so a decoder that refuses it accepts nothing anyone ships. The
// reconstruction is in Woff2Glyf.cpp and was checked outline-by-outline against an
// independent implementation before it was trusted, because the failure mode here is
// not a refusal -- it is a font whose glyphs are subtly the wrong shape.
//
// What is still refused is the `hmtx` transform, which no measured font uses, and a
// `glyf` whose substreams do not account for the table exactly.
struct Woff2Font {
  // The reassembled sfnt: the file a rasterizer would have been given directly.
  std::vector<std::byte> sfnt;
};

// Nothing when the bytes are not a WOFF2 this browser can reassemble -- malformed,
// truncated, over a bound, or transformed. `max_output` bounds the reassembled
// font for the reason every decompressor here is bounded: the sizes come from the
// file.
std::optional<Woff2Font> DecodeWoff2(std::span<const std::byte> input,
                                     std::size_t max_output = 32u * 1024u * 1024u);

// Whether these bytes begin with WOFF2's signature. Cheap, and the reason it is
// public: the font loader can tell a WOFF2 from an sfnt without attempting either.
bool IsWoff2(std::span<const std::byte> input);

}  // namespace microbrowser::gfx

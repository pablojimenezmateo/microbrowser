#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gfx/Woff2.h"
#include "util/PerformanceCounters.h"

// A WOFF2 file, from a server.
//
// The container is a table *directory* over one brotli stream, and that is what
// makes it worth fuzzing rather than trusting: a table's position is the sum of the
// lengths declared before it, so a directory that lies about one length moves every
// table after it. Every read of the decompressed stream is therefore an offset the
// file chose.
//
// The properties asserted are the two the caller relies on:
//
//   * **The reassembled font never exceeds the ceiling.** WOFF2 is the one place a
//     declared output size exists -- `totalSfntSize` -- and taking that claim
//     seriously is what makes the bound cheap; a file that lies about it must be
//     caught by the arithmetic afterwards.
//   * **A refusal is total.** Nothing is handed back, so a caller cannot read a
//     half-reassembled font -- which for a font means mangled glyphs rather than a
//     failure, and a bug nobody can attribute.
//   * **`loca` describes the `glyf` that was produced.** This is the invariant the
//     transformed half turns on: `loca` is not in the file, every offset in it is a
//     consequence of walking seven substreams in lockstep, and the last offset is
//     the length of the table that was built. A font where they disagree parses in
//     this decoder and reads a glyph out of the next table in FreeType.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) {
    return 0;
  }
  using namespace microbrowser;

  // The ceiling out of the input, so the boundary is explored rather than one
  // arbitrary limit, and small enough that an expanding stream is refused rather
  // than allocated.
  const std::size_t ceiling = static_cast<std::size_t>(data[0]) * 4096u + 64u;
  const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(data + 1), size - 1);

  const std::uint64_t reconstructions_before =
      util::ReadPerformanceCounter(util::PerfCounterId::GfxWoff2GlyfReconstructed);
  const std::optional<gfx::Woff2Font> font = gfx::DecodeWoff2(file, ceiling);
  if (font.has_value() && font->sfnt.size() > ceiling) {
    __builtin_trap();
  }
  // A decoded font is at least an sfnt header plus one directory entry, or the
  // directory arithmetic produced something no rasterizer could read.
  if (font.has_value() && font->sfnt.size() < 12u + 16u) {
    __builtin_trap();
  }
  // `IsWoff2` must agree with the decoder about the signature: a file the decoder
  // accepted that `IsWoff2` rejects would be a font loader that never tries it.
  if (font.has_value() && !gfx::IsWoff2(file)) {
    __builtin_trap();
  }
  // `loca` against `glyf`, but **only for a font whose `glyf` was reconstructed**.
  // For an untransformed font both tables are copied out of the file verbatim, and a
  // file is free to declare a `loca` that describes something else -- that is the
  // font's bug, not this decoder's, and asserting on it here would be asserting that
  // the decoder validates tables it is only carrying. Which one happened is a
  // question the counter already answers, so nothing needs a second directory
  // parser to find out.
  const bool reconstructed =
      util::ReadPerformanceCounter(util::PerfCounterId::GfxWoff2GlyfReconstructed) >
      reconstructions_before;
  if (font.has_value() && reconstructed) {
    const std::vector<std::byte>& sfnt = font->sfnt;
    const auto u16 = [&sfnt](std::size_t at) -> std::size_t {
      return (static_cast<std::size_t>(static_cast<std::uint8_t>(sfnt[at])) << 8) |
             static_cast<std::uint8_t>(sfnt[at + 1]);
    };
    const auto u32 = [&u16](std::size_t at) -> std::size_t {
      return (u16(at) << 16) | u16(at + 2);
    };
    std::size_t glyf_length = 0;
    std::size_t loca_offset = 0;
    std::size_t loca_length = 0;
    std::size_t head_offset = 0;
    bool have_glyf = false;
    bool have_loca = false;
    bool have_head = false;
    const std::size_t count = u16(4);
    for (std::size_t i = 0; i < count && 12u + (i + 1u) * 16u <= sfnt.size(); ++i) {
      const std::size_t entry = 12u + i * 16u;
      const std::size_t tag = u32(entry);
      if (tag == 0x676C7966u) {  // 'glyf'
        glyf_length = u32(entry + 12);
        have_glyf = true;
      } else if (tag == 0x6C6F6361u) {  // 'loca'
        loca_offset = u32(entry + 8);
        loca_length = u32(entry + 12);
        have_loca = true;
      } else if (tag == 0x68656164u) {  // 'head'
        head_offset = u32(entry + 8);
        have_head = true;
      }
    }
    if (have_glyf && have_loca && have_head && loca_length >= 4u &&
        loca_offset + loca_length <= sfnt.size() && head_offset + 52u <= sfnt.size()) {
      // Short or long is `head`'s to say -- guessing it from the table's length gets
      // a four-entry short `loca` wrong -- and either way the *last* entry is the end
      // of the glyph data.
      const bool short_format = u16(head_offset + 50u) == 0u;
      const std::size_t last =
          short_format ? u16(loca_offset + loca_length - 2u) * 2u
                       : u32(loca_offset + loca_length - 4u);
      if (last > glyf_length) {
        __builtin_trap();
      }
    }
  }

  // And a ceiling of zero can produce nothing.
  if (gfx::DecodeWoff2(file, 0).has_value()) {
    __builtin_trap();
  }
  return 0;
}

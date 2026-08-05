#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Private to src/gfx: the reader both halves of the WOFF2 decoder read with, and
// the transformed-`glyf` reconstruction the container half calls into.
//
// It is one header rather than two because the reader is the whole reason the
// reconstruction is safe to write at all -- every length in a transformed `glyf`
// comes from the file, including the ones that say how many points a contour has,
// and a second reader with slightly different overflow behaviour is how one of
// them ends up unchecked.
namespace microbrowser::gfx::woff2 {

// A bounds-checked, sticky-failing reader. The same shape src/media's BoxReader
// uses, and for the same reason: once a read has gone past the end, every later
// read has to keep saying so rather than answering about a different offset.
class Reader {
 public:
  Reader() = default;
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool Ok() const { return ok_; }
  std::size_t Offset() const { return at_; }
  std::size_t Size() const { return bytes_.size(); }
  bool AtEnd() const { return at_ >= bytes_.size(); }

  std::uint8_t U8() {
    if (!Ok() || at_ + 1 > bytes_.size()) {
      ok_ = false;
      return 0;
    }
    return static_cast<std::uint8_t>(bytes_[at_++]);
  }
  std::uint16_t U16() {
    const std::uint16_t high = U8();
    return static_cast<std::uint16_t>((high << 8) | U8());
  }
  std::int16_t I16() { return static_cast<std::int16_t>(U16()); }
  std::uint32_t U32() {
    const std::uint32_t high = U16();
    return (high << 16) | U16();
  }

  // `count` bytes, or nothing and a failed reader. Used for the two places a
  // WOFF2 substream is copied rather than interpreted -- a composite glyph's
  // components and a glyph's instructions.
  std::span<const std::byte> Take(std::size_t count) {
    if (!Ok() || count > bytes_.size() - std::min(at_, bytes_.size())) {
      ok_ = false;
      return {};
    }
    const std::span<const std::byte> taken = bytes_.subspan(at_, count);
    at_ += count;
    return taken;
  }

  // The bytes between two offsets this reader already walked past. The composite
  // glyph case needs it: the components are *parsed* to find where they end and
  // then copied verbatim, because re-serializing a component transform would be a
  // second encoder to disagree with the first.
  std::span<const std::byte> Between(std::size_t from, std::size_t to) const {
    if (from > to || to > bytes_.size()) {
      return {};
    }
    return bytes_.subspan(from, to - from);
  }

  // WOFF2's `UIntBase128`: a big-endian base-128 number, at most five bytes, with
  // no leading zero byte allowed. The two refusals are the specification's and
  // both matter: without the length cap a stream of 0x80 bytes reads forever, and
  // without the leading-zero rule one number has many spellings -- which for a
  // *length* means two files that describe different fonts compare equal.
  bool Base128(std::uint32_t& out) {
    std::uint32_t result = 0;
    for (int i = 0; i < 5; ++i) {
      const std::uint8_t byte = U8();
      if (!Ok()) {
        return false;
      }
      if (i == 0 && byte == 0x80) {
        return false;  // a leading zero
      }
      if ((result & 0xFE000000u) != 0) {
        return false;  // the next shift would overflow 32 bits
      }
      result = (result << 7) | static_cast<std::uint32_t>(byte & 0x7Fu);
      if ((byte & 0x80u) == 0) {
        out = result;
        return true;
      }
    }
    return false;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

// The two tables a transformed `glyf` becomes. `loca` is not stored in a WOFF2 at
// all -- its length is declared as zero and every offset in it is a *consequence*
// of reconstructing the outlines, which is why the two come back together.
struct GlyfTables {
  std::vector<std::byte> glyf;
  std::vector<std::byte> loca;
  // The format the produced `loca` actually uses, which is not always the one the
  // file asked for: this decoder re-encodes each outline and can land above the
  // 131,070-byte ceiling a short `loca` can address. The caller writes this into
  // `head`, because `head.indexToLocFormat` disagreeing with `loca` is a font that
  // reads every glyph at the wrong offset.
  std::uint16_t index_format = 0;
};

// Nothing on any refusal, and every refusal counts itself. `glyph_count` is what
// `maxp` says: a transformed `glyf` carries its own count and the two disagreeing
// is a font whose last `loca` entry does not exist.
std::optional<GlyfTables> ReconstructGlyf(std::span<const std::byte> transformed,
                                          std::size_t max_output);

}  // namespace microbrowser::gfx::woff2

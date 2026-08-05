// WOFF2, the container.
//
// ADR 0024. The fixture below was produced by **brotli's own encoder**, from a
// generator that also emits the sfnt the decoder must reassemble -- so this checks
// the container against the format as an independent implementation writes it,
// rather than against this project's reading of the specification. Regenerate with
// the program recorded in docs/session-log.md session 20.
//
// The property is byte-exactness. A container's whole job is to hand back the file
// that went in, and "nearly" is a font with a table at the wrong offset -- which
// renders as mangled glyphs rather than as a failure.
//
// The second fixture is the one that matters, and it is a real font rather than a
// hand-built one: a *transformed* `glyf`, which is what every WOFF2 the web serves
// uses. It was built and compressed by fontTools -- an independent implementation
// of both halves -- and its expected reconstruction was checked outline by outline
// against fontTools' own reconstruction before being frozen here. The same check
// was run over two real Google Fonts faces (807 and 518 glyphs, 312 of them
// composite) with no mismatch; those are too large to embed and the procedure is in
// docs/session-log.md session 20.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/FontCatalog.h"
#include "gfx/Woff2.h"

namespace microbrowser::tests {

namespace {

// 70 bytes
constexpr std::uint8_t kWoff2[] = {
    0x77, 0x4F, 0x46, 0x32, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x12,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x06, 0x01, 0x08, 0x8B, 0x06, 0x80, 0x00, 0x00, 0x00, 0x01, 0xAA,
    0xBB, 0x00, 0x01, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x03};

constexpr std::uint8_t kExpectedSfnt[] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00,
    0x63, 0x6D, 0x61, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C,
    0x00, 0x00, 0x00, 0x06, 0x68, 0x65, 0x61, 0x64, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01,
    0xAA, 0xBB, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44};

constexpr std::uint8_t kWoff2Transformed[] = {
    0x77, 0x4F, 0x46, 0x32, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x54, 0x00, 0x0A, 0x00,
    0x00, 0x00, 0x00, 0x02, 0xBC, 0x00, 0x00, 0x01, 0x0E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x06, 0x60, 0x00, 0x40, 0x0A, 0x44, 0x56, 0x01, 0x36, 0x02, 0x24, 0x03,
    0x0A, 0x0B, 0x0A, 0x00, 0x04, 0x20, 0x05, 0x60, 0x07, 0x35, 0x1B, 0x0E, 0x02, 0x00, 0x9E,
    0x03, 0xEE, 0xCE, 0x22, 0xBA, 0x2C, 0x08, 0x03, 0x42, 0xDD, 0x2D, 0x39, 0x43, 0x8A, 0xF5,
    0x69, 0x04, 0xD5, 0xB2, 0xF5, 0xCC, 0xEE, 0xC7, 0x20, 0xFF, 0x81, 0xF2, 0x29, 0x58, 0x8C,
    0xA3, 0xAE, 0x3C, 0x52, 0x63, 0x15, 0x1E, 0x4F, 0xB6, 0x21, 0xAA, 0xF3, 0xF9, 0x3F, 0xF7,
    0x77, 0x6B, 0xC3, 0xE0, 0x8A, 0xE3, 0x03, 0x89, 0x68, 0xC1, 0x87, 0x30, 0xA0, 0x19, 0xFF,
    0x6C, 0x80, 0x02, 0x9E, 0x4C, 0xF9, 0x8F, 0xF3, 0x61, 0xB3, 0xE3, 0x23, 0x0A, 0x73, 0x09,
    0xA4, 0x2D, 0x4D, 0x24, 0xD1, 0x96, 0x5B, 0x11, 0x45, 0x9C, 0xE0, 0x93, 0xC1, 0x7B, 0xCA,
    0x22, 0xD1, 0x18, 0xF1, 0xF9, 0x17, 0x95, 0x6C, 0x28, 0x42, 0x15, 0x29, 0x80, 0x2B, 0x29,
    0xB0, 0x07, 0xDB, 0x9F, 0xED, 0x01, 0x48, 0x14, 0xAC, 0xC1, 0x08, 0xAC, 0xC1, 0xDA, 0x07,
    0x74, 0x55, 0x21, 0xED, 0xC1, 0x6E, 0x61, 0x0F, 0x56, 0xAB, 0x7D, 0x40, 0xA0, 0x22, 0x14,
    0x50, 0x41, 0x1F, 0x89, 0x01, 0x26, 0xE8, 0x62, 0x0C, 0x42, 0xD0, 0xAA, 0x5C, 0x4C, 0x3A,
    0xE3, 0xD1, 0xE4, 0xFE, 0x7A, 0x8C, 0xFD, 0x7B, 0x9F, 0x42, 0x4E, 0xFE, 0xE5, 0x3F, 0x41,
    0x57, 0xDC, 0xA8, 0x73, 0xE0, 0x56, 0x74, 0x43, 0x4B, 0x28, 0x04, 0x77, 0xBD, 0x3F, 0xE1,
    0x4C, 0xFD, 0x16, 0xA3, 0xBC, 0xC1, 0xCB, 0x55, 0x5E, 0xF7, 0xDB, 0x37, 0x14, 0x09, 0x82,
    0xB2, 0xD7, 0x0E, 0xD9, 0x40, 0x41, 0x16, 0x2F, 0x01, 0x00, 0x88, 0xD6, 0x34, 0x53, 0xA8,
    0x3A, 0x02, 0x9C, 0xE0, 0xB1, 0xC0, 0x65, 0x85, 0xD0, 0x23, 0x23, 0xE7, 0x9E, 0x22, 0xEA,
    0x40, 0x98, 0x9A, 0x66, 0x64, 0xD9, 0xF8, 0xF5, 0xB5, 0xED, 0x4B, 0xFE, 0x62, 0xEA, 0x15,
    0xF0, 0xD5, 0x2E, 0x9B, 0xDD, 0x22, 0x17, 0xC8, 0x95, 0x76, 0x9D, 0xD8, 0x82, 0x26, 0x4D,
    0x25, 0x52, 0x89, 0xD2, 0x3D, 0xB6, 0x60, 0xB2, 0x7D, 0x02,
};

constexpr std::uint8_t kExpectedGlyf[] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2C, 0x02, 0xBC, 0x00, 0x03, 0x00, 0x00, 0x31,
    0x21, 0x11, 0x21, 0x01, 0x2C, 0xFE, 0xD4, 0x02, 0xBC, 0x00, 0x01, 0x00, 0x32, 0x00, 0x00,
    0x01, 0xF4, 0x01, 0xF4, 0x00, 0x04, 0x00, 0x00, 0x33, 0x21, 0x36, 0x27, 0x21, 0x32, 0x01,
    0x90, 0x32, 0x32, 0xFE, 0x70, 0xFA, 0xFA, 0xFF, 0xFF, 0x00, 0x96, 0x00, 0x3C, 0x02, 0x58,
    0x02, 0x30, 0x00, 0x06, 0x00, 0x01, 0x64, 0x3C,
};

constexpr std::uint8_t kExpectedLoca[] = {
    0x00, 0x00, 0x00, 0x0C, 0x00, 0x1A, 0x00, 0x1A, 0x00, 0x22,
};

std::span<const std::byte> AsBytes(const std::uint8_t* data, std::size_t size) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
}

std::vector<std::byte> Bytes(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> span = AsBytes(data, size);
  return std::vector<std::byte>(span.begin(), span.end());
}

// One table out of a reassembled sfnt, found the way a rasterizer finds it: through
// the directory this decoder wrote. Nothing here trusts the order tables were
// written in, because WOFF2 preserves the *original* font's order and that is not
// sorted by tag.
std::span<const std::byte> Table(const std::vector<std::byte>& sfnt, const char* tag) {
  if (sfnt.size() < 12) {
    return {};
  }
  const auto u16 = [&sfnt](std::size_t at) {
    return static_cast<std::size_t>(
               static_cast<std::uint8_t>(sfnt[at])) << 8 |
           static_cast<std::uint8_t>(sfnt[at + 1]);
  };
  const auto u32 = [&u16](std::size_t at) { return (u16(at) << 16) | u16(at + 2); };
  const std::size_t count = u16(4);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t entry = 12 + i * 16;
    if (entry + 16 > sfnt.size()) {
      return {};
    }
    if (std::memcmp(sfnt.data() + entry, tag, 4) != 0) {
      continue;
    }
    const std::size_t offset = u32(entry + 8);
    const std::size_t length = u32(entry + 12);
    if (offset + length > sfnt.size()) {
      return {};
    }
    return std::span<const std::byte>(sfnt).subspan(offset, length);
  }
  return {};
}

bool SameBytes(std::span<const std::byte> actual, const std::uint8_t* expected,
               std::size_t size) {
  return actual.size() == size && std::memcmp(actual.data(), expected, size) == 0;
}

}  // namespace

void RegisterWoff2Tests(std::vector<TestCase>& tests) {
  AddTest(tests, "Woff2/ReassemblesTheSfntByteForByte", [] {
    const std::optional<gfx::Woff2Font> font =
        gfx::DecodeWoff2(AsBytes(kWoff2, sizeof(kWoff2)));
    Expect(font.has_value(), "the fixture decodes");
    ExpectEqInt(static_cast<long long>(font->sfnt.size()), sizeof(kExpectedSfnt),
                "the reassembled file is the size the header declared");
    // Byte for byte, because a container that is nearly right is a font with a
    // table at the wrong offset -- and that renders as mangled glyphs rather than
    // as a failure anyone can attribute.
    Expect(std::memcmp(font->sfnt.data(), kExpectedSfnt, sizeof(kExpectedSfnt)) == 0,
           "and every byte of it, including the padding between tables");
  });

  AddTest(tests, "Woff2/IsWoff2AgreesWithTheDecoderAboutTheSignature", [] {
    Expect(gfx::IsWoff2(AsBytes(kWoff2, sizeof(kWoff2))), "the fixture is one");
    static constexpr std::uint8_t kSfntSignature[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    Expect(!gfx::IsWoff2(AsBytes(kSfntSignature, sizeof(kSfntSignature))),
           "a plain sfnt is not");
    // A font loader asks this before it tries either path, so a disagreement here is
    // a container that is never attempted.
    Expect(!gfx::DecodeWoff2(AsBytes(kSfntSignature, sizeof(kSfntSignature))).has_value(),
           "and the decoder refuses it too");
  });

  AddTest(tests, "Woff2/ATruncationAnywhereIsARefusalRatherThanAShortFont", [] {
    for (std::size_t length = 0; length < sizeof(kWoff2); ++length) {
      Expect(!gfx::DecodeWoff2(AsBytes(kWoff2, length)).has_value(),
             "a truncated container must not produce a font: a font missing its last "
             "table is not a smaller font, it is a different one");
    }
  });

  AddTest(tests, "Woff2/TheDeclaredSizeIsRefusedFromItsOwnClaim", [] {
    // WOFF2 is the one place a container says how large it becomes, and taking that
    // claim seriously is what makes the bound cheap: nothing is decompressed at all
    // when the claim is already over the ceiling.
    Expect(!gfx::DecodeWoff2(AsBytes(kWoff2, sizeof(kWoff2)), 8).has_value(),
           "a ceiling under the declared size refuses before decompressing");
    Expect(gfx::DecodeWoff2(AsBytes(kWoff2, sizeof(kWoff2)), 4096).has_value(),
           "and a ceiling above it does not");
    Expect(!gfx::DecodeWoff2(AsBytes(kWoff2, sizeof(kWoff2)), 0).has_value(),
           "and nothing can be produced under a ceiling of zero");
  });

  AddTest(tests, "Woff2/EveryByteFlippedIsARefusalOrAFontAndNeverACrash", [] {
    // The container is a table *directory* over one brotli stream: a table's
    // position is the sum of the lengths declared before it, so one changed length
    // moves every table after it. This walks every byte rather than sampling,
    // because the interesting bytes are the length fields and they are four of
    // seventy.
    for (std::size_t i = 0; i < sizeof(kWoff2); ++i) {
      std::vector<std::uint8_t> mutated(kWoff2, kWoff2 + sizeof(kWoff2));
      mutated[i] = static_cast<std::uint8_t>(mutated[i] ^ 0xFF);
      const std::optional<gfx::Woff2Font> font =
          gfx::DecodeWoff2(AsBytes(mutated.data(), mutated.size()), 64u * 1024u);
      if (font.has_value()) {
        Expect(font->sfnt.size() <= 64u * 1024u, "a decoded font stays inside the ceiling");
      }
    }
  });

  AddTest(tests, "Woff2/ATransformedGlyfIsReconstructedFromItsSubstreams", [] {
    // **This replaces a test that asserted the opposite.** Until this session a
    // transformed `glyf` was refused whole, on the honest grounds that a
    // half-reconstruction is mangled glyphs rather than a failure. The refusal had
    // to go because it refused the entire web: fonts.gstatic.com transforms `glyf`
    // on every face it serves, so the container accepted nothing anyone ships.
    //
    // The bytes below are the outlines a rasterizer will see, and they are frozen
    // here byte-for-byte rather than described, because "the coordinates are
    // roughly right" is exactly the failure mode a reconstruction has.
    const std::optional<gfx::Woff2Font> font =
        gfx::DecodeWoff2(AsBytes(kWoff2Transformed, sizeof(kWoff2Transformed)));
    Expect(font.has_value(), "a transformed font decodes");
    Expect(SameBytes(Table(font->sfnt, "glyf"), kExpectedGlyf, sizeof(kExpectedGlyf)),
           "and its `glyf` is the outlines, re-encoded exactly");
    // `loca` is not in the file at all -- its declared length is zero -- so every
    // offset in it is a consequence of the reconstruction. It is the table that
    // proves the substreams were walked in lockstep: one glyph decoded a byte long
    // moves every offset after it.
    Expect(SameBytes(Table(font->sfnt, "loca"), kExpectedLoca, sizeof(kExpectedLoca)),
           "and `loca`, which the file does not contain, is rebuilt with it");
  });

  AddTest(tests, "Woff2/HeadAgreesWithTheLocaThatWasActuallyWritten", [] {
    // A short `loca` halves every offset, so `head.indexToLocFormat` and `loca`
    // have to say the same thing or the font reads every glyph at double or half
    // its offset. This decoder re-encodes outlines slightly larger than the
    // original, so it can be forced to a long `loca` where the file said short --
    // and then it must correct `head`.
    const std::optional<gfx::Woff2Font> font =
        gfx::DecodeWoff2(AsBytes(kWoff2Transformed, sizeof(kWoff2Transformed)));
    Expect(font.has_value(), "the fixture decodes");
    const std::span<const std::byte> head = Table(font->sfnt, "head");
    ExpectEqInt(static_cast<long long>(head.size()), 54, "head is a whole head");
    const int index_format = static_cast<int>(static_cast<std::uint8_t>(head[51]));
    const std::span<const std::byte> loca = Table(font->sfnt, "loca");
    // Four glyphs means five offsets: two bytes each when short, four when long.
    ExpectEqInt(static_cast<long long>(loca.size()), index_format == 0 ? 10 : 20,
                "and `loca` is the size that format implies");
  });

  AddTest(tests, "Woff2/TheRasterizerAcceptsAReconstructedFont", [] {
    // The end of the argument: FreeType is the only opinion that counts about
    // whether the reassembled bytes are a font, and it checks things this decoder
    // does not -- that `maxp` agrees with `loca`, that every glyph parses, that the
    // directory points where it says. A test that only compared bytes would pass on
    // a font nothing can open.
    gfx::FontLibrary library;
    gfx::FontCatalog catalog{library};
    Expect(catalog.RegisterWebFont("Fixture", 400, false,
                                   Bytes(kWoff2Transformed, sizeof(kWoff2Transformed))),
           "the transformed font registers");
    gfx::FontRequest request;
    request.families = {"Fixture"};
    request.weight = 400;
    Expect(catalog.FontFor(request) != nullptr, "and the family resolves to a face");
  });

  AddTest(tests, "Woff2/ATruncatedTransformedGlyfIsRefused", [] {
    // Every substream length is the file's, and a glyph's position in each one is
    // the sum of what the glyphs before it consumed. Truncation is the case that
    // reads past the end of a substream and into the next.
    for (std::size_t length = 0; length < sizeof(kWoff2Transformed); ++length) {
      Expect(!gfx::DecodeWoff2(AsBytes(kWoff2Transformed, length)).has_value(),
             "a truncated transformed font must not produce a font");
    }
  });

  AddTest(tests, "Woff2/ATransformedHmtxIsStillRefused", [] {
    // The limit that remains, and it is a small one: WOFF2 can also transform
    // `hmtx`, dropping the left-side-bearing arrays because they repeat the glyph
    // bounding boxes. Reconstructing it means reading every box back out of the
    // `glyf` that was just rebuilt, no measured font uses it, and a wrong `hmtx` is
    // text with the wrong spacing rather than a failure -- so it is refused.
    //
    // Built by hand from the untransformed fixture's header: one table, tag index 3
    // (`hmtx`), transform bits 1 -- which for everything except glyf and loca means
    // *transformed*.
    std::vector<std::uint8_t> transformed(kWoff2, kWoff2 + 48);
    transformed[13] = 1;                 // numTables = 1
    transformed.push_back(0x40u | 3u);   // flags: known tag `hmtx`, transform 1
    transformed.push_back(0x08);         // original length
    transformed.push_back(0x08);         // transformed length
    Expect(!gfx::DecodeWoff2(AsBytes(transformed.data(), transformed.size())).has_value(),
           "a transformed hmtx is refused");
  });

  AddTest(tests, "Woff2/EveryByteOfTheTransformedFixtureFlippedIsRefusedOrAFont", [] {
    // The transformed fixture walked byte by byte, because the bytes that matter
    // are the substream sizes, the per-contour point counts and the flag bytes --
    // and a wrong point count is the input that would read one substream into the
    // next. Nothing here asserts *which* answer comes back, only that it is an
    // answer: a refusal, or a font inside the ceiling.
    for (std::size_t i = 0; i < sizeof(kWoff2Transformed); ++i) {
      std::vector<std::uint8_t> mutated(kWoff2Transformed,
                                        kWoff2Transformed + sizeof(kWoff2Transformed));
      mutated[i] = static_cast<std::uint8_t>(mutated[i] ^ 0xFF);
      const std::optional<gfx::Woff2Font> font =
          gfx::DecodeWoff2(AsBytes(mutated.data(), mutated.size()), 64u * 1024u);
      if (font.has_value()) {
        Expect(font->sfnt.size() <= 64u * 1024u, "a decoded font stays inside the ceiling");
      }
    }
  });
}

}  // namespace microbrowser::tests

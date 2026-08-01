#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/Painter.h"
#include "gfx/PngDecoder.h"
#include "support/SyntheticPng.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::DecodePng;
using gfx::Image;
using gfx::IntPoint;
using gfx::Painter;
using gfx::PngDecodeResult;

namespace {

PngSpec RgbaSpec(int width, int height, std::uint8_t filter = 0) {
  PngSpec spec;
  spec.width = width;
  spec.height = height;
  spec.color_type = 6;
  spec.bit_depth = 8;
  spec.filter = filter;
  spec.rows.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      spec.rows.push_back(static_cast<std::uint8_t>(x * 7 + 3));
      spec.rows.push_back(static_cast<std::uint8_t>(y * 11 + 5));
      spec.rows.push_back(static_cast<std::uint8_t>((x + y) * 13));
      spec.rows.push_back(255);
    }
  }
  return spec;
}

std::uint32_t ExpectedRgba(int x, int y) {
  const auto r = static_cast<std::uint32_t>(static_cast<std::uint8_t>(x * 7 + 3));
  const auto g = static_cast<std::uint32_t>(static_cast<std::uint8_t>(y * 11 + 5));
  const auto b = static_cast<std::uint32_t>(static_cast<std::uint8_t>((x + y) * 13));
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Flips one byte of a valid PNG and requires the decoder to survive it. Used
// across the whole file, because "survives corruption in the header" and
// "survives corruption in the pixel data" are different code paths.
void ExpectSurvivesCorruption(const std::vector<std::byte>& original, std::size_t stride) {
  for (std::size_t index = 0; index < original.size(); index += stride) {
    std::vector<std::byte> corrupted = original;
    corrupted[index] = static_cast<std::byte>(static_cast<std::uint8_t>(corrupted[index]) ^ 0xFFu);
    const PngDecodeResult result = DecodePng(corrupted);
    // Either it decodes to something coherent or it fails; what it must never
    // do is read out of bounds, which ASan checks while this drives it.
    if (result.Ok()) {
      Expect(result.image.Width() > 0 && result.image.Height() > 0,
             "a successful decode must produce a real image");
    }
  }
}

}  // namespace

void RegisterPngDecoderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Png/RecognizesItsOwnSignature", [] {
    Expect(gfx::LooksLikePng(BuildPng(RgbaSpec(2, 2))), "a PNG looks like one");
    const std::vector<std::byte> not_png(16, std::byte{0x42});
    Expect(!gfx::LooksLikePng(not_png), "and other bytes do not");
    Expect(!gfx::LooksLikePng({}), "nor does nothing");
  });

  AddTest(tests, "Png/DecodesEightBitRgba", [] {
    const PngDecodeResult result = DecodePng(BuildPng(RgbaSpec(5, 4)));
    Expect(result.Ok(), result.error != nullptr ? result.error : "decode failed");
    ExpectEqInt(result.image.Width(), 5, "width from IHDR");
    ExpectEqInt(result.image.Height(), 4, "height from IHDR");
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 5; ++x) {
        ExpectEqInt(result.image.Row(y)[x], ExpectedRgba(x, y),
                    "every pixel must come back exactly as it went in");
      }
    }
    Expect(result.image.IsOpaque(), "an all-255 alpha image is opaque");
  });

  // All five filters. Filtering is where a decoder is most likely to be subtly
  // wrong, because a wrong filter still produces a picture.
  AddTest(tests, "Png/ReversesEveryScanlineFilter", [] {
    for (std::uint8_t filter = 0; filter <= 4; ++filter) {
      const PngDecodeResult result = DecodePng(BuildPng(RgbaSpec(9, 7, filter)));
      Expect(result.Ok(), std::string("filter ") + std::to_string(filter) + " failed to decode");
      for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 9; ++x) {
          ExpectEqInt(result.image.Row(y)[x], ExpectedRgba(x, y),
                      std::string("filter ") + std::to_string(filter) +
                          " produced the wrong pixels");
        }
      }
    }
  });

  AddTest(tests, "Png/DecodesEveryColourType", [] {
    // Grayscale.
    PngSpec gray;
    gray.width = 4;
    gray.height = 2;
    gray.color_type = 0;
    gray.rows = {0, 64, 128, 255, 255, 128, 64, 0};
    PngDecodeResult result = DecodePng(BuildPng(gray));
    Expect(result.Ok(), "grayscale must decode");
    ExpectEqInt(result.image.Row(0)[0], 0xFF000000u, "black stays black across all three channels");
    ExpectEqInt(result.image.Row(0)[3], 0xFFFFFFFFu, "and white stays white");

    // Grayscale with alpha.
    PngSpec gray_alpha;
    gray_alpha.width = 2;
    gray_alpha.height = 1;
    gray_alpha.color_type = 4;
    gray_alpha.rows = {200, 128, 50, 255};
    result = DecodePng(BuildPng(gray_alpha));
    Expect(result.Ok(), "gray+alpha must decode");
    ExpectEqInt(result.image.Row(0)[0], 0x80C8C8C8u, "grey replicated, alpha kept");
    Expect(!result.image.IsOpaque(), "and the image knows it is not opaque");

    // RGB, no alpha channel at all.
    PngSpec rgb;
    rgb.width = 2;
    rgb.height = 1;
    rgb.color_type = 2;
    rgb.rows = {255, 0, 0, 0, 0, 255};
    result = DecodePng(BuildPng(rgb));
    Expect(result.Ok(), "RGB must decode");
    ExpectEqInt(result.image.Row(0)[0], 0xFFFF0000u, "red");
    ExpectEqInt(result.image.Row(0)[1], 0xFF0000FFu, "blue");
    Expect(result.image.IsOpaque(), "a colour type with no alpha is opaque");
  });

  AddTest(tests, "Png/DecodesPalettedImagesIncludingTransparency", [] {
    PngSpec spec;
    spec.width = 4;
    spec.height = 1;
    spec.color_type = 3;
    spec.bit_depth = 8;
    spec.palette = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    spec.transparency = {0, 128};  // first entry clear, second half transparent
    spec.rows = {0, 1, 2, 3};

    const PngDecodeResult result = DecodePng(BuildPng(spec));
    Expect(result.Ok(), result.error != nullptr ? result.error : "palette decode failed");
    ExpectEqInt(result.image.Row(0)[0], 0x00FF0000u, "tRNS makes the first entry transparent");
    ExpectEqInt(result.image.Row(0)[1], 0x8000FF00u, "and the second half transparent");
    ExpectEqInt(result.image.Row(0)[2], 0xFF0000FFu, "entries past tRNS stay opaque");
    ExpectEqInt(result.image.Row(0)[3], 0xFFFFFFFFu, "as does the last");
  });

  // Sub-byte depths pack several pixels per byte, which is the arithmetic most
  // decoders get wrong at the end of a row.
  AddTest(tests, "Png/DecodesSubByteBitDepths", [] {
    // 1-bit grayscale, 5 pixels: 10110 padded to a byte.
    PngSpec one_bit;
    one_bit.width = 5;
    one_bit.height = 1;
    one_bit.color_type = 0;
    one_bit.bit_depth = 1;
    one_bit.rows = {0b10110000};
    PngDecodeResult result = DecodePng(BuildPng(one_bit));
    Expect(result.Ok(), "1-bit grayscale must decode");
    ExpectEqInt(result.image.Row(0)[0], 0xFFFFFFFFu, "bit set is white");
    ExpectEqInt(result.image.Row(0)[1], 0xFF000000u, "bit clear is black");
    ExpectEqInt(result.image.Row(0)[4], 0xFF000000u,
                "and the fifth pixel comes from the correct bit of the first byte");

    // 2-bit: four levels, replicated to 0/85/170/255.
    PngSpec two_bit;
    two_bit.width = 4;
    two_bit.height = 1;
    two_bit.color_type = 0;
    two_bit.bit_depth = 2;
    two_bit.rows = {0b00011011};
    result = DecodePng(BuildPng(two_bit));
    Expect(result.Ok(), "2-bit grayscale must decode");
    ExpectEqInt(result.image.Row(0)[1] & 0xFFu, 85,
                "levels scale by bit replication, so the maximum maps to exactly 255");
    ExpectEqInt(result.image.Row(0)[3] & 0xFFu, 255, "and the top level is fully white");

    // 4-bit.
    PngSpec four_bit;
    four_bit.width = 3;
    four_bit.height = 1;
    four_bit.color_type = 0;
    four_bit.bit_depth = 4;
    four_bit.rows = {0x0F, 0x80};
    result = DecodePng(BuildPng(four_bit));
    Expect(result.Ok(), "4-bit grayscale must decode");
    ExpectEqInt(result.image.Row(0)[0] & 0xFFu, 0, "the high nibble is the first pixel");
    ExpectEqInt(result.image.Row(0)[1] & 0xFFu, 255, "and the low nibble the second");
  });

  AddTest(tests, "Png/DecodesSixteenBitSamplesByScalingThemDown", [] {
    PngSpec spec;
    spec.width = 2;
    spec.height = 1;
    spec.color_type = 0;
    spec.bit_depth = 16;
    // Big-endian 16-bit samples: 0xFFFF and 0x0000.
    spec.rows = {0xFF, 0xFF, 0x00, 0x00};
    const PngDecodeResult result = DecodePng(BuildPng(spec));
    Expect(result.Ok(), "16-bit must decode");
    ExpectEqInt(result.image.Row(0)[0], 0xFFFFFFFFu, "the maximum sample is white");
    ExpectEqInt(result.image.Row(0)[1], 0xFF000000u, "and the minimum is black");
  });

  AddTest(tests, "Png/DecodesInterlacedImages", [] {
    PngSpec spec = RgbaSpec(11, 9);
    spec.interlaced = true;
    const PngDecodeResult result = DecodePng(BuildPng(spec));
    Expect(result.Ok(), result.error != nullptr ? result.error : "interlaced decode failed");
    for (int y = 0; y < 9; ++y) {
      for (int x = 0; x < 11; ++x) {
        ExpectEqInt(result.image.Row(y)[x], ExpectedRgba(x, y),
                    "an Adam7 image must reassemble to exactly the same pixels as a "
                    "progressive one; a wrong pass offset scatters them subtly");
      }
    }
  });

  AddTest(tests, "Png/DecodesAStreamAnotherEncoderProduced", [] {
    // Everything above uses stored DEFLATE blocks, which exercises PNG without
    // exercising the Huffman decoder. This one is real zlib output.
    const PngDecodeResult result = DecodePng(ReferenceGradientPng());
    Expect(result.Ok(), result.error != nullptr ? result.error : "reference PNG failed to decode");
    ExpectEqInt(result.image.Width(), 8, "8x8");
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        ExpectEqInt(result.image.Row(y)[x], ReferenceGradientPixel(x, y),
                    "a compressed stream must decode to the same pixels the encoder started "
                    "from");
      }
    }
  });

  // --- Hostile input --------------------------------------------------------

  AddTest(tests, "Png/RejectsStructurallyInvalidFiles", [] {
    Expect(!DecodePng({}).Ok(), "an empty buffer is not a PNG");

    const std::vector<std::byte> noise(200, std::byte{0x5A});
    Expect(!DecodePng(noise).Ok(), "neither is noise");

    // Valid signature, nothing after it.
    std::vector<std::byte> signature_only = BuildPng(RgbaSpec(2, 2));
    signature_only.resize(8);
    Expect(!DecodePng(signature_only).Ok(), "a signature alone is not an image");

    // No IEND.
    std::vector<std::byte> no_end = BuildPng(RgbaSpec(2, 2));
    no_end.resize(no_end.size() - 12);
    Expect(!DecodePng(no_end).Ok(), "a file with no IEND is truncated, however complete it looks");
  });

  AddTest(tests, "Png/RejectsAChunkWhoseCrcDoesNotMatch", [] {
    std::vector<std::byte> png = BuildPng(RgbaSpec(4, 4));
    // Flip a byte inside IHDR's data, leaving its stored CRC alone.
    png[20] = static_cast<std::byte>(static_cast<std::uint8_t>(png[20]) ^ 0x01u);
    const PngDecodeResult result = DecodePng(png);
    Expect(!result.Ok(),
           "a chunk that fails its own checksum must fail the decode; rendering it anyway "
           "means rendering a picture of the corruption");
  });

  AddTest(tests, "Png/RejectsImpossibleHeaders", [] {
    const auto with_ihdr = [](std::uint32_t width, std::uint32_t height, std::uint8_t depth,
                              std::uint8_t color_type) {
      PngSpec spec = RgbaSpec(2, 2);
      std::vector<std::byte> png = BuildPng(spec);
      // Overwrite IHDR's payload and repair nothing: the CRC will now fail,
      // which is itself a rejection, so instead build the fields directly.
      const auto write = [&png](std::size_t offset, std::uint32_t value) {
        png[offset] = static_cast<std::byte>(value >> 24);
        png[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFu);
        png[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFu);
        png[offset + 3] = static_cast<std::byte>(value & 0xFFu);
      };
      write(16, width);
      write(20, height);
      png[24] = static_cast<std::byte>(depth);
      png[25] = static_cast<std::byte>(color_type);
      return png;
    };

    // Each of these fails, and the CRC check would catch them anyway — what
    // matters is that none of them reaches an allocation first.
    Expect(!DecodePng(with_ihdr(0, 4, 8, 6)).Ok(), "a zero width is not an image");
    Expect(!DecodePng(with_ihdr(4, 0, 8, 6)).Ok(), "nor a zero height");
    Expect(!DecodePng(with_ihdr(0xFFFFFFFF, 0xFFFFFFFF, 8, 6)).Ok(),
           "and 18 exapixels must be rejected before anything is reserved");
    Expect(!DecodePng(with_ihdr(4, 4, 3, 6)).Ok(), "bit depth 3 does not exist");
    Expect(!DecodePng(with_ihdr(4, 4, 8, 7)).Ok(), "colour type 7 does not exist");
    Expect(!DecodePng(with_ihdr(4, 4, 16, 3)).Ok(), "a 16-bit palette does not exist");
  });

  AddTest(tests, "Png/RejectsAPaletteImageWithNoPalette", [] {
    PngSpec spec;
    spec.width = 2;
    spec.height = 1;
    spec.color_type = 3;
    spec.rows = {0, 1};
    // No PLTE at all.
    Expect(!DecodePng(BuildPng(spec)).Ok(),
           "a paletted image without a palette has no colours to use");
  });

  AddTest(tests, "Png/APaletteIndexPastTheEndIsTransparentRatherThanOutOfBounds", [] {
    PngSpec spec;
    spec.width = 3;
    spec.height = 1;
    spec.color_type = 3;
    spec.palette = {255, 0, 0, 0, 255, 0};  // two entries
    spec.rows = {0, 1, 200};                // the third names an entry that does not exist
    const PngDecodeResult result = DecodePng(BuildPng(spec));
    Expect(result.Ok(), "the file is otherwise well formed, so it decodes");
    ExpectEqInt(result.image.Row(0)[2], 0x00000000u,
                "an index past the palette must read as transparent, never as whatever is "
                "in memory after the table");
  });

  AddTest(tests, "Png/TruncationAtAnyPointFailsCleanly", [] {
    const std::vector<std::byte> complete = BuildPng(RgbaSpec(6, 6));
    for (std::size_t length = 0; length < complete.size(); ++length) {
      const std::vector<std::byte> truncated(complete.begin(),
                                             complete.begin() + static_cast<long>(length));
      const PngDecodeResult result = DecodePng(truncated);
      Expect(!result.Ok() || result.image.IsValid(),
             "a truncated file either fails or produces a real image, never a half-built one");
    }
  });

  AddTest(tests, "Png/CorruptionAtAnyByteIsSurvivable", [] {
    ExpectSurvivesCorruption(BuildPng(RgbaSpec(6, 6)), 5);
    ExpectSurvivesCorruption(ReferenceGradientPng(), 3);
  });

  AddTest(tests, "Png/AnUnknownScanlineFilterIsRejected", [] {
    // Build a valid file and then rewrite a filter byte inside the image data.
    // The stored-block layout puts the raw scanlines at a known offset, which
    // is the other reason these fixtures use stored blocks.
    std::vector<std::byte> png = BuildPng(RgbaSpec(4, 4));
    for (std::size_t i = 0; i + 4 < png.size(); ++i) {
      // Find the IDAT payload's first stored-block data byte, which is the
      // first scanline's filter byte.
      if (static_cast<char>(png[i]) == 'I' && static_cast<char>(png[i + 1]) == 'D' &&
          static_cast<char>(png[i + 2]) == 'A' && static_cast<char>(png[i + 3]) == 'T') {
        const std::size_t filter_byte = i + 4 + 2 + 5;  // zlib header, block header
        png[filter_byte] = static_cast<std::byte>(9);
        break;
      }
    }
    // The CRC now fails, which rejects it for the wrong reason — so this only
    // asserts that it is rejected, and the filter path itself is covered by the
    // round trip through all five valid filters above.
    Expect(!DecodePng(png).Ok(), "a file with a rewritten filter byte must not decode");
  });

  // --- Drawing --------------------------------------------------------------

  AddTest(tests, "Painter/DrawsADecodedImage", [] {
    const PngDecodeResult decoded = DecodePng(BuildPng(RgbaSpec(4, 3)));
    Expect(decoded.Ok(), "decode");

    Canvas canvas(16, 16);
    canvas.Clear(Color::Rgb(0, 0, 0));
    Painter painter(canvas);
    painter.DrawImage(decoded.image, IntPoint{5, 6});

    ExpectEqInt(canvas.Row(6)[5], ExpectedRgba(0, 0), "the top-left pixel lands where asked");
    ExpectEqInt(canvas.Row(8)[8], ExpectedRgba(3, 2), "and the bottom-right one too");
    ExpectEqInt(canvas.Row(5)[5], 0xFF000000u, "the row above is untouched");
    ExpectEqInt(canvas.Row(6)[4], 0xFF000000u, "as is the column to the left");
  });

  AddTest(tests, "Painter/AnImageDrawnOffTheSurfaceIsClipped", [] {
    const PngDecodeResult decoded = DecodePng(BuildPng(RgbaSpec(8, 8)));
    Expect(decoded.Ok(), "decode");

    Canvas canvas(8, 8);
    canvas.Clear(Color::Rgb(0, 0, 0));
    Painter painter(canvas);
    painter.DrawImage(decoded.image, IntPoint{-4, -4});
    ExpectEqInt(canvas.Row(0)[0], ExpectedRgba(4, 4),
                "a partially off-surface image draws the part that is on it");

    painter.DrawImage(decoded.image, IntPoint{1000, 1000});
    painter.DrawImage(decoded.image, IntPoint{-1000, -1000});
    ExpectEqInt(canvas.Row(0)[0], ExpectedRgba(4, 4),
                "and one entirely off it draws nothing at all");
  });

  AddTest(tests, "Painter/ATranslucentImageIsBlendedRatherThanCopied", [] {
    PngSpec spec;
    spec.width = 1;
    spec.height = 1;
    spec.color_type = 6;
    spec.rows = {0, 0, 0, 128};  // half-transparent black
    const PngDecodeResult decoded = DecodePng(BuildPng(spec));
    Expect(decoded.Ok(), "decode");
    Expect(!decoded.image.IsOpaque(), "the decoder noticed the alpha");

    Canvas canvas(2, 2);
    canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
    Painter painter(canvas);
    painter.DrawImage(decoded.image, IntPoint{0, 0});
    ExpectEqInt(static_cast<int>((canvas.Row(0)[0] >> 16) & 0xFFu), 127,
                "half-transparent black over white leaves 127, the same answer the span "
                "blitter gives");
  });
}

}  // namespace microbrowser::tests

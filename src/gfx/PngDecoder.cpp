#include "gfx/PngDecoder.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

#include "util/Inflate.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

enum class ColorType : std::uint8_t {
  Grayscale = 0,
  Rgb = 2,
  Palette = 3,
  GrayscaleAlpha = 4,
  Rgba = 6,
};

struct Header {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bit_depth = 0;
  ColorType color_type = ColorType::Grayscale;
  bool interlaced = false;

  int Channels() const {
    switch (color_type) {
      case ColorType::Grayscale:
      case ColorType::Palette:
        return 1;
      case ColorType::GrayscaleAlpha:
        return 2;
      case ColorType::Rgb:
        return 3;
      case ColorType::Rgba:
        return 4;
    }
    return 0;
  }

  // Bits per pixel, which for sub-byte depths is not a whole number of bytes
  // and is exactly where naive decoders go wrong.
  int BitsPerPixel() const { return Channels() * bit_depth; }
  int BytesPerPixel() const { return std::max(1, BitsPerPixel() / 8); }
};

std::uint32_t ReadU32(std::span<const std::byte> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

// CRC-32 as PNG defines it. The table is built once on first use rather than
// baked in, because a 1KB constant nobody can verify by eye is worse than eight
// lines that generate it.
const std::array<std::uint32_t, 256>& CrcTable() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      result[n] = c;
    }
    return result;
  }();
  return table;
}

std::uint32_t Crc32(std::span<const std::byte> data) {
  const auto& table = CrcTable();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte value : data) {
    crc = table[(crc ^ static_cast<std::uint32_t>(value)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

bool ValidHeader(const Header& header) {
  if (header.width == 0 || header.height == 0) {
    return false;
  }
  switch (header.color_type) {
    case ColorType::Grayscale:
      return header.bit_depth == 1 || header.bit_depth == 2 || header.bit_depth == 4 ||
             header.bit_depth == 8 || header.bit_depth == 16;
    case ColorType::Palette:
      return header.bit_depth == 1 || header.bit_depth == 2 || header.bit_depth == 4 ||
             header.bit_depth == 8;
    case ColorType::Rgb:
    case ColorType::GrayscaleAlpha:
    case ColorType::Rgba:
      return header.bit_depth == 8 || header.bit_depth == 16;
  }
  return false;
}

// The Paeth predictor, from the PNG spec. Written exactly as the spec states it
// rather than in a cleverer equivalent form, because "equivalent" is the word
// that hides the bug.
std::uint8_t Paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
  const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
  const int pa = std::abs(p - static_cast<int>(a));
  const int pb = std::abs(p - static_cast<int>(b));
  const int pc = std::abs(p - static_cast<int>(c));
  if (pa <= pb && pa <= pc) {
    return a;
  }
  return pb <= pc ? b : c;
}

// Reverses one scanline's filter in place. `previous` is the already-unfiltered
// row above, or all zeros for the first row.
bool Unfilter(std::uint8_t filter, std::uint8_t* row, const std::uint8_t* previous,
              std::size_t length, int bytes_per_pixel) {
  const auto bpp = static_cast<std::size_t>(bytes_per_pixel);
  switch (filter) {
    case 0:
      return true;
    case 1:
      for (std::size_t i = bpp; i < length; ++i) {
        row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
      }
      return true;
    case 2:
      for (std::size_t i = 0; i < length; ++i) {
        row[i] = static_cast<std::uint8_t>(row[i] + previous[i]);
      }
      return true;
    case 3:
      for (std::size_t i = 0; i < length; ++i) {
        const int left = i >= bpp ? row[i - bpp] : 0;
        row[i] = static_cast<std::uint8_t>(row[i] + ((left + previous[i]) / 2));
      }
      return true;
    case 4:
      for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t left = i >= bpp ? row[i - bpp] : 0;
        const std::uint8_t up_left = i >= bpp ? previous[i - bpp] : 0;
        row[i] = static_cast<std::uint8_t>(row[i] + Paeth(left, previous[i], up_left));
      }
      return true;
    default:
      // Not a filter type. Continuing with the row unfiltered would produce a
      // picture, which is worse than producing nothing.
      return false;
  }
}

// Extracts the `index`-th sample of `bit_depth` bits from a packed row.
std::uint32_t SampleAt(const std::uint8_t* row, std::size_t index, int bit_depth) {
  switch (bit_depth) {
    case 8:
      return row[index];
    case 16:
      // Sixteen-bit samples are scaled down to eight. Keeping them would double
      // the memory of every image for precision no display shows.
      return row[index * 2];
    default: {
      const int per_byte = 8 / bit_depth;
      const std::size_t byte = index / static_cast<std::size_t>(per_byte);
      const int shift = 8 - bit_depth * (static_cast<int>(index % static_cast<std::size_t>(per_byte)) + 1);
      const std::uint32_t mask = (1u << bit_depth) - 1u;
      return (row[byte] >> shift) & mask;
    }
  }
}

// Scales an n-bit sample to the full 0..255 range. Bit replication rather than
// a multiply-and-shift: it maps the maximum to exactly 255, which a naive
// `value * 255 / max` also does but a `value << (8 - depth)` does not.
std::uint8_t ScaleSample(std::uint32_t value, int bit_depth) {
  switch (bit_depth) {
    case 16:
    case 8:
      return static_cast<std::uint8_t>(value);
    case 4:
      return static_cast<std::uint8_t>(value * 17u);
    case 2:
      return static_cast<std::uint8_t>(value * 85u);
    case 1:
      return static_cast<std::uint8_t>(value * 255u);
    default:
      return 0;
  }
}

struct Palette {
  std::array<std::uint32_t, 256> entries{};
  std::size_t count = 0;
};

// Adam7 pass geometry, straight from the spec.
constexpr std::array<int, 7> kPassStartX = {0, 4, 0, 2, 0, 1, 0};
constexpr std::array<int, 7> kPassStartY = {0, 0, 4, 0, 2, 0, 1};
constexpr std::array<int, 7> kPassStepX = {8, 8, 4, 4, 2, 2, 1};
constexpr std::array<int, 7> kPassStepY = {8, 8, 8, 4, 4, 2, 2};

std::uint64_t PassWidth(const Header& header, int pass) {
  const auto step = static_cast<std::uint64_t>(kPassStepX[static_cast<std::size_t>(pass)]);
  const auto start = static_cast<std::uint64_t>(kPassStartX[static_cast<std::size_t>(pass)]);
  return header.width > start ? (header.width - start + step - 1) / step : 0;
}

std::uint64_t PassHeight(const Header& header, int pass) {
  const auto step = static_cast<std::uint64_t>(kPassStepY[static_cast<std::size_t>(pass)]);
  const auto start = static_cast<std::uint64_t>(kPassStartY[static_cast<std::size_t>(pass)]);
  return header.height > start ? (header.height - start + step - 1) / step : 0;
}

std::uint64_t RowBytes(const Header& header, std::uint64_t width) {
  // Rounded up: a row of sub-byte samples is padded to a byte boundary.
  return (width * static_cast<std::uint64_t>(header.BitsPerPixel()) + 7) / 8;
}

// Total unfiltered size, which is what bounds the inflate. Each scanline of
// each pass carries one leading filter byte.
std::uint64_t ExpectedRawSize(const Header& header) {
  if (!header.interlaced) {
    return (RowBytes(header, header.width) + 1) * header.height;
  }
  std::uint64_t total = 0;
  for (int pass = 0; pass < 7; ++pass) {
    const std::uint64_t width = PassWidth(header, pass);
    const std::uint64_t height = PassHeight(header, pass);
    if (width == 0 || height == 0) {
      continue;
    }
    total += (RowBytes(header, width) + 1) * height;
  }
  return total;
}

// One unfiltered scanline to ARGB, written into `out` at the given stride and
// starting position. Handles every colour type and bit depth.
void ExpandRow(const Header& header, const Palette& palette, const std::uint8_t* row,
               std::uint64_t width, std::uint32_t* out, int step) {
  const int depth = header.bit_depth;
  for (std::uint64_t x = 0; x < width; ++x) {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    switch (header.color_type) {
      case ColorType::Grayscale: {
        r = g = b = ScaleSample(SampleAt(row, x, depth), depth);
        break;
      }
      case ColorType::GrayscaleAlpha: {
        const std::size_t base = static_cast<std::size_t>(x) * 2;
        r = g = b = ScaleSample(SampleAt(row, base, depth), depth);
        a = ScaleSample(SampleAt(row, base + 1, depth), depth);
        break;
      }
      case ColorType::Rgb: {
        const std::size_t base = static_cast<std::size_t>(x) * 3;
        r = ScaleSample(SampleAt(row, base, depth), depth);
        g = ScaleSample(SampleAt(row, base + 1, depth), depth);
        b = ScaleSample(SampleAt(row, base + 2, depth), depth);
        break;
      }
      case ColorType::Rgba: {
        const std::size_t base = static_cast<std::size_t>(x) * 4;
        r = ScaleSample(SampleAt(row, base, depth), depth);
        g = ScaleSample(SampleAt(row, base + 1, depth), depth);
        b = ScaleSample(SampleAt(row, base + 2, depth), depth);
        a = ScaleSample(SampleAt(row, base + 3, depth), depth);
        break;
      }
      case ColorType::Palette: {
        const std::uint32_t index = SampleAt(row, x, depth);
        // An index past the palette is malformed. Transparent black is the
        // conservative answer: it draws nothing rather than reading past the
        // table or inventing a colour.
        const std::uint32_t entry = index < palette.count ? palette.entries[index] : 0u;
        out[static_cast<std::size_t>(x) * static_cast<std::size_t>(step)] = entry;
        continue;
      }
    }

    out[static_cast<std::size_t>(x) * static_cast<std::size_t>(step)] =
        (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
        (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }
}

}  // namespace

bool LooksLikePng(std::span<const std::byte> bytes) {
  if (bytes.size() < kSignature.size()) {
    return false;
  }
  for (std::size_t i = 0; i < kSignature.size(); ++i) {
    if (static_cast<std::uint8_t>(bytes[i]) != kSignature[i]) {
      return false;
    }
  }
  return true;
}

PngDecodeResult DecodePng(std::span<const std::byte> bytes) {
  PngDecodeResult result;
  AddPerformanceCounter(PerfCounterId::GfxPngDecodes);

  const auto fail = [&result](const char* reason) {
    result.error = reason;
    AddPerformanceCounter(PerfCounterId::GfxPngDecodeFailures);
    return result;
  };

  if (!LooksLikePng(bytes)) {
    return fail("not a PNG");
  }

  Header header;
  bool seen_header = false;
  bool seen_end = false;
  Palette palette;
  std::vector<std::byte> compressed;
  std::size_t offset = kSignature.size();

  while (offset + 8 <= bytes.size()) {
    const std::uint32_t length = ReadU32(bytes, offset);
    // Checked against what is actually left, in 64 bits, before it is used to
    // index anything. A chunk claiming four gigabytes must fail here.
    if (static_cast<std::uint64_t>(length) + 12 > bytes.size() - offset) {
      return fail("chunk length past end of file");
    }
    const std::size_t type_offset = offset + 4;
    const std::size_t data_offset = offset + 8;
    const std::span<const std::byte> data = bytes.subspan(data_offset, length);

    const std::uint32_t stored_crc = ReadU32(bytes, data_offset + length);
    if (Crc32(bytes.subspan(type_offset, length + 4)) != stored_crc) {
      return fail("chunk CRC mismatch");
    }

    char type[5] = {};
    for (int i = 0; i < 4; ++i) {
      type[i] = static_cast<char>(bytes[type_offset + static_cast<std::size_t>(i)]);
    }
    offset = data_offset + length + 4;

    if (std::strcmp(type, "IHDR") == 0) {
      if (seen_header || length != 13) {
        return fail("bad IHDR");
      }
      header.width = ReadU32(data, 0);
      header.height = ReadU32(data, 4);
      header.bit_depth = static_cast<std::uint8_t>(data[8]);
      header.color_type = static_cast<ColorType>(data[9]);
      const auto compression = static_cast<std::uint8_t>(data[10]);
      const auto filter = static_cast<std::uint8_t>(data[11]);
      const auto interlace = static_cast<std::uint8_t>(data[12]);
      if (compression != 0 || filter != 0 || interlace > 1) {
        return fail("unsupported compression, filter or interlace method");
      }
      header.interlaced = interlace == 1;
      if (!ValidHeader(header)) {
        return fail("bad dimensions or bit depth for colour type");
      }
      // The bound that makes every later size computation safe, in 64 bits and
      // before a single byte is reserved.
      if (static_cast<std::uint64_t>(header.width) * header.height > kMaxImagePixels) {
        return fail("image exceeds the pixel budget");
      }
      seen_header = true;
    } else if (std::strcmp(type, "PLTE") == 0) {
      if (!seen_header || length % 3 != 0 || length > 256 * 3) {
        return fail("bad PLTE");
      }
      palette.count = length / 3;
      for (std::size_t i = 0; i < palette.count; ++i) {
        palette.entries[i] = 0xFF000000u |
                             (static_cast<std::uint32_t>(data[i * 3]) << 16) |
                             (static_cast<std::uint32_t>(data[i * 3 + 1]) << 8) |
                             static_cast<std::uint32_t>(data[i * 3 + 2]);
      }
    } else if (std::strcmp(type, "tRNS") == 0) {
      if (!seen_header) {
        return fail("tRNS before IHDR");
      }
      if (header.color_type == ColorType::Palette) {
        if (length > palette.count) {
          return fail("tRNS longer than the palette");
        }
        for (std::size_t i = 0; i < length; ++i) {
          palette.entries[i] =
              (palette.entries[i] & 0x00FFFFFFu) |
              (static_cast<std::uint32_t>(data[i]) << 24);
        }
      }
      // For the non-palette colour types tRNS names a single transparent
      // colour. Ignored deliberately rather than half-implemented: it is rare,
      // and a wrong implementation of it is a wrong picture rather than a
      // failed decode.
    } else if (std::strcmp(type, "IDAT") == 0) {
      if (!seen_header) {
        return fail("IDAT before IHDR");
      }
      compressed.insert(compressed.end(), data.begin(), data.end());
    } else if (std::strcmp(type, "IEND") == 0) {
      seen_end = true;
      break;
    }
    // Every other chunk is ancillary and skipped. A decoder that failed on
    // chunks it did not recognize would reject most of the web.
  }

  if (!seen_header) {
    return fail("no IHDR");
  }
  if (!seen_end) {
    return fail("no IEND");
  }
  if (compressed.empty()) {
    return fail("no image data");
  }
  if (header.color_type == ColorType::Palette && palette.count == 0) {
    return fail("palette image with no palette");
  }

  const std::uint64_t raw_size = ExpectedRawSize(header);
  if (raw_size > kMaxImagePixels * 8) {
    return fail("image data exceeds the decompression budget");
  }

  std::vector<std::byte> raw;
  if (!util::ZlibInflate(compressed, static_cast<std::size_t>(raw_size), raw)) {
    return fail("bad image data stream");
  }
  if (raw.size() != raw_size) {
    // Short is truncation, long cannot happen because inflate was bounded.
    return fail("image data is the wrong size for the header");
  }

  std::vector<std::uint32_t> pixels(
      static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height), 0u);
  const int bytes_per_pixel = header.BytesPerPixel();
  auto* raw_bytes = reinterpret_cast<std::uint8_t*>(raw.data());
  std::size_t cursor = 0;

  const int passes = header.interlaced ? 7 : 1;
  std::vector<std::uint8_t> previous;
  for (int pass = 0; pass < passes; ++pass) {
    const std::uint64_t pass_width = header.interlaced ? PassWidth(header, pass) : header.width;
    const std::uint64_t pass_height = header.interlaced ? PassHeight(header, pass) : header.height;
    if (pass_width == 0 || pass_height == 0) {
      continue;
    }
    const auto row_bytes = static_cast<std::size_t>(RowBytes(header, pass_width));
    previous.assign(row_bytes, 0u);

    for (std::uint64_t y = 0; y < pass_height; ++y) {
      if (cursor + 1 + row_bytes > raw.size()) {
        return fail("truncated image data");
      }
      const std::uint8_t filter = raw_bytes[cursor++];
      std::uint8_t* row = raw_bytes + cursor;
      if (!Unfilter(filter, row, previous.data(), row_bytes, bytes_per_pixel)) {
        return fail("unknown scanline filter");
      }
      cursor += row_bytes;

      const std::uint64_t target_y =
          header.interlaced
              ? static_cast<std::uint64_t>(kPassStartY[static_cast<std::size_t>(pass)]) +
                    y * static_cast<std::uint64_t>(kPassStepY[static_cast<std::size_t>(pass)])
              : y;
      const std::uint64_t target_x =
          header.interlaced
              ? static_cast<std::uint64_t>(kPassStartX[static_cast<std::size_t>(pass)])
              : 0;
      const int step = header.interlaced ? kPassStepX[static_cast<std::size_t>(pass)] : 1;
      std::uint32_t* out = pixels.data() +
                           static_cast<std::size_t>(target_y) *
                               static_cast<std::size_t>(header.width) +
                           static_cast<std::size_t>(target_x);
      ExpandRow(header, palette, row, pass_width, out, step);

      std::memcpy(previous.data(), row, row_bytes);
    }
  }

  if (!result.image.Adopt(static_cast<int>(header.width), static_cast<int>(header.height),
                          std::move(pixels))) {
    return fail("could not adopt the decoded pixels");
  }
  AddPerformanceCounter(PerfCounterId::GfxPngPixelsDecoded,
                        static_cast<std::uint64_t>(header.width) * header.height);
  return result;
}

}  // namespace microbrowser::gfx

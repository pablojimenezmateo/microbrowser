#include "support/SyntheticPng.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace microbrowser::tests {

namespace {

std::uint32_t Crc32(const std::vector<std::uint8_t>& data) {
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
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::uint8_t value : data) {
    crc = table[(crc ^ value) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t Adler32(const std::vector<std::uint8_t>& data) {
  constexpr std::uint32_t kModulus = 65521;
  std::uint32_t low = 1;
  std::uint32_t high = 0;
  for (const std::uint8_t value : data) {
    low = (low + value) % kModulus;
    high = (high + low) % kModulus;
  }
  return (high << 16) | low;
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void AppendChunk(std::vector<std::uint8_t>& out, std::string_view type,
                 const std::vector<std::uint8_t>& data) {
  AppendU32(out, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> typed;
  for (const char c : type) {
    typed.push_back(static_cast<std::uint8_t>(c));
  }
  typed.insert(typed.end(), data.begin(), data.end());
  out.insert(out.end(), typed.begin(), typed.end());
  AppendU32(out, Crc32(typed));
}

// A zlib stream of stored DEFLATE blocks. No compression, every byte visible.
std::vector<std::uint8_t> StoredZlib(const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> out = {0x78, 0x01};
  std::size_t offset = 0;
  do {
    const std::size_t length = std::min<std::size_t>(data.size() - offset, 65535);
    const bool last = offset + length >= data.size();
    out.push_back(last ? 0x01u : 0x00u);
    out.push_back(static_cast<std::uint8_t>(length & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFFu));
    const auto complement = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length));
    out.push_back(static_cast<std::uint8_t>(complement & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((complement >> 8) & 0xFFu));
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
               data.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
  } while (offset < data.size());
  AppendU32(out, Adler32(data));
  return out;
}

int ChannelsFor(int color_type) {
  switch (color_type) {
    case 0:
    case 3:
      return 1;
    case 4:
      return 2;
    case 2:
      return 3;
    case 6:
      return 4;
    default:
      return 0;
  }
}

std::size_t RowBytes(const PngSpec& spec, int width) {
  const int bits = ChannelsFor(spec.color_type) * spec.bit_depth;
  return static_cast<std::size_t>((static_cast<std::int64_t>(width) * bits + 7) / 8);
}

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

// Applies the requested filter, so the decoder has to reverse it. The inverse
// of gfx::Unfilter, written out separately on purpose — a test that reuses the
// implementation it is testing verifies only that the code is self-consistent.
void ApplyFilter(std::uint8_t filter, const std::uint8_t* row, const std::uint8_t* previous,
                 std::size_t length, int bytes_per_pixel, std::vector<std::uint8_t>& out) {
  const auto bpp = static_cast<std::size_t>(bytes_per_pixel);
  for (std::size_t i = 0; i < length; ++i) {
    const std::uint8_t left = i >= bpp ? row[i - bpp] : 0;
    const std::uint8_t up = previous[i];
    const std::uint8_t up_left = i >= bpp ? previous[i - bpp] : 0;
    std::uint8_t value = 0;
    switch (filter) {
      case 0:
        value = row[i];
        break;
      case 1:
        value = static_cast<std::uint8_t>(row[i] - left);
        break;
      case 2:
        value = static_cast<std::uint8_t>(row[i] - up);
        break;
      case 3:
        value = static_cast<std::uint8_t>(row[i] - ((static_cast<int>(left) + up) / 2));
        break;
      default:
        value = static_cast<std::uint8_t>(row[i] - Paeth(left, up, up_left));
        break;
    }
    out.push_back(value);
  }
}

constexpr std::array<int, 7> kPassStartX = {0, 4, 0, 2, 0, 1, 0};
constexpr std::array<int, 7> kPassStartY = {0, 0, 4, 0, 2, 0, 1};
constexpr std::array<int, 7> kPassStepX = {8, 8, 4, 4, 2, 2, 1};
constexpr std::array<int, 7> kPassStepY = {8, 8, 8, 4, 4, 2, 2};

}  // namespace

std::vector<std::byte> BuildPng(const PngSpec& spec) {
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

  std::vector<std::uint8_t> ihdr;
  AppendU32(ihdr, static_cast<std::uint32_t>(spec.width));
  AppendU32(ihdr, static_cast<std::uint32_t>(spec.height));
  ihdr.push_back(static_cast<std::uint8_t>(spec.bit_depth));
  ihdr.push_back(static_cast<std::uint8_t>(spec.color_type));
  ihdr.push_back(0);  // compression method
  ihdr.push_back(0);  // filter method
  ihdr.push_back(spec.interlaced ? 1u : 0u);
  AppendChunk(out, "IHDR", ihdr);

  if (!spec.palette.empty()) {
    AppendChunk(out, "PLTE", spec.palette);
  }
  if (!spec.transparency.empty()) {
    AppendChunk(out, "tRNS", spec.transparency);
  }

  const int bytes_per_pixel =
      std::max(1, ChannelsFor(spec.color_type) * spec.bit_depth / 8);

  std::vector<std::uint8_t> raw;
  if (!spec.interlaced) {
    const std::size_t stride = RowBytes(spec, spec.width);
    std::vector<std::uint8_t> previous(stride, 0u);
    for (int y = 0; y < spec.height; ++y) {
      const std::uint8_t* row = spec.rows.data() + static_cast<std::size_t>(y) * stride;
      raw.push_back(spec.filter);
      ApplyFilter(spec.filter, row, previous.data(), stride, bytes_per_pixel, raw);
      std::memcpy(previous.data(), row, stride);
    }
  } else {
    // Adam7. `rows` is still the full unfiltered image; the passes are sampled
    // out of it here, which is what an encoder does.
    const std::size_t full_stride = RowBytes(spec, spec.width);
    const int sample_bytes = ChannelsFor(spec.color_type) * spec.bit_depth / 8;
    for (int pass = 0; pass < 7; ++pass) {
      const int start_x = kPassStartX[static_cast<std::size_t>(pass)];
      const int start_y = kPassStartY[static_cast<std::size_t>(pass)];
      const int step_x = kPassStepX[static_cast<std::size_t>(pass)];
      const int step_y = kPassStepY[static_cast<std::size_t>(pass)];
      const int pass_width = spec.width > start_x ? (spec.width - start_x + step_x - 1) / step_x : 0;
      const int pass_height =
          spec.height > start_y ? (spec.height - start_y + step_y - 1) / step_y : 0;
      if (pass_width == 0 || pass_height == 0) {
        continue;
      }
      const std::size_t stride = RowBytes(spec, pass_width);
      std::vector<std::uint8_t> previous(stride, 0u);
      std::vector<std::uint8_t> row(stride, 0u);
      for (int y = 0; y < pass_height; ++y) {
        for (int x = 0; x < pass_width; ++x) {
          const std::size_t source = static_cast<std::size_t>(start_y + y * step_y) * full_stride +
                                     static_cast<std::size_t>(start_x + x * step_x) *
                                         static_cast<std::size_t>(sample_bytes);
          std::memcpy(row.data() + static_cast<std::size_t>(x) *
                                       static_cast<std::size_t>(sample_bytes),
                      spec.rows.data() + source, static_cast<std::size_t>(sample_bytes));
        }
        raw.push_back(spec.filter);
        ApplyFilter(spec.filter, row.data(), previous.data(), stride, bytes_per_pixel, raw);
        std::memcpy(previous.data(), row.data(), stride);
      }
    }
  }

  AppendChunk(out, "IDAT", StoredZlib(raw));
  AppendChunk(out, "IEND", {});

  std::vector<std::byte> bytes;
  bytes.reserve(out.size());
  for (const std::uint8_t value : out) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

std::vector<std::uint8_t> SolidRgbaRows(int width, int height, std::uint8_t r, std::uint8_t g,
                                        std::uint8_t b, std::uint8_t a) {
  std::vector<std::uint8_t> rows;
  rows.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  for (int i = 0; i < width * height; ++i) {
    rows.push_back(r);
    rows.push_back(g);
    rows.push_back(b);
    rows.push_back(a);
  }
  return rows;
}

namespace {

// Produced by zlib, not by the builder above, so the decoder faces a stream it
// did not help construct. Regenerate with the script in InflateTests.cpp's
// comment, swapping the payload for an 8x8 RGBA gradient.
constexpr std::uint8_t kGradientPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xC4,
    0x0F, 0xBE, 0x8B, 0x00, 0x00, 0x00, 0x7D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x1D, 0xCA,
    0x51, 0x01, 0x02, 0x00, 0x08, 0xC4, 0xD0, 0x0B, 0x83, 0x5D, 0x30, 0x0C, 0x76, 0xC1, 0x30,
    0xD8, 0x05, 0xC3, 0x9C, 0xC3, 0xFD, 0xEE, 0x49, 0xB2, 0x42, 0x5F, 0xA5, 0x3E, 0x2A, 0xBD,
    0xD5, 0x7A, 0x69, 0xF4, 0xD4, 0xEA, 0xC1, 0xF9, 0xE7, 0x00, 0x04, 0x20, 0x00, 0x01, 0x08,
    0x40, 0x00, 0x02, 0x10, 0x07, 0x12, 0x90, 0x80, 0x04, 0x24, 0x20, 0x01, 0x09, 0x48, 0x40,
    0x1E, 0x28, 0x40, 0x01, 0x0A, 0x50, 0x80, 0x02, 0x14, 0xA0, 0x00, 0x75, 0xA0, 0x01, 0x0D,
    0x68, 0x40, 0x03, 0x1A, 0xD0, 0x80, 0x06, 0xF4, 0x81, 0x01, 0x0C, 0x60, 0x00, 0x03, 0x18,
    0xC0, 0x00, 0x06, 0x30, 0x07, 0x16, 0xB0, 0x80, 0x05, 0x2C, 0x60, 0x01, 0x0B, 0x58, 0xC0,
    0x1E, 0x30, 0xC0, 0x00, 0x03, 0x0C, 0x30, 0xC0, 0x00, 0x03, 0xFC, 0x03, 0x0B, 0x5D, 0x5F,
    0x89, 0xCB, 0xDC, 0x98, 0x71, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
    0x60, 0x82};

}  // namespace

std::vector<std::byte> ReferenceGradientPng() {
  std::vector<std::byte> bytes;
  bytes.reserve(sizeof(kGradientPng));
  for (const std::uint8_t value : kGradientPng) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

std::uint32_t ReferenceGradientPixel(int x, int y) {
  const auto r = static_cast<std::uint32_t>(x * 255 / 7);
  const std::uint32_t b = 255u - r;
  const auto a = static_cast<std::uint32_t>(y * 255 / 7);
  return (a << 24) | (r << 16) | b;
}

}  // namespace microbrowser::tests

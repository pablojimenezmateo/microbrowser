#include "gfx/JpegPixels.h"

#include <algorithm>
#include <cmath>

#include "gfx/Image.h"

namespace microbrowser::gfx {

namespace {

// The 1-D inverse DCT basis, as T.81 A.3.3 states it:
//
//     kBasis[u][x] = C(u)/2 * cos((2x + 1) u pi / 16),  C(0) = 1/sqrt(2)
//
// The transform is applied as two of these — rows then columns — because the
// 2-D IDCT is separable, and that is 1024 multiplies per block against 4096 for
// the direct form. Faster forms exist and every one of them is an approximation
// with its own error budget; this is the definition, and the block below that
// skips a row of zero coefficients is where the speed actually comes from,
// because in a real photograph most of them are zero.
const std::array<std::array<float, 8>, 8>& Basis() {
  static const std::array<std::array<float, 8>, 8> table = [] {
    std::array<std::array<float, 8>, 8> values{};
    for (int u = 0; u < 8; ++u) {
      const float scale = u == 0 ? 0.353553390593273762f : 0.5f;  // C(u)/2
      for (int x = 0; x < 8; ++x) {
        values[static_cast<std::size_t>(u)][static_cast<std::size_t>(x)] =
            scale * std::cos(static_cast<float>((2 * x + 1) * u) * 3.14159265358979324f / 16.0f);
      }
    }
    return values;
  }();
  return table;
}

std::uint8_t ClampSample(float value) {
  const int rounded = static_cast<int>(std::lround(value)) + 128;
  return static_cast<std::uint8_t>(std::clamp(rounded, 0, 255));
}

// Dequantises one block and inverse-transforms it into `out`, which is a
// `stride`-wide window of the plane.
void InverseTransformBlock(const std::int16_t* coefficients,
                           const std::array<std::uint16_t, 64>& quantization, std::uint8_t* out,
                           std::size_t stride) {
  const auto& basis = Basis();

  std::array<float, 64> dequantized{};
  bool flat = true;
  for (std::size_t i = 0; i < 64; ++i) {
    const int value = static_cast<int>(coefficients[i]) * static_cast<int>(quantization[i]);
    dequantized[i] = static_cast<float>(value);
    if (i > 0 && value != 0) {
      flat = false;
    }
  }
  if (flat) {
    // A block with no AC energy is one colour. Most blocks in a photograph's
    // chroma planes are this, and the whole transform is one multiply.
    const std::uint8_t value = ClampSample(dequantized[0] * 0.125f);
    for (std::size_t y = 0; y < 8; ++y) {
      std::fill_n(out + y * stride, 8, value);
    }
    return;
  }

  std::array<float, 64> rows{};
  for (std::size_t v = 0; v < 8; ++v) {
    const float* line = dequantized.data() + v * 8;
    bool zero = true;
    for (std::size_t u = 1; u < 8; ++u) {
      if (line[u] != 0.0f) {
        zero = false;
        break;
      }
    }
    if (zero) {
      const float value = line[0] * basis[0][0];
      std::fill_n(rows.data() + v * 8, 8, value);
      continue;
    }
    for (std::size_t x = 0; x < 8; ++x) {
      float sum = 0.0f;
      for (std::size_t u = 0; u < 8; ++u) {
        sum += basis[u][x] * line[u];
      }
      rows[v * 8 + x] = sum;
    }
  }

  for (std::size_t x = 0; x < 8; ++x) {
    for (std::size_t y = 0; y < 8; ++y) {
      float sum = 0.0f;
      for (std::size_t v = 0; v < 8; ++v) {
        sum += basis[v][y] * rows[v * 8 + x];
      }
      out[y * stride + x] = ClampSample(sum);
    }
  }
}

// Fixed point at 16 bits, from the JFIF conversion:
//   R = Y                        + 1.402   (Cr - 128)
//   G = Y - 0.344136 (Cb - 128)  - 0.714136(Cr - 128)
//   B = Y + 1.772   (Cb - 128)
constexpr int kFixedOne = 1 << 16;
constexpr int kCrToR = static_cast<int>(1.402 * kFixedOne + 0.5);
constexpr int kCbToG = static_cast<int>(0.344136 * kFixedOne + 0.5);
constexpr int kCrToG = static_cast<int>(0.714136 * kFixedOne + 0.5);
constexpr int kCbToB = static_cast<int>(1.772 * kFixedOne + 0.5);

std::uint32_t ClampChannel(int value) {
  return static_cast<std::uint32_t>(std::clamp(value, 0, 255));
}

// One output axis's mapping back into a subsampled plane: the two samples to
// blend and how much of the second to take, in 1/256ths.
struct AxisMap {
  std::vector<int> low;
  std::vector<int> high;
  std::vector<int> weight;
};

// Triangle filtering, which is what a chroma plane is entitled to: the sample
// at index i covers the interval [i, i+1) of its own grid, so the output pixel
// at x wants the plane at (x + 0.5) * numerator / denominator - 0.5, and the
// two samples either side of that get linear weights.
//
// Nearest-neighbour is conforming and four lines shorter, and it costs a mean
// of four levels per channel against libjpeg on a 4:2:0 photograph — measured,
// in tests/JpegDecoderTests.cpp, before this replaced it. Four levels is a
// visible chroma staircase along every diagonal edge, and it is also loose
// enough to hide a chroma plane that is genuinely off by a row, which is the
// bug this decoder would otherwise have no way to catch.
AxisMap BuildAxis(int size, int numerator, int denominator, int plane_size) {
  AxisMap map;
  map.low.resize(static_cast<std::size_t>(size));
  map.high.resize(static_cast<std::size_t>(size));
  map.weight.resize(static_cast<std::size_t>(size));
  for (int i = 0; i < size; ++i) {
    const std::int64_t position =
        (128 * static_cast<std::int64_t>(2 * i + 1) * numerator) / denominator - 128;
    // Floor division, which is not what / does for a negative numerator, and
    // the first output pixel of an upsampled plane is exactly the case where
    // the position is negative.
    std::int64_t index = position >> 8;
    const auto fraction = static_cast<int>(position - (index << 8));
    const auto slot = static_cast<std::size_t>(i);
    map.low[slot] = static_cast<int>(std::clamp<std::int64_t>(index, 0, plane_size - 1));
    map.high[slot] = static_cast<int>(std::clamp<std::int64_t>(index + 1, 0, plane_size - 1));
    map.weight[slot] = fraction;
  }
  return map;
}

}  // namespace

std::vector<std::uint8_t> RenderJpegPlane(const JpegPlane& plane) {
  std::vector<std::uint8_t> samples;
  if (plane.blocks_per_line <= 0 || plane.blocks_per_column <= 0 ||
      plane.quantization == nullptr) {
    return samples;
  }
  const auto stride = static_cast<std::size_t>(plane.blocks_per_line) * 8u;
  const auto rows = static_cast<std::size_t>(plane.blocks_per_column) * 8u;
  const std::size_t expected =
      static_cast<std::size_t>(plane.blocks_per_line) *
      static_cast<std::size_t>(plane.blocks_per_column) * 64u;
  if (plane.coefficients.size() != expected) {
    return samples;
  }
  samples.assign(stride * rows, 0u);

  for (int row = 0; row < plane.blocks_per_column; ++row) {
    for (int column = 0; column < plane.blocks_per_line; ++column) {
      const std::size_t block =
          (static_cast<std::size_t>(row) * static_cast<std::size_t>(plane.blocks_per_line) +
           static_cast<std::size_t>(column)) *
          64u;
      std::uint8_t* out = samples.data() + static_cast<std::size_t>(row) * 8u * stride +
                          static_cast<std::size_t>(column) * 8u;
      InverseTransformBlock(plane.coefficients.data() + block, *plane.quantization, out, stride);
    }
  }
  return samples;
}

bool ComposeJpegPixels(const std::array<JpegSamplePlane, 3>& planes, int count, int width,
                       int height, int max_h, int max_v, bool rgb,
                       std::vector<std::uint32_t>& out) {
  if (width <= 0 || height <= 0 || max_h <= 0 || max_v <= 0 || (count != 1 && count != 3)) {
    return false;
  }
  const auto total = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (total > kMaxImagePixels) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    const JpegSamplePlane& plane = planes[static_cast<std::size_t>(i)];
    // The last read this function makes is at (height - 1, width - 1), so this
    // is the check that makes every one of them in bounds. It is stated here,
    // once, rather than trusted from the caller, because the caller computed
    // these numbers from the file.
    if (plane.width <= 0 || plane.height <= 0 || plane.h <= 0 || plane.v <= 0 ||
        plane.h > max_h || plane.v > max_v ||
        static_cast<std::size_t>(plane.width) > plane.stride ||
        static_cast<std::uint64_t>(plane.height) * plane.stride > plane.samples.size()) {
      return false;
    }
  }
  out.assign(static_cast<std::size_t>(total), 0u);

  std::array<AxisMap, 3> horizontal;
  std::array<AxisMap, 3> vertical;
  for (int i = 0; i < count; ++i) {
    const JpegSamplePlane& plane = planes[static_cast<std::size_t>(i)];
    horizontal[static_cast<std::size_t>(i)] = BuildAxis(width, plane.h, max_h, plane.width);
    vertical[static_cast<std::size_t>(i)] = BuildAxis(height, plane.v, max_v, plane.height);
  }

  for (int y = 0; y < height; ++y) {
    std::uint32_t* line = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    for (int x = 0; x < width; ++x) {
      std::array<int, 3> sample{};
      for (int i = 0; i < count; ++i) {
        const JpegSamplePlane& plane = planes[static_cast<std::size_t>(i)];
        const AxisMap& across = horizontal[static_cast<std::size_t>(i)];
        const AxisMap& down = vertical[static_cast<std::size_t>(i)];
        const auto column = static_cast<std::size_t>(x);
        const auto row = static_cast<std::size_t>(y);
        const std::uint8_t* upper =
            plane.samples.data() + static_cast<std::size_t>(down.low[row]) * plane.stride;
        const std::uint8_t* lower =
            plane.samples.data() + static_cast<std::size_t>(down.high[row]) * plane.stride;
        const int left = across.low[column];
        const int right = across.high[column];
        const int wx = across.weight[column];
        const int wy = down.weight[row];
        const int top = (upper[left] * (256 - wx) + upper[right] * wx + 128) >> 8;
        const int bottom = (lower[left] * (256 - wx) + lower[right] * wx + 128) >> 8;
        sample[static_cast<std::size_t>(i)] = (top * (256 - wy) + bottom * wy + 128) >> 8;
      }
      std::uint32_t r = 0;
      std::uint32_t g = 0;
      std::uint32_t b = 0;
      if (count == 1) {
        r = g = b = static_cast<std::uint32_t>(sample[0]);
      } else if (rgb) {
        r = static_cast<std::uint32_t>(sample[0]);
        g = static_cast<std::uint32_t>(sample[1]);
        b = static_cast<std::uint32_t>(sample[2]);
      } else {
        const int luma = sample[0];
        const int cb = sample[1] - 128;
        const int cr = sample[2] - 128;
        r = ClampChannel(luma + ((kCrToR * cr + (kFixedOne >> 1)) >> 16));
        g = ClampChannel(luma - ((kCbToG * cb + kCrToG * cr + (kFixedOne >> 1)) >> 16));
        b = ClampChannel(luma + ((kCbToB * cb + (kFixedOne >> 1)) >> 16));
      }
      line[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }
  return true;
}

}  // namespace microbrowser::gfx

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace microbrowser::decoder_tool {

inline std::uint8_t ClampU8(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

inline void Yuv420ToRgba(std::span<const std::uint8_t> y_plane, std::span<const std::uint8_t> u_plane,
                         std::span<const std::uint8_t> v_plane, int y_stride, int uv_stride,
                         std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& rgba) {
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  rgba.resize(pixel_count * 4u);
  for (std::uint32_t row = 0; row < height; ++row) {
    for (std::uint32_t col = 0; col < width; ++col) {
      const int y_value = static_cast<int>(y_plane[static_cast<std::size_t>(row) *
                                                         static_cast<std::size_t>(y_stride) +
                                                     col]);
      const int u_value =
          static_cast<int>(u_plane[(row / 2) * static_cast<std::uint32_t>(uv_stride) + (col / 2)]) -
          128;
      const int v_value =
          static_cast<int>(v_plane[(row / 2) * static_cast<std::uint32_t>(uv_stride) + (col / 2)]) -
          128;
      const int red = static_cast<int>(y_value + (359 * v_value) / 256);
      const int green = static_cast<int>(y_value - (88 * u_value + 183 * v_value) / 256);
      const int blue = static_cast<int>(y_value + (454 * u_value) / 256);
      const std::size_t out = (static_cast<std::size_t>(row) * width + col) * 4u;
      rgba[out] = ClampU8(red);
      rgba[out + 1] = ClampU8(green);
      rgba[out + 2] = ClampU8(blue);
      rgba[out + 3] = 255u;
    }
  }
}

inline void FloatPlanarToS16Interleaved(std::span<const float*> planes, std::uint32_t channels,
                                        std::uint32_t sample_count,
                                        std::vector<std::uint8_t>& pcm) {
  pcm.resize(static_cast<std::size_t>(channels) * static_cast<std::size_t>(sample_count) * 2u);
  for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const float value = planes[channel][sample];
      const float clamped = std::clamp(value, -1.0f, 1.0f);
      const std::int16_t sample_s16 =
          static_cast<std::int16_t>(clamped < 0.0f ? clamped * 32768.0f : clamped * 32767.0f);
      const std::size_t out =
          (static_cast<std::size_t>(sample) * channels + channel) * 2u;
      pcm[out] = static_cast<std::uint8_t>(sample_s16 & 0xFF);
      pcm[out + 1] = static_cast<std::uint8_t>((sample_s16 >> 8) & 0xFF);
    }
  }
}

inline void S16InterleavedToBytes(std::span<const std::int16_t> samples, std::vector<std::uint8_t>& pcm) {
  pcm.resize(samples.size() * 2u);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const std::int16_t sample = samples[i];
    pcm[i * 2u] = static_cast<std::uint8_t>(sample & 0xFF);
    pcm[i * 2u + 1u] = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
  }
}

}  // namespace microbrowser::decoder_tool

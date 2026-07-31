#pragma once

#include <cstdint>

namespace microbrowser::gfx {

// One pixel, stored as 0xAARRGGBB in a native-endian uint32.
//
// Non-premultiplied. CSS colors arrive non-premultiplied, images decode
// non-premultiplied, and premultiplying on store loses precision on low-alpha
// colors that later get composited more than once. Premultiplication happens
// inside the blend, where it is a temporary.
//
// The layout matches SDL_PIXELFORMAT_ARGB8888 on little-endian, so a Canvas
// scanline uploads to a texture with no per-pixel swizzle. That is the only
// reason this is ARGB rather than RGBA; nothing else in gfx depends on it.
struct Color {
  std::uint32_t argb = 0;

  static constexpr Color Rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return Rgba(r, g, b, 0xFF);
  }

  static constexpr Color Rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return Color{(static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
                 (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b)};
  }

  static constexpr Color Transparent() { return Color{0}; }

  constexpr std::uint8_t Alpha() const { return static_cast<std::uint8_t>(argb >> 24); }
  constexpr std::uint8_t Red() const { return static_cast<std::uint8_t>(argb >> 16); }
  constexpr std::uint8_t Green() const { return static_cast<std::uint8_t>(argb >> 8); }
  constexpr std::uint8_t Blue() const { return static_cast<std::uint8_t>(argb); }

  constexpr bool IsOpaque() const { return Alpha() == 0xFF; }
  constexpr bool IsFullyTransparent() const { return Alpha() == 0; }

  constexpr Color WithAlpha(std::uint8_t a) const {
    return Color{(argb & 0x00FFFFFFu) | (static_cast<std::uint32_t>(a) << 24)};
  }

  friend constexpr bool operator==(Color, Color) = default;
};

// Exact 8-bit "multiply by a/255" with correct rounding, no division.
// (x * a + 128) then folding the high byte back in is the standard identity for
// round(x * a / 255); a plain `>> 8` is off by up to one level and the error
// accumulates visibly over stacked translucent layers.
constexpr std::uint32_t MulDiv255(std::uint32_t x, std::uint32_t a) {
  const std::uint32_t t = x * a + 128;
  return (t + (t >> 8)) >> 8;
}

// Source-over composite of a non-premultiplied `src` onto an opaque-or-not
// `dst`. Returns the ARGB result.
//
// Scalar and obvious on purpose: correctness is pinned by unit tests here, and
// the SIMD span blitters that replace this in the hot paths are then validated
// against it rather than against a second hand-derived formula.
constexpr std::uint32_t BlendSrcOver(std::uint32_t dst, Color src) {
  const std::uint32_t sa = src.Alpha();
  if (sa == 0) {
    return dst;
  }
  if (sa == 0xFF) {
    return src.argb;
  }
  const std::uint32_t ia = 255u - sa;

  const std::uint32_t da = (dst >> 24) & 0xFFu;
  const std::uint32_t dr = (dst >> 16) & 0xFFu;
  const std::uint32_t dg = (dst >> 8) & 0xFFu;
  const std::uint32_t db = dst & 0xFFu;

  const std::uint32_t out_a = sa + MulDiv255(da, ia);
  const std::uint32_t out_r = MulDiv255(src.Red(), sa) + MulDiv255(dr, ia);
  const std::uint32_t out_g = MulDiv255(src.Green(), sa) + MulDiv255(dg, ia);
  const std::uint32_t out_b = MulDiv255(src.Blue(), sa) + MulDiv255(db, ia);

  return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static_assert(sizeof(Color) == 4, "Color is stored per pixel; it must stay one word");

}  // namespace microbrowser::gfx

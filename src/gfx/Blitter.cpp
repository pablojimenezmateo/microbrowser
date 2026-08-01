#include "gfx/Blitter.h"

#include "util/PerformanceCounters.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#define MICROBROWSER_BLITTER_SSE2 1
#endif

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Everything below rests on one identity.
//
// Source-over with a non-premultiplied source is, per channel,
//     out = MulDiv255(src, sa) + MulDiv255(dst, 255 - sa)
// and for alpha,
//     out = sa + MulDiv255(dst_a, 255 - sa).
//
// The source is constant across a span, so `MulDiv255(src, sa)` is constant
// too — one value computed per span rather than per pixel. Writing `sa` itself
// into the alpha byte of that constant makes the alpha channel obey the same
// expression as the color channels, so the per-pixel work is a single uniform
// `constant + MulDiv255(dst, inverse_alpha)` across all four bytes.
//
// It cannot overflow: with sa + ia == 255, the largest result is
// MulDiv255(255, sa) + MulDiv255(255, ia) == sa + ia == 255. That is why the
// vector add below is a plain add rather than a saturating one, and why a
// saturating add would hide a bug here rather than prevent one.
std::uint32_t ConstantTerm(Color source) {
  const std::uint32_t sa = source.Alpha();
  return (sa << 24) | (MulDiv255(source.Red(), sa) << 16) | (MulDiv255(source.Green(), sa) << 8) |
         MulDiv255(source.Blue(), sa);
}

#if defined(MICROBROWSER_BLITTER_SSE2)

// MulDiv255 for eight u16 lanes, bit-identical to the scalar version.
//
// Every intermediate is checked against the 16-bit range: x * ia is at most
// 65025, plus 128 is 65153, plus its own high byte is 65407. All below 65536,
// so the unsigned 16-bit lanes never wrap and no widening to 32 bits is needed
// — which is what makes this four pixels per iteration instead of two.
inline __m128i MulDiv255Lanes(__m128i x, __m128i inverse_alpha) {
  __m128i t = _mm_mullo_epi16(x, inverse_alpha);
  t = _mm_add_epi16(t, _mm_set1_epi16(128));
  t = _mm_add_epi16(t, _mm_srli_epi16(t, 8));
  return _mm_srli_epi16(t, 8);
}

void BlendSpanVector(std::uint32_t* destination, std::size_t length, Color source) {
  const std::uint32_t inverse_alpha = 255u - source.Alpha();
  const __m128i zero = _mm_setzero_si128();
  const __m128i inverse = _mm_set1_epi16(static_cast<short>(inverse_alpha));
  const __m128i constant = _mm_set1_epi32(static_cast<int>(ConstantTerm(source)));

  std::size_t i = 0;
  for (; i + 4 <= length; i += 4) {
    const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + i));
    const __m128i low = MulDiv255Lanes(_mm_unpacklo_epi8(pixels, zero), inverse);
    const __m128i high = MulDiv255Lanes(_mm_unpackhi_epi8(pixels, zero), inverse);
    const __m128i scaled = _mm_packus_epi16(low, high);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i),
                     _mm_add_epi8(scaled, constant));
  }
  BlendSpanSrcOverScalar(destination + i, length - i, source);
}

#endif  // MICROBROWSER_BLITTER_SSE2

}  // namespace

void BlendSpanSrcOverScalar(std::uint32_t* destination, std::size_t length, Color source) {
  for (std::size_t i = 0; i < length; ++i) {
    destination[i] = BlendSrcOver(destination[i], source);
  }
}

void BlendSpanSrcOver(std::uint32_t* destination, std::size_t length, Color source) {
  if (length == 0 || source.IsFullyTransparent()) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::GfxBlendedPixels, static_cast<std::uint64_t>(length));
#if defined(MICROBROWSER_BLITTER_SSE2)
  BlendSpanVector(destination, length, source);
#else
  BlendSpanSrcOverScalar(destination, length, source);
#endif
}

void BlendMaskSrcOver(std::uint32_t* destination, const std::uint8_t* mask, std::size_t length,
                      Color source) {
  const std::uint32_t source_alpha = source.Alpha();
  if (source_alpha == 0) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::GfxMaskPixels, static_cast<std::uint64_t>(length));
  for (std::size_t i = 0; i < length; ++i) {
    const std::uint32_t coverage = mask[i];
    // Skipping zero coverage is not a micro-optimization: a glyph mask is
    // mostly empty, so this branch is taken for the majority of the pixels a
    // page of text touches.
    if (coverage == 0) {
      continue;
    }
    if (coverage == 255 && source_alpha == 255) {
      destination[i] = source.argb;
      continue;
    }
    destination[i] = BlendSrcOver(
        destination[i],
        source.WithAlpha(static_cast<std::uint8_t>(MulDiv255(source_alpha, coverage))));
  }
}

bool BlendSpanIsVectorized() {
#if defined(MICROBROWSER_BLITTER_SSE2)
  return true;
#else
  return false;
#endif
}

}  // namespace microbrowser::gfx

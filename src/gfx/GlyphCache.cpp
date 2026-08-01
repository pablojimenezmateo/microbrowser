#include "gfx/GlyphCache.h"

#include <algorithm>
#include <cmath>

#include "gfx/AffineTransform.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// A glyph larger than this is not cached. It would evict everything else to
// hold one shape that is on screen once, which is the failure mode a byte
// budget alone does not prevent.
constexpr int kMaxCachedGlyphExtent = 512;

std::uint8_t QuantizeSubpixel(float offset) {
  // Only the fraction matters; the integer part is applied when blitting.
  float fraction = offset - std::floor(offset);
  if (!(fraction >= 0.0f) || !(fraction < 1.0f)) {
    fraction = 0.0f;  // a NaN offset positions at zero rather than anywhere
  }
  const auto index = static_cast<int>(fraction * static_cast<float>(kSubpixelPositions));
  return static_cast<std::uint8_t>(std::clamp(index, 0, kSubpixelPositions - 1));
}

}  // namespace

std::size_t GlyphCache::KeyHash::operator()(const Key& key) const {
  // A plain multiply-xor mix. The keys differ in their low bits (glyph ids and
  // subpixel indices are small and dense), so folding the pointer's high bits
  // down is what keeps the buckets from collapsing.
  std::size_t hash = reinterpret_cast<std::uintptr_t>(key.face);
  hash ^= (hash >> 32);
  hash = hash * 0x9E3779B97F4A7C15ull + key.size_fixed;
  hash = hash * 0x9E3779B97F4A7C15ull + key.glyph;
  hash = hash * 0x9E3779B97F4A7C15ull + key.subpixel;
  hash = hash * 0x9E3779B97F4A7C15ull + key.hinting;
  return hash;
}

void GlyphCache::Clear() {
  entries_.clear();
  order_.clear();
  bytes_ = 0;
}

void GlyphCache::SetByteBudget(std::size_t bytes) {
  budget_ = bytes;
  EvictToBudget();
}

void GlyphCache::EvictToBudget() {
  while (bytes_ > budget_ && !order_.empty()) {
    const Key oldest = order_.front();
    order_.pop_front();
    const auto found = entries_.find(oldest);
    if (found != entries_.end()) {
      bytes_ -= std::min(bytes_, found->second.Bytes());
      entries_.erase(found);
      AddPerformanceCounter(PerfCounterId::GfxGlyphCacheEvictions);
    }
  }
}

const GlyphImage* GlyphCache::Acquire(const Font& font, GlyphId glyph, float subpixel_offset) {
  Key key;
  key.face = font.FaceIdentity();
  // 16.16 rather than the float itself: two sizes that compare equal as floats
  // must produce the same key, and a NaN size must not produce a key that never
  // matches itself and so leaks an entry per call.
  const float size = font.PixelSize();
  if (!(size > 0.0f) || size > 4096.0f) {
    return nullptr;
  }
  key.size_fixed = static_cast<std::uint32_t>(size * 65536.0f);
  key.glyph = glyph;
  key.subpixel = QuantizeSubpixel(subpixel_offset);
  key.hinting = static_cast<std::uint8_t>(font.HintingMode());

  const auto found = entries_.find(key);
  if (found != entries_.end()) {
    ++hits_;
    AddPerformanceCounter(PerfCounterId::GfxGlyphCacheHits);
    return found->second.IsEmpty() ? nullptr : &found->second;
  }
  ++misses_;
  AddPerformanceCounter(PerfCounterId::GfxGlyphCacheMisses);

  GlyphImage image;
  if (font.GlyphOutline(glyph, outline_)) {
    const float fraction =
        static_cast<float>(key.subpixel) / static_cast<float>(kSubpixelPositions);
    const FloatRect bounds = outline_.ControlBounds();
    // The mask covers the outline shifted by the sub-pixel fraction, rounded
    // outward. One pixel of slack on the right absorbs the shift.
    const IntRect pixels = EnclosingIntRect(FloatRect{bounds.x + fraction, bounds.y,
                                                      bounds.width + 1.0f, bounds.height});
    if (!pixels.IsEmpty() && pixels.width <= kMaxCachedGlyphExtent &&
        pixels.height <= kMaxCachedGlyphExtent) {
      image.origin = IntPoint{pixels.x, pixels.y};
      image.width = pixels.width;
      image.height = pixels.height;
      image.coverage.assign(
          static_cast<std::size_t>(pixels.width) * static_cast<std::size_t>(pixels.height), 0u);

      // Rasterize with the glyph moved so the mask's top-left is the origin.
      const AffineTransform to_mask = AffineTransform::Translation(
          fraction - static_cast<float>(pixels.x), -static_cast<float>(pixels.y));
      const auto& spans = rasterizer_.Rasterize(outline_, FillRule::NonZero,
                                                IntRect{0, 0, pixels.width, pixels.height},
                                                to_mask);
      for (const CoverageSpan& span : spans) {
        const auto row = static_cast<std::size_t>(span.y) * static_cast<std::size_t>(pixels.width);
        std::fill_n(image.coverage.begin() + static_cast<std::ptrdiff_t>(row + static_cast<std::size_t>(span.x)),
                    static_cast<std::size_t>(span.length), span.coverage);
      }
      AddPerformanceCounter(PerfCounterId::GfxGlyphsRasterized);
    }
  }

  // Glyphs with nothing to draw are cached too, as empty entries. Otherwise
  // every space on the page pays for an outline load that always fails.
  const std::size_t added = image.Bytes();
  const auto inserted = entries_.emplace(key, std::move(image));
  order_.push_back(key);
  bytes_ += added;
  EvictToBudget();

  const auto entry = entries_.find(key);
  if (entry == entries_.end() || entry->second.IsEmpty()) {
    return nullptr;
  }
  (void)inserted;
  return &entry->second;
}

}  // namespace microbrowser::gfx

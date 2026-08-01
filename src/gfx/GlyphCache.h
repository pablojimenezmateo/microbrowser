#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/Rasterizer.h"

namespace microbrowser::gfx {

// A rasterized glyph: an 8-bit coverage mask and where to put it.
//
// A mask rather than a colored bitmap, because the same glyph is drawn in every
// color a page uses and caching it per color would multiply the cache by the
// palette. Colour arrives at blit time, exactly as it does for a path fill.
struct GlyphImage {
  // Top-left corner of the mask relative to the pen position on the baseline.
  // Usually negative in y, because a glyph sits above its baseline.
  IntPoint origin;
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> coverage;  // row-major, width * height

  std::size_t Bytes() const { return coverage.capacity() + sizeof(GlyphImage); }
  bool IsEmpty() const { return width <= 0 || height <= 0; }
};

// Number of horizontal sub-pixel positions a glyph is cached at.
//
// Text is positioned in fractional pixels — that is what stops a line from
// drifting away from its own advances — so a cache keyed only on the glyph
// would return a mask rendered at the wrong fraction and undo it. Four
// positions is the usual compromise: a quarter pixel of positioning error is
// below what 8-bit coverage can express, and it costs four entries per glyph
// rather than the hundreds a continuous key would.
inline constexpr int kSubpixelPositions = 4;

// Caches rasterized glyphs.
//
// This is the difference between text being usable and not: a screenful of body
// text re-outlines and re-rasterizes tens of thousands of glyphs per frame
// otherwise, and every one of them is the same handful of shapes.
class GlyphCache {
 public:
  // Null when the glyph has nothing to draw (a space), which is a normal answer.
  // The returned pointer is invalidated by the next call that inserts.
  const GlyphImage* Acquire(const Font& font, GlyphId glyph, float subpixel_offset);

  void Clear();
  std::size_t Bytes() const { return bytes_; }
  std::size_t Size() const { return entries_.size(); }

  // Cache statistics, for a test to assert on rather than a thing somebody
  // infers from a frame time.
  std::uint64_t Hits() const { return hits_; }
  std::uint64_t Misses() const { return misses_; }

  // Entries are evicted oldest-first once the cache exceeds this. A byte budget
  // rather than an entry count: a 200px heading glyph is four thousand times
  // the size of an 8px one, and counting entries would let a few headings
  // consume more memory than a document's worth of body text.
  void SetByteBudget(std::size_t bytes);
  std::size_t ByteBudget() const { return budget_; }

 private:
  struct Key {
    const void* face = nullptr;
    std::uint32_t size_fixed = 0;  // pixel size in 16.16, so the key is exact
    GlyphId glyph = 0;
    std::uint8_t subpixel = 0;
    std::uint8_t hinting = 0;

    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& key) const;
  };

  void EvictToBudget();

  std::unordered_map<Key, GlyphImage, KeyHash> entries_;
  // Insertion order, for eviction. First-in-first-out rather than
  // least-recently-used: FIFO needs no bookkeeping on the hit path, which is
  // the path that matters, and for glyphs the two orders rarely disagree —
  // a document reuses the same small set continuously.
  std::deque<Key> order_;
  std::size_t bytes_ = 0;
  std::size_t budget_ = 4u * 1024u * 1024u;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  PathRasterizer rasterizer_;
  Path outline_;
};

}  // namespace microbrowser::gfx

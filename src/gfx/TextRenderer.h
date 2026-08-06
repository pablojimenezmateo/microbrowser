#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "gfx/FontCatalog.h"
#include "gfx/Painter.h"
#include "gfx/TextShaper.h"

namespace microbrowser::gfx {

// Draws a run of text: resolves the font, shapes it, and paints the glyphs.
//
// This exists because Painter deliberately refuses to take a string. Shaping is
// the most expensive step in a text stack and the one most easily repeated by
// accident — a DrawText(string) on Painter would re-shape every run on every
// frame, which is the single worst mistake this code can make. Putting the
// entry point here forces the cache to be part of the design rather than an
// optimization someone adds after a profile.
//
// The cache is keyed on the text and the resolved font, and evicts
// least-recently-used. A page that scrolls re-paints the same runs every frame;
// a page that animates text changes them, and the cache must not grow without
// bound because of it.
class TextRenderer {
 public:
  explicit TextRenderer(FontProvider& fonts) : fonts_(&fonts) {}

  // Paints `text` with its baseline starting at `origin`. Does nothing when the
  // request resolves to no font, which is what a page naming a family that is
  // not installed should do — not crash, and not silently pick something.
  void DrawRun(Painter& painter, std::string_view text, const FontRequest& request,
               FloatPoint origin, Color color, bool right_to_left = false);

  // Total advance width of `text`, or 0 when the request resolves to no font.
  // Shares the cache with DrawRun, so measuring during layout and painting
  // afterwards shape each run once between them.
  float MeasureRun(std::string_view text, const FontRequest& request,
                   bool right_to_left = false);

  // Ascent and descent for a request, both positive and measured from the
  // baseline. Zeroed when nothing resolves.
  FontMetrics MetricsFor(const FontRequest& request);

  // Bounds the cache. Counted in runs rather than bytes because a run's cost is
  // its glyph vector and they are all within an order of magnitude of each
  // other.
  void SetCapacity(std::size_t runs);
  std::size_t CachedRuns() const { return entries_.size(); }
  void Clear();

  FontProvider& Fonts() const { return *fonts_; }

 private:
  // Keyed on what the font *is*, not on where it lives. A Font* would couple
  // this cache's correctness to the provider's allocation lifetimes: a
  // destroyed Font whose address is reused by another face would produce a
  // stale hit, and the symptom is one font's text laid out with another's
  // metrics -- with nothing in the cache that looks wrong.
  struct Key {
    std::string text;
    const void* face = nullptr;
    std::uint32_t size_bits = 0;
    Hinting hinting = Hinting::None;
    // Part of the key, because the same text shaped in the two directions is two different glyph
    // vectors -- and a cache that ignored it would answer a measurement with the other one.
    bool right_to_left = false;

    friend bool operator==(const Key&, const Key&) = default;
  };

  static Key KeyFor(std::string_view text, const Font& font, bool right_to_left);
  struct KeyHash {
    std::size_t operator()(const Key& key) const;
  };

  // Null when the request resolves to no font.
  // A stretch of text and the font that can draw it. The unit of both painting and measuring, so
  // that the two answers cannot disagree about where a fallback began.
  struct CoveragePiece {
    std::string_view text;
    Font* font = nullptr;
  };

  // Maximal stretches of `text` each drawable by one font. One piece for ordinary Latin text, which is
  // the path that must stay cheap; more when the run crosses a coverage boundary -- a Japanese word in
  // an English sentence, an emoji, a mathematical symbol.
  std::vector<CoveragePiece> SplitByCoverage(std::string_view text, const FontRequest& request);

  // The shaped-run cache, keyed by text and font. Takes the font rather than choosing one, because
  // choosing is now per character and happens in SplitByCoverage.
  const ShapedRun* LookupWithFont(std::string_view text, Font& font, bool right_to_left);

  FontProvider* fonts_;
  TextShaper shaper_;
  // A list, so an entry's address is stable while it sits in the map and an
  // eviction is a splice rather than a rehash.
  std::list<Key> recent_;
  std::unordered_map<Key, std::pair<ShapedRun, std::list<Key>::iterator>, KeyHash> entries_;
  std::size_t capacity_ = 512;
};

}  // namespace microbrowser::gfx

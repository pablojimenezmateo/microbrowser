#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "gfx/Font.h"
#include "gfx/Geometry.h"

namespace microbrowser::gfx {

// One glyph, placed.
//
// `cluster` is the byte offset into the source UTF-8 that produced this glyph.
// It is not decoration: it is the only way back from a glyph to the text, which
// is what selection, caret placement, and hit testing are all built on. A
// shaper that discards it produces text that cannot be selected.
struct PositionedGlyph {
  GlyphId glyph = 0;
  float x_offset = 0.0f;
  float y_offset = 0.0f;
  float x_advance = 0.0f;
  float y_advance = 0.0f;
  std::uint32_t cluster = 0;

  friend bool operator==(const PositionedGlyph&, const PositionedGlyph&) = default;
};

struct ShapedRun {
  std::vector<PositionedGlyph> glyphs;
  // Sum of the advances. Not the ink extent — a run ending in a space is wider
  // than the pixels it marks, and layout needs the advance.
  float width = 0.0f;
  bool right_to_left = false;

  void Clear() {
    glyphs.clear();
    width = 0.0f;
    right_to_left = false;
  }
};

// Turns text into positioned glyphs, via HarfBuzz.
//
// Shaping is not "look up each character in the cmap". A ligature turns two
// characters into one glyph, an Indic reordering moves a glyph before the
// character that produced it, an Arabic letter picks one of four forms from its
// neighbours, and a mark attaches at a position the font specifies. Every one
// of those is a correctness issue rather than a typographic nicety, and each is
// a specialty. ADR 0001 sanctions HarfBuzz for exactly this.
//
// Reusable, and meant to be: it keeps the HarfBuzz buffer and a one-entry font
// cache, so shaping a paragraph line by line does not rebuild them per line.
class TextShaper {
 public:
  TextShaper();
  ~TextShaper();

  TextShaper(const TextShaper&) = delete;
  TextShaper& operator=(const TextShaper&) = delete;

  // Shapes UTF-8 `text` with `font`. Direction, script and language are guessed
  // from the text itself, which is right for a single run and is not a
  // substitute for the bidi algorithm — that arrives with line layout, and its
  // job is to hand this function runs that are already one direction each.
  const ShapedRun& Shape(const Font& font, std::string_view text);

  const ShapedRun& Run() const { return run_; }

 private:
  void ReleaseFont();

  void* buffer_ = nullptr;      // hb_buffer_t
  void* hb_font_ = nullptr;     // hb_font_t, for the face and size below
  const void* cached_face_ = nullptr;
  float cached_size_ = 0.0f;
  Hinting cached_hinting_ = Hinting::None;
  ShapedRun run_;
};

}  // namespace microbrowser::gfx

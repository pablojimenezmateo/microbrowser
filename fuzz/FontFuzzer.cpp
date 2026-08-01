#include <cstddef>
#include <cstdint>
#include <vector>

#include "gfx/Font.h"
#include "gfx/Path.h"
#include "gfx/TextShaper.h"

// Font files, fed arbitrary bytes.
//
// FreeType does the parsing, so this fuzzes the seam rather than the library:
// the size and glyph indices we hand back to it, the outline decomposition, and
// the shaping path. A page chooses its own fonts, so the bytes are hostile even
// though the parser is not ours.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const microbrowser::gfx::FontLibrary library;
  std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(data),
                               reinterpret_cast<const std::byte*>(data) + size);
  auto face = microbrowser::gfx::FontFace::Load(library, std::move(bytes));
  if (!face.has_value()) {
    return 0;
  }

  microbrowser::gfx::Font font(*face, 17.5f);
  font.Metrics();

  microbrowser::gfx::Path path;
  const std::size_t glyphs = face->GlyphCount() < 64 ? face->GlyphCount() : 64;
  for (std::size_t i = 0; i < glyphs; ++i) {
    const auto glyph = static_cast<microbrowser::gfx::GlyphId>(i);
    font.Advance(glyph);
    font.GlyphOutline(glyph, path);
    font.GlyphBounds(glyph);
  }

  microbrowser::gfx::TextShaper shaper;
  shaper.Shape(font, "Shaping a run through a font nobody validated.");
  return 0;
}

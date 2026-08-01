#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gfx/Geometry.h"
#include "gfx/Path.h"

namespace microbrowser::gfx {

// A glyph index within one face. Not a character: the mapping from text to
// glyphs is shaping's job, and the two are not one-to-one in either direction.
using GlyphId = std::uint16_t;

struct FontMetrics {
  // Both positive, measured from the baseline. FreeType reports descent as a
  // negative number and roughly half of all font code has a sign bug because of
  // it, so the sign is normalized once, here.
  float ascent = 0.0f;
  float descent = 0.0f;
  float line_gap = 0.0f;

  float LineHeight() const { return ascent + descent + line_gap; }

  friend bool operator==(const FontMetrics&, const FontMetrics&) = default;
};

// Whether to run the font's own hinting instructions.
//
// The default is None, which is a decision rather than a shortcut. Hinting
// snaps stems to whole pixels, which fights sub-pixel glyph positioning — the
// thing that keeps a line of text from drifting — and its output changes
// between FreeType versions, which would make every text reference test a
// FreeType version test. Modern renderers on high-DPI displays have moved the
// same way. Normal remains available for the day someone measures small-text
// legibility on a 96 DPI panel and decides otherwise.
enum class Hinting : std::uint8_t { None, Normal };

// What text asks for, before anything has decided which file answers.
//
// A request rather than a Font, because the thing that knows the text (layout,
// a display list, another process) must not need a live FreeType face to
// describe it. Resolution happens once, at paint time, through a FontProvider.
// This is also what lets a display list carrying text cross a process boundary:
// it names a family, not a handle.
struct FontRequest {
  // Empty means "whatever the provider's default is". Generic families
  // ("serif", "sans-serif", "monospace") are spelled as themselves.
  std::string family;
  float size = 16.0f;
  // CSS numeric weights: 400 is normal, 700 is bold.
  int weight = 400;
  bool italic = false;

  friend bool operator==(const FontRequest&, const FontRequest&) = default;
};

// Owns the FreeType library handle.
//
// An object rather than a process-wide singleton: the repo bans global service
// locators, and the ownership question ("who outlives whom") has a real answer
// here — every FontFace borrows this, so it must outlive them all.
class FontLibrary {
 public:
  FontLibrary();
  ~FontLibrary();

  FontLibrary(const FontLibrary&) = delete;
  FontLibrary& operator=(const FontLibrary&) = delete;
  FontLibrary(FontLibrary&& other) noexcept;
  FontLibrary& operator=(FontLibrary&& other) noexcept;

  bool IsValid() const { return library_ != nullptr; }

 private:
  friend class FontFace;
  // FT_Library, type-erased so that no FreeType type appears in a public
  // header. ADR 0001 requires the dependency to sit behind a seam, and this is
  // where the seam is.
  void* library_ = nullptr;
};

// One parsed font file.
//
// Holds its own copy of the bytes because FreeType does not: FT_New_Memory_Face
// keeps a pointer into the buffer for the life of the face, so a caller passing
// a temporary would be handing FreeType a dangling pointer that only misbehaves
// once a glyph is loaded.
class FontFace {
 public:
  FontFace() = default;
  ~FontFace();

  FontFace(const FontFace&) = delete;
  FontFace& operator=(const FontFace&) = delete;
  FontFace(FontFace&& other) noexcept;
  FontFace& operator=(FontFace&& other) noexcept;

  // Nullopt for anything FreeType will not parse. Font files are attacker
  // controlled — a page picks the font — so this is a routine outcome, not an
  // error path.
  static std::optional<FontFace> Load(const FontLibrary& library, std::vector<std::byte> bytes,
                                      int face_index = 0);

  bool IsValid() const { return face_ != nullptr; }
  int UnitsPerEm() const;
  std::size_t GlyphCount() const;
  std::string FamilyName() const;

  // What the face says about itself, for building a font database without a
  // filename heuristic. A filename says "Bold" only by convention, and the
  // convention is not universal; OS/2 says it in a number.
  int Weight() const;
  bool IsItalic() const;

  // Zero (.notdef) for an unmapped codepoint, which is what the font itself
  // says and what shaping expects to see.
  GlyphId GlyphForCodepoint(char32_t codepoint) const;

  std::span<const std::byte> Bytes() const { return bytes_; }

 private:
  friend class Font;
  // Shaping needs the native face to build an hb_font_t from. A friend rather
  // than a public accessor: the whole point of the type erasure is that a
  // FreeType handle is not part of this module's surface.
  friend class TextShaper;
  void* face_ = nullptr;  // FT_Face
  std::vector<std::byte> bytes_;
};

// A face at a size. Cheap to construct; holds no glyph state of its own.
//
// Not thread-safe, and neither is the face it points at: a FreeType face
// carries the active size internally, so two Fonts sharing a face must not be
// used concurrently. Single-threaded painting is the only mode that exists
// today, and when it stops being so this needs FT_New_Size, not a mutex.
class Font {
 public:
  Font(FontFace& face, float pixel_size, Hinting hinting = Hinting::None);

  float PixelSize() const { return pixel_size_; }
  Hinting HintingMode() const { return hinting_; }

  // Identity of the face this font draws from, for use as a cache key. An
  // opaque token rather than a handle: nothing may be done with it but compared,
  // and it is a FontFace address rather than anything FreeType owns.
  const void* FaceIdentity() const { return face_; }
  FontMetrics Metrics() const;

  // Horizontal advance in device pixels. Fractional: rounding advances to whole
  // pixels is what makes long lines drift away from where layout put them.
  float Advance(GlyphId glyph) const;

  // Fills `out` with the glyph outline in device pixels, y down, relative to
  // the glyph origin on the baseline. False when the glyph has no outline,
  // which includes a space and is not a failure.
  bool GlyphOutline(GlyphId glyph, Path& out) const;

  // Device-pixel bounding box of the outline, relative to the glyph origin.
  FloatRect GlyphBounds(GlyphId glyph) const;

 private:
  friend class TextShaper;
  bool Activate() const;

  FontFace* face_;
  float pixel_size_;
  Hinting hinting_;
};

}  // namespace microbrowser::gfx

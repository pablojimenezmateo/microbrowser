#include "gfx/TextShaper.h"

#include <cstddef>

#include <hb.h>
#include <hb-ft.h>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// HarfBuzz reports positions in 26.6 fixed point when the font came from
// hb_ft_font_create, because it inherits FreeType's scaled units. The same
// conversion as Font.cpp, and it has to stay the same one — a mismatch here
// would make shaped advances disagree with rendered outlines by a factor of 64,
// which looks like a font that is either invisible or enormous.
constexpr float kFixed26_6 = 1.0f / 64.0f;

}  // namespace

TextShaper::TextShaper() {
  buffer_ = hb_buffer_create();
}

TextShaper::~TextShaper() {
  ReleaseFont();
  if (buffer_ != nullptr) {
    hb_buffer_destroy(static_cast<hb_buffer_t*>(buffer_));
  }
}

void TextShaper::ReleaseFont() {
  if (hb_font_ != nullptr) {
    hb_font_destroy(static_cast<hb_font_t*>(hb_font_));
    hb_font_ = nullptr;
  }
  cached_face_ = nullptr;
  cached_size_ = 0.0f;
}

const ShapedRun& TextShaper::Shape(const Font& font, std::string_view text,
                                   bool right_to_left) {
  run_.Clear();
  if (text.empty() || buffer_ == nullptr) {
    return run_;
  }

  FontFace* face = font.face_;
  if (face == nullptr || !face->IsValid()) {
    return run_;
  }
  // Activating the face applies the size to the FT_Face, which is what
  // hb_ft_font_create reads. Shaping a size the font cannot be set to would
  // otherwise silently shape at whatever size was set last.
  if (!font.Activate()) {
    return run_;
  }

  // One-entry cache. A paragraph is many runs at one size, so this is the case
  // worth having; a run that switches font mid-paragraph pays a rebuild, which
  // is still cheaper than keying a map on a pointer and a float.
  const bool cache_hit = hb_font_ != nullptr && cached_face_ == face &&
                         cached_size_ == font.PixelSize() && cached_hinting_ == font.hinting_;
  if (!cache_hit) {
    ReleaseFont();
    hb_font_ = hb_ft_font_create_referenced(static_cast<FT_Face>(face->face_));
    if (hb_font_ == nullptr) {
      return run_;
    }
    // The load flags must match the ones the outline path uses, or shaping
    // measures a differently-hinted glyph from the one that gets drawn and the
    // text drifts from its own advances.
    hb_ft_font_set_load_flags(static_cast<hb_font_t*>(hb_font_),
                              font.hinting_ == Hinting::Normal
                                  ? (FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL)
                                  : (FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING));
    cached_face_ = face;
    cached_size_ = font.PixelSize();
    cached_hinting_ = font.hinting_;
  } else {
    // The face is shared and its size may have been changed by another Font
    // since the last shape; tell HarfBuzz to re-read it.
    hb_ft_font_changed(static_cast<hb_font_t*>(hb_font_));
  }

  auto* buffer = static_cast<hb_buffer_t*>(buffer_);
  hb_buffer_clear_contents(buffer);
  // Length as int: HarfBuzz takes a signed length, and -1 means
  // null-terminated. A string_view is not, so the length is always explicit.
  hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                     static_cast<int>(text.size()));
  hb_buffer_guess_segment_properties(buffer);
  // After the guess, because the guess also sets script and language -- which are still worth having.
  // Only the direction is overridden, and only ever to what bidi already resolved.
  hb_buffer_set_direction(buffer, right_to_left ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
  hb_shape(static_cast<hb_font_t*>(hb_font_), buffer, nullptr, 0);

  unsigned int count = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
  if (infos == nullptr || positions == nullptr) {
    return run_;
  }

  run_.right_to_left =
      HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(buffer)) != 0;
  run_.glyphs.reserve(count);
  for (unsigned int i = 0; i < count; ++i) {
    PositionedGlyph glyph;
    // hb_codepoint_t is 32-bit here and holds a glyph index after shaping; a
    // face with more than 65535 glyphs would wrap, so it saturates to .notdef.
    glyph.glyph = infos[i].codepoint > 0xFFFFu ? GlyphId{0}
                                               : static_cast<GlyphId>(infos[i].codepoint);
    glyph.cluster = infos[i].cluster;
    glyph.x_offset = static_cast<float>(positions[i].x_offset) * kFixed26_6;
    // HarfBuzz y grows upward, ours grows down the screen. Same flip as the
    // outline path, for the same reason.
    glyph.y_offset = -static_cast<float>(positions[i].y_offset) * kFixed26_6;
    glyph.x_advance = static_cast<float>(positions[i].x_advance) * kFixed26_6;
    glyph.y_advance = -static_cast<float>(positions[i].y_advance) * kFixed26_6;
    run_.width += glyph.x_advance;
    run_.glyphs.push_back(glyph);
  }

  AddPerformanceCounter(PerfCounterId::GfxTextShapes);
  AddPerformanceCounter(PerfCounterId::GfxShapedGlyphs, run_.glyphs.size());
  return run_;
}

}  // namespace microbrowser::gfx

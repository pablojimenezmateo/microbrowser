#include "gfx/Font.h"

#include <algorithm>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

FT_Library AsLibrary(void* handle) {
  return static_cast<FT_Library>(handle);
}

FT_Face AsFace(void* handle) {
  return static_cast<FT_Face>(handle);
}

// FreeType reports scaled outline coordinates in 26.6 fixed point, and metrics
// in the same. One conversion, named, rather than a `/ 64.0f` scattered around.
constexpr float kFixed26_6 = 1.0f / 64.0f;

float FromFixed26_6(FT_Pos value) {
  return static_cast<float>(value) * kFixed26_6;
}

// Outline decomposition. FreeType's y axis points up from the baseline and ours
// points down the screen, so every y is negated on the way through — once,
// here, rather than by each caller.
struct OutlineSink {
  Path* path = nullptr;
};

FloatPoint ToDevice(const FT_Vector* v) {
  return FloatPoint{FromFixed26_6(v->x), -FromFixed26_6(v->y)};
}

int MoveTo(const FT_Vector* to, void* user) {
  Path& path = *static_cast<OutlineSink*>(user)->path;
  // A TrueType contour is implicitly closed; FreeType emits a move for each new
  // one without ever emitting a close, so the close belongs here.
  path.Close();
  path.MoveTo(ToDevice(to));
  return 0;
}

int LineTo(const FT_Vector* to, void* user) {
  static_cast<OutlineSink*>(user)->path->LineTo(ToDevice(to));
  return 0;
}

int ConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
  static_cast<OutlineSink*>(user)->path->QuadTo(ToDevice(control), ToDevice(to));
  return 0;
}

int CubicTo(const FT_Vector* first, const FT_Vector* second, const FT_Vector* to, void* user) {
  static_cast<OutlineSink*>(user)->path->CubicTo(ToDevice(first), ToDevice(second), ToDevice(to));
  return 0;
}

FT_Int32 LoadFlagsFor(Hinting hinting) {
  // NO_BITMAP always: a bitmap strike would come back as pixels rather than an
  // outline, and this path exists to feed our own rasterizer.
  const FT_Int32 base = FT_LOAD_NO_BITMAP;
  if (hinting == Hinting::Normal) {
    return base | FT_LOAD_TARGET_NORMAL;
  }
  return base | FT_LOAD_NO_HINTING;
}

}  // namespace

FontLibrary::FontLibrary() {
  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library) == 0) {
    library_ = library;
  }
}

FontLibrary::~FontLibrary() {
  if (library_ != nullptr) {
    FT_Done_FreeType(AsLibrary(library_));
  }
}

FontLibrary::FontLibrary(FontLibrary&& other) noexcept : library_(other.library_) {
  other.library_ = nullptr;
}

FontLibrary& FontLibrary::operator=(FontLibrary&& other) noexcept {
  if (this != &other) {
    if (library_ != nullptr) {
      FT_Done_FreeType(AsLibrary(library_));
    }
    library_ = other.library_;
    other.library_ = nullptr;
  }
  return *this;
}

FontFace::~FontFace() {
  if (face_ != nullptr) {
    FT_Done_Face(AsFace(face_));
  }
}

FontFace::FontFace(FontFace&& other) noexcept
    : face_(other.face_), bytes_(std::move(other.bytes_)) {
  other.face_ = nullptr;
}

FontFace& FontFace::operator=(FontFace&& other) noexcept {
  if (this != &other) {
    if (face_ != nullptr) {
      FT_Done_Face(AsFace(face_));
    }
    face_ = other.face_;
    bytes_ = std::move(other.bytes_);
    other.face_ = nullptr;
  }
  return *this;
}

std::optional<FontFace> FontFace::Load(const FontLibrary& library, std::vector<std::byte> bytes,
                                       int face_index) {
  if (!library.IsValid() || bytes.empty()) {
    return std::nullopt;
  }

  FontFace result;
  result.bytes_ = std::move(bytes);

  FT_Face face = nullptr;
  const FT_Error error = FT_New_Memory_Face(
      AsLibrary(library.library_), reinterpret_cast<const FT_Byte*>(result.bytes_.data()),
      static_cast<FT_Long>(result.bytes_.size()), static_cast<FT_Long>(face_index), &face);
  if (error != 0 || face == nullptr) {
    AddPerformanceCounter(PerfCounterId::GfxFontLoadFailures);
    return std::nullopt;
  }
  // Scalable outlines only. A bitmap-only face would load and then produce no
  // outline for any glyph, which would surface as invisible text rather than as
  // a font that could not be used.
  if (FT_IS_SCALABLE(face) == 0) {
    FT_Done_Face(face);
    AddPerformanceCounter(PerfCounterId::GfxFontLoadFailures);
    return std::nullopt;
  }

  result.face_ = face;
  AddPerformanceCounter(PerfCounterId::GfxFontsLoaded);
  return result;
}

int FontFace::UnitsPerEm() const {
  return face_ == nullptr ? 0 : static_cast<int>(AsFace(face_)->units_per_EM);
}

std::size_t FontFace::GlyphCount() const {
  return face_ == nullptr ? 0 : static_cast<std::size_t>(AsFace(face_)->num_glyphs);
}

std::string FontFace::FamilyName() const {
  if (face_ == nullptr || AsFace(face_)->family_name == nullptr) {
    return {};
  }
  return std::string(AsFace(face_)->family_name);
}

int FontFace::Weight() const {
  if (face_ == nullptr) {
    return 400;
  }
  // OS/2 is the authority and is present in every font a browser will meet, but
  // it is optional in the format, so the bold style flag is the fallback and
  // 400 is the fallback to that.
  const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(AsFace(face_), FT_SFNT_OS2));
  if (os2 != nullptr && os2->version != 0xFFFF && os2->usWeightClass >= 1 &&
      os2->usWeightClass <= 1000) {
    return static_cast<int>(os2->usWeightClass);
  }
  return (AsFace(face_)->style_flags & FT_STYLE_FLAG_BOLD) != 0 ? 700 : 400;
}

bool FontFace::IsItalic() const {
  if (face_ == nullptr) {
    return false;
  }
  return (AsFace(face_)->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
}

GlyphId FontFace::GlyphForCodepoint(char32_t codepoint) const {
  if (face_ == nullptr) {
    return 0;
  }
  const FT_UInt index = FT_Get_Char_Index(AsFace(face_), static_cast<FT_ULong>(codepoint));
  // FT_UInt is 32-bit and a glyph id is 16; a face claiming more than 65535
  // glyphs would silently wrap here, so it saturates to .notdef instead.
  return index > 0xFFFFu ? GlyphId{0} : static_cast<GlyphId>(index);
}

Font::Font(FontFace& face, float pixel_size, Hinting hinting)
    : face_(&face), pixel_size_(pixel_size), hinting_(hinting) {}

bool Font::Activate() const {
  if (face_ == nullptr || !face_->IsValid()) {
    return false;
  }
  if (!(pixel_size_ > 0.0f) || pixel_size_ > 4096.0f) {
    // A size arriving from CSS can be anything at all, including a NaN. The
    // upper bound is not a style limit, it is the point past which FreeType's
    // 26.6 metrics stop being representable.
    return false;
  }
  // Fractional sizes go through FT_Set_Char_Size, which takes 26.6: a 13.5px
  // font is ordinary once a page sets a percentage line height, and rounding it
  // to 13 would make every such page render at the wrong size.
  const FT_F26Dot6 size = static_cast<FT_F26Dot6>(pixel_size_ * 64.0f);
  return FT_Set_Char_Size(AsFace(face_->face_), size, size, 72, 72) == 0;
}

FontMetrics Font::Metrics() const {
  FontMetrics metrics;
  if (!Activate()) {
    return metrics;
  }
  const FT_Size_Metrics& size = AsFace(face_->face_)->size->metrics;
  metrics.ascent = FromFixed26_6(size.ascender);
  // FreeType's descender is negative (it is a coordinate, not a distance).
  metrics.descent = -FromFixed26_6(size.descender);
  metrics.line_gap =
      std::max(0.0f, FromFixed26_6(size.height) - metrics.ascent - metrics.descent);
  return metrics;
}

float Font::Advance(GlyphId glyph) const {
  if (!Activate()) {
    return 0.0f;
  }
  FT_Face face = AsFace(face_->face_);
  if (FT_Load_Glyph(face, glyph, LoadFlagsFor(hinting_)) != 0) {
    return 0.0f;
  }
  return FromFixed26_6(face->glyph->advance.x);
}

bool Font::GlyphOutline(GlyphId glyph, Path& out) const {
  out.Clear();
  if (!Activate()) {
    return false;
  }
  FT_Face face = AsFace(face_->face_);
  if (FT_Load_Glyph(face, glyph, LoadFlagsFor(hinting_)) != 0) {
    return false;
  }
  if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_points == 0) {
    return false;  // a space, or a glyph with nothing to draw
  }

  AddPerformanceCounter(PerfCounterId::GfxGlyphOutlines);
  FT_Outline_Funcs funcs{};
  funcs.move_to = MoveTo;
  funcs.line_to = LineTo;
  funcs.conic_to = ConicTo;
  funcs.cubic_to = CubicTo;
  funcs.shift = 0;
  funcs.delta = 0;

  OutlineSink sink;
  sink.path = &out;
  if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &sink) != 0) {
    out.Clear();
    return false;
  }
  // The final contour never gets a following move to close it.
  out.Close();
  return !out.IsEmpty();
}

FloatRect Font::GlyphBounds(GlyphId glyph) const {
  if (!Activate()) {
    return FloatRect{};
  }
  FT_Face face = AsFace(face_->face_);
  if (FT_Load_Glyph(face, glyph, LoadFlagsFor(hinting_)) != 0) {
    return FloatRect{};
  }
  FT_BBox box{};
  FT_Outline_Get_CBox(&face->glyph->outline, &box);
  if (box.xMax <= box.xMin || box.yMax <= box.yMin) {
    return FloatRect{};
  }
  // The y flip swaps which edge is the top.
  const float left = FromFixed26_6(box.xMin);
  const float right = FromFixed26_6(box.xMax);
  const float top = -FromFixed26_6(box.yMax);
  const float bottom = -FromFixed26_6(box.yMin);
  return FloatRect{left, top, right - left, bottom - top};
}

}  // namespace microbrowser::gfx

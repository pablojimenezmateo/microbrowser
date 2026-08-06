#include "gfx/TextRenderer.h"

#include <bit>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Mixed rather than xored: a face pointer's low bits are alignment zeros, and
// xor would let two runs of the same text in different fonts land in the same
// bucket far more often than the hash's width suggests.
void MixInto(std::size_t& hash, std::size_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

}  // namespace

std::size_t TextRenderer::KeyHash::operator()(const Key& key) const {
  std::size_t hash = std::hash<std::string>{}(key.text);
  MixInto(hash, std::hash<const void*>{}(key.face));
  MixInto(hash, key.size_bits);
  MixInto(hash, static_cast<std::size_t>(key.hinting));
  return hash;
}

TextRenderer::Key TextRenderer::KeyFor(std::string_view text, const Font& font) {
  return Key{std::string(text), font.FaceIdentity(), std::bit_cast<std::uint32_t>(font.PixelSize()),
             font.HintingMode()};
}

void TextRenderer::SetCapacity(std::size_t runs) {
  capacity_ = runs;
  while (entries_.size() > capacity_ && !recent_.empty()) {
    entries_.erase(recent_.back());
    recent_.pop_back();
  }
}

void TextRenderer::Clear() {
  entries_.clear();
  recent_.clear();
}

std::vector<TextRenderer::CoveragePiece> TextRenderer::SplitByCoverage(std::string_view text,
                                                                      const FontRequest& request) {
  std::vector<CoveragePiece> pieces;
  std::size_t at = 0;
  std::size_t piece_start = 0;
  Font* current = nullptr;
  std::uint32_t code = 0;
  std::size_t previous = 0;
  while (util::DecodeUtf8(text, at, code)) {
    Font* wanted = fonts_->FontForCodePoint(request, static_cast<char32_t>(code));
    if (current == nullptr) {
      current = wanted;
    } else if (wanted != current) {
      pieces.push_back(CoveragePiece{text.substr(piece_start, previous - piece_start), current});
      piece_start = previous;
      current = wanted;
    }
    previous = at;
  }
  if (piece_start < text.size() || pieces.empty()) {
    pieces.push_back(CoveragePiece{text.substr(piece_start), current});
  }
  if (pieces.size() > 1) {
    AddPerformanceCounter(PerfCounterId::TextRunsSplitByCoverage);
  }
  return pieces;
}

const ShapedRun* TextRenderer::LookupWithFont(std::string_view text, Font& font) {
  Key key = KeyFor(text, font);
  const auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    AddPerformanceCounter(PerfCounterId::ShapedRunCacheHits);
    recent_.splice(recent_.begin(), recent_, existing->second.second);
    return &existing->second.first;
  }

  AddPerformanceCounter(PerfCounterId::ShapedRunCacheMisses);
  const ShapedRun& shaped = shaper_.Shape(font, text);

  if (capacity_ == 0) {
    // Not cacheable, but still shaped. Returning the shaper's own run is safe
    // because the next Shape call is the only thing that invalidates it, and
    // the caller uses it before then.
    return &shaped;
  }
  while (entries_.size() >= capacity_ && !recent_.empty()) {
    entries_.erase(recent_.back());
    recent_.pop_back();
  }
  recent_.push_front(key);
  const auto inserted = entries_.emplace(std::move(key), std::make_pair(shaped, recent_.begin()));
  return &inserted.first->second.first;
}

void TextRenderer::DrawRun(Painter& painter, std::string_view text, const FontRequest& request,
                           FloatPoint origin, Color color) {
  if (text.empty() || color.IsFullyTransparent()) {
    return;
  }
  // Split at coverage boundaries and draw each piece with the font that can draw it.
  //
  // **This is what makes CJK render rather than showing boxes.** A page asks for `sans-serif`, gets a
  // face with no CJK glyphs, and every ideograph is a `.notdef` box -- on a machine with 31 Japanese
  // faces installed. One font per *element* is what the author asked for; one font per *character* is
  // what the machine can do, and every browser resolves it this way.
  //
  // The common case -- a whole run in one font -- takes one pass and one shape, which is what the
  // `pieces.size() == 1` path below preserves: splitting must not cost anything on Latin text.
  float pen = origin.x;
  for (const CoveragePiece& piece : SplitByCoverage(text, request)) {
    if (piece.font == nullptr) {
      continue;
    }
    Font* font = piece.font;
    const ShapedRun* run = LookupWithFont(piece.text, *font);
    if (run == nullptr) {
      continue;
    }
    painter.DrawGlyphs(*font, *run, FloatPoint{pen, origin.y}, color);
    pen += run->width;
    AddPerformanceCounter(PerfCounterId::TextRunsPainted);
  }
}

float TextRenderer::MeasureRun(std::string_view text, const FontRequest& request) {
  if (text.empty()) {
    return 0.0f;
  }
  // Measured the same way it is drawn, piece by piece. A measurement taken with one font and a paint
  // done with several is a line that overflows by however much the fallback face differs -- and the
  // two answers coming from one function is the only way they cannot drift.
  float width = 0.0f;
  for (const CoveragePiece& piece : SplitByCoverage(text, request)) {
    if (piece.font == nullptr) {
      continue;
    }
    if (const ShapedRun* run = LookupWithFont(piece.text, *piece.font)) {
      width += run->width;
    }
  }
  return width;
}

FontMetrics TextRenderer::MetricsFor(const FontRequest& request) {
  const Font* font = fonts_->FontFor(request);
  return font == nullptr ? FontMetrics{} : font->Metrics();
}

}  // namespace microbrowser::gfx

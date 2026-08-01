#include "gfx/TextRenderer.h"

#include <bit>
#include <utility>

#include "util/PerformanceCounters.h"

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

const ShapedRun* TextRenderer::Lookup(std::string_view text, const FontRequest& request,
                                      Font*& font_out) {
  font_out = fonts_->FontFor(request);
  if (font_out == nullptr) {
    return nullptr;
  }

  Key key = KeyFor(text, *font_out);
  const auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    AddPerformanceCounter(PerfCounterId::ShapedRunCacheHits);
    recent_.splice(recent_.begin(), recent_, existing->second.second);
    return &existing->second.first;
  }

  AddPerformanceCounter(PerfCounterId::ShapedRunCacheMisses);
  const ShapedRun& shaped = shaper_.Shape(*font_out, text);

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
  Font* font = nullptr;
  const ShapedRun* run = Lookup(text, request, font);
  if (run == nullptr) {
    return;
  }
  painter.DrawGlyphs(*font, *run, origin, color);
  AddPerformanceCounter(PerfCounterId::TextRunsPainted);
}

float TextRenderer::MeasureRun(std::string_view text, const FontRequest& request) {
  if (text.empty()) {
    return 0.0f;
  }
  Font* font = nullptr;
  const ShapedRun* run = Lookup(text, request, font);
  return run == nullptr ? 0.0f : run->width;
}

FontMetrics TextRenderer::MetricsFor(const FontRequest& request) {
  const Font* font = fonts_->FontFor(request);
  return font == nullptr ? FontMetrics{} : font->Metrics();
}

}  // namespace microbrowser::gfx

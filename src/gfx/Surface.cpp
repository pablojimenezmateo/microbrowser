#include "gfx/Surface.h"

#include <cstring>
#include <utility>

namespace microbrowser::gfx {

Surface::Surface(SurfaceId id, IntSize size) : id_(id), size_(size) {}

bool Surface::Update(std::span<const std::uint32_t> pixels) {
  // The product is computed in 64 bits before it is compared, because both
  // edges are bounded at construction but their product still overflows an int
  // at a quarter of that bound.
  const std::uint64_t expected =
      static_cast<std::uint64_t>(size_.width) * static_cast<std::uint64_t>(size_.height);
  if (expected == 0 || pixels.size() != expected) {
    return false;
  }
  pixels_.assign(pixels.begin(), pixels.end());
  ++generation_;
  return true;
}

const std::uint32_t* Surface::Row(int y) const {
  if (y < 0 || y >= size_.height || pixels_.empty()) {
    return nullptr;
  }
  return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.width);
}

Surface* SurfaceRegistry::Create(IntSize size) {
  if (size.width <= 0 || size.height <= 0 || size.width > kMaxSurfaceEdge ||
      size.height > kMaxSurfaceEdge) {
    return nullptr;
  }
  if (surfaces_.size() >= kMaxSurfaces) {
    return nullptr;
  }
  const SurfaceId id = next_id_++;
  auto surface = std::make_unique<Surface>(id, size);
  Surface* raw = surface.get();
  surfaces_.emplace(id, std::move(surface));
  return raw;
}

Surface* SurfaceRegistry::Find(SurfaceId id) {
  const auto found = surfaces_.find(id);
  return found == surfaces_.end() ? nullptr : found->second.get();
}

const Surface* SurfaceRegistry::Find(SurfaceId id) const {
  const auto found = surfaces_.find(id);
  return found == surfaces_.end() ? nullptr : found->second.get();
}

bool SurfaceRegistry::Destroy(SurfaceId id) { return surfaces_.erase(id) > 0; }

}  // namespace microbrowser::gfx

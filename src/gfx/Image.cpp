#include "gfx/Image.h"

namespace microbrowser::gfx {

const std::uint32_t* Image::Row(int y) const {
  if (y < 0 || y >= height_ || !IsValid()) {
    return nullptr;
  }
  return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
}

bool Image::Adopt(int width, int height, std::vector<std::uint32_t> pixels) {
  width_ = 0;
  height_ = 0;
  opaque_ = true;
  pixels_.clear();

  if (width <= 0 || height <= 0) {
    return false;
  }
  // 64-bit before the comparison. `width * height` in int is the canonical
  // image-decoder heap overflow, and it does not stop being one because this
  // function only checks a size.
  const auto expected =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (expected != pixels.size()) {
    return false;
  }

  for (const std::uint32_t pixel : pixels) {
    if ((pixel >> 24) != 0xFFu) {
      opaque_ = false;
      break;
    }
  }
  width_ = width;
  height_ = height;
  pixels_ = std::move(pixels);
  return true;
}

}  // namespace microbrowser::gfx

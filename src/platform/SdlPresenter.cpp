#include "platform/SdlPresenter.h"

#include <SDL3/SDL.h>

#include <cstdint>

#include "gfx/Canvas.h"
#include "gfx/DirtyRegion.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::platform {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// gfx::Color is 0xAARRGGBB in a native-endian uint32, which is exactly
// SDL_PIXELFORMAT_ARGB8888 on a little-endian host. Asserting it here means a
// big-endian port fails to build rather than rendering with swapped channels.
static_assert(SDL_BYTEORDER == SDL_LIL_ENDIAN,
              "Canvas pixel layout assumes little-endian ARGB8888; see gfx/Color.h");

bool UploadRect(SDL_Texture* texture, const gfx::Canvas& canvas, const gfx::IntRect& rect) {
  const gfx::IntRect clipped = rect.Intersected(canvas.Bounds());
  if (clipped.IsEmpty()) {
    return true;
  }
  const std::uint32_t* row = canvas.Row(clipped.Top());
  if (row == nullptr) {
    return false;
  }
  const SDL_Rect target{clipped.x, clipped.y, clipped.width, clipped.height};
  const void* pixels = row + clipped.Left();
  if (!SDL_UpdateTexture(texture, &target, pixels, static_cast<int>(canvas.StrideBytes()))) {
    return false;
  }
  AddPerformanceCounter(PerfCounterId::FrameTexturePixelsUploaded,
                        static_cast<std::uint64_t>(clipped.Area()));
  return true;
}

}  // namespace

SdlPresenter::~SdlPresenter() {
  Reset();
}

void SdlPresenter::Reset() {
  if (texture_ != nullptr) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  texture_size_ = gfx::IntSize{};
  texture_primed_ = false;
}

bool SdlPresenter::EnsureTexture(SDL_Renderer* renderer, const gfx::IntSize& size) {
  if (texture_ != nullptr && texture_size_ == size) {
    return true;
  }
  Reset();
  if (size.IsEmpty()) {
    return false;
  }

  texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                               size.width, size.height);
  if (texture_ == nullptr) {
    SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
    return false;
  }

  // The texture is the whole window and there is nothing behind it. Leaving the
  // default BLEND mode would composite each present against the previous one,
  // so any pixel with alpha < 255 would darken cumulatively frame over frame.
  // NONE is also the cheaper blit.
  SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
  // The texture is drawn 1:1; nearest costs nothing and cannot resample.
  SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);

  texture_size_ = size;
  return true;
}

bool SdlPresenter::Present(SDL_Renderer* renderer,
                           const gfx::Canvas& canvas,
                           const gfx::DirtyRegion& dirty,
                           bool full_repaint) {
  util::PerformanceTrace::Scope scope("SdlPresenter::Present");

  if (renderer == nullptr || canvas.IsEmpty()) {
    return false;
  }

  const gfx::IntSize size{canvas.Width(), canvas.Height()};
  if (!EnsureTexture(renderer, size)) {
    return false;
  }

  const bool upload_everything = full_repaint || !texture_primed_ || dirty.IsEmpty();
  if (upload_everything) {
    AddPerformanceCounter(PerfCounterId::FramesFullRepaint);
    if (!UploadRect(texture_, canvas, canvas.Bounds())) {
      Reset();
      return false;
    }
  } else {
    AddPerformanceCounter(PerfCounterId::FramesPartialRepaint);
    AddPerformanceCounter(PerfCounterId::FrameDirtyRects,
                          static_cast<std::uint64_t>(dirty.Count()));
    AddPerformanceCounter(PerfCounterId::FrameDirtyPixels,
                          static_cast<std::uint64_t>(dirty.Area()));
    for (const gfx::IntRect& rect : dirty.Rects()) {
      if (!UploadRect(texture_, canvas, rect)) {
        Reset();
        return false;
      }
    }
  }
  texture_primed_ = true;

  // No SDL_RenderClear: the texture covers the entire drawable and is opaque,
  // so clearing first would write every pixel twice.
  if (!SDL_RenderTexture(renderer, texture_, nullptr, nullptr)) {
    SDL_Log("SDL_RenderTexture failed: %s", SDL_GetError());
    Reset();
    return false;
  }
  if (!SDL_RenderPresent(renderer)) {
    SDL_Log("SDL_RenderPresent failed: %s", SDL_GetError());
    Reset();
    return false;
  }

  AddPerformanceCounter(PerfCounterId::FramesPresented);
  return true;
}

}  // namespace microbrowser::platform

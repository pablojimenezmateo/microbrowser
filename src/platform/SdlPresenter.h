#pragma once

#include "gfx/Geometry.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace microbrowser::gfx {
class Canvas;
class DirtyRegion;
}  // namespace microbrowser::gfx

namespace microbrowser::platform {

// Gets a Canvas onto the screen, and nothing else.
//
// One streaming texture the size of the drawable, updated only over the rects
// that changed, then a single blit and present. No per-frame allocation, no
// intermediate surface, and no full-surface upload unless the frame really did
// change everywhere.
//
// The reason this is a class rather than a function is the texture: it must
// survive between frames for partial upload to mean anything, and it must be
// destroyed and rebuilt when the drawable resizes. That lifetime is the whole
// of its state.
class SdlPresenter {
 public:
  SdlPresenter() = default;
  ~SdlPresenter();

  SdlPresenter(const SdlPresenter&) = delete;
  SdlPresenter& operator=(const SdlPresenter&) = delete;

  // Upload `dirty` (or everything, when `full_repaint` is set or the texture
  // was just created) and present. Returns false if the frame could not be
  // presented, in which case the caller should force a full repaint next frame
  // rather than trusting its damage bookkeeping.
  bool Present(SDL_Renderer* renderer,
               const gfx::Canvas& canvas,
               const gfx::DirtyRegion& dirty,
               bool full_repaint);

  // Drop the texture. Called on resize; also the recovery path when a present
  // fails.
  void Reset();

 private:
  bool EnsureTexture(SDL_Renderer* renderer, const gfx::IntSize& size);

  SDL_Texture* texture_ = nullptr;
  gfx::IntSize texture_size_;
  // False until the texture has been filled at least once. A partial upload
  // into a never-written streaming texture shows uninitialized driver memory in
  // the untouched region, so the first frame after any (re)creation is forced
  // to be full.
  bool texture_primed_ = false;
};

}  // namespace microbrowser::platform

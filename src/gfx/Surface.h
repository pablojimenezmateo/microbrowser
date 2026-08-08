#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "gfx/Geometry.h"

namespace microbrowser::gfx {

class Canvas;
class DisplayList;

// A rectangle of pixels the compositor owns, whose contents change without the
// display list changing.
//
// This exists because of one piece of arithmetic, recorded in ADR 0013. A 1080p
// video frame is about 8MB. Putting one in a display list would mean copying it
// into every frame's list, diffing it against the previous frame's copy sixty
// times a second, and serializing it across the process boundary each time --
// all to discover that a rectangle changed. The display list diff exists to
// avoid repainting what did not change, and video is precisely the case where
// its answer can only ever be "all of it".
//
// So the display list carries a *hole* instead: a DrawSurfaceCommand naming an
// id and a destination rectangle. The command is byte-identical between two
// frames of a playing video, because nothing about it changed -- what changed
// is the surface's contents, which is not the display list's business. Damage
// for a surface comes from its generation counter instead; see
// app/DirtyRegionPolicy.h.
//
// **A surface id is a name, not an index.** Every other side table in a display
// list (paths, text, fonts, images) is addressed by an index into *that list's*
// own vector, and ipc/Message.cpp is careful never to put one on the wire for
// exactly that reason: an index from a hostile renderer is an out-of-bounds
// read waiting to happen. A surface id is the opposite by construction -- it is
// looked up in a map, so an id naming a surface that does not exist is a miss
// that composites nothing. That is what makes a surface the one resource a
// display list can name across a trust boundary.
//
// **Ownership.** A Surface is written by whoever produces its frames and read
// by the compositor. Today that is one thread; when a decoder is out of process
// (ADR 0013 requires it to be) the frames arrive as messages and are applied on
// the same thread that composites, which is what keeps this a plain object with
// no lock in it. A decoder that writes here directly would need that decision
// revisited, and this paragraph is where to revisit it.
using SurfaceId = std::uint32_t;

// Never a valid surface. Zero-initialized command structs therefore name
// nothing rather than naming surface 0.
inline constexpr SurfaceId kNoSurface = 0;

class Surface {
 public:
  Surface(SurfaceId id, IntSize size);

  SurfaceId Id() const { return id_; }
  IntSize Size() const { return size_; }
  IntRect Bounds() const { return IntRect{0, 0, size_.width, size_.height}; }

  // A surface with no pixels composites nothing. It is a legitimate state --
  // a video element exists before its first frame decodes -- so it is a
  // question rather than an invariant.
  bool HasContent() const { return !pixels_.empty(); }

  // How many times the contents have been replaced.
  //
  // This is the whole damage model for a surface, and it is a counter rather
  // than an `is_playing` flag because a counter is an observation and a flag is
  // a claim. A paused video stops advancing its generation without anyone
  // having to remember to say so, and a decoder that stalls stops damaging the
  // screen instead of burning a full repaint per frame on unchanged pixels.
  std::uint64_t Generation() const { return generation_; }

  // Replaces the contents and bumps the generation. Refuses a span that is not
  // exactly width * height, and leaves the previous contents in place if so --
  // a frame of the wrong size is a decoder bug or a hostile message, and
  // showing the last good frame beats showing whatever the arithmetic produced.
  bool Update(std::span<const std::uint32_t> pixels);

  std::span<const std::uint32_t> Pixels() const { return pixels_; }
  // Null for a row outside the surface, and for a surface with no content.
  const std::uint32_t* Row(int y) const;

 private:
  SurfaceId id_ = kNoSurface;
  IntSize size_;
  std::uint64_t generation_ = 0;
  std::vector<std::uint32_t> pixels_;
};

// The largest surface either edge may be. The same bound ipc/Message.cpp puts
// on an image, and for the same reason: a size is arithmetic over
// attacker-influenced numbers before it is an allocation.
inline constexpr int kMaxSurfaceEdge = 16384;

// How many surfaces may exist at once. A page that could create them without
// bound could exhaust memory through a feature that is meant to save it.
inline constexpr std::size_t kMaxSurfaces = 64;

// The surfaces this compositor knows about, by id.
//
// A map rather than a vector, because the lookup has to be able to *fail*: that
// failure is what makes a surface id safe to accept from somewhere less trusted
// than here. Ids are never reused within one registry, so a stale id from an
// old frame names nothing rather than naming whatever was allocated next.
class SurfaceRegistry {
 public:
  // Null when the size is not a size, or when kMaxSurfaces already exist.
  Surface* Create(IntSize size);

  Surface* Find(SurfaceId id);
  const Surface* Find(SurfaceId id) const;

  // True if there was one to destroy. Ids are not recycled afterwards.
  bool Destroy(SurfaceId id);

  std::size_t Count() const { return surfaces_.size(); }

 private:
  std::unordered_map<SurfaceId, std::unique_ptr<Surface>> surfaces_;
  // Starts at one, so that kNoSurface is never handed out.
  SurfaceId next_id_ = 1;
};

// One surface, and where in the frame it goes.
//
// Produced by walking a display list; consumed by the presenter. The rectangle
// is already intersected with whatever clips were open around the command, so
// the consumer composites into it without replaying the clip stack -- which it
// could not do anyway, having only the placements and not the list.
struct SurfacePlacement {
  SurfaceId surface = kNoSurface;
  IntRect destination;

  friend bool operator==(const SurfacePlacement&, const SurfacePlacement&) = default;
};

// Paints every surface hole named by `list` into `canvas`. Called after `gfx::Execute`, which
// deliberately leaves holes empty.
void CompositeSurfaces(Canvas& canvas, const DisplayList& list, const SurfaceRegistry& surfaces);

}  // namespace microbrowser::gfx

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "engine/DecoderClient.h"
#include "engine/MediaElements.h"
#include "gfx/Surface.h"
#include "media/MediaState.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::engine {

// Decoded video frames for MSE-backed `<video>` elements. ADR 0028 §3 + ADR 0031.
//
// Owns the surface registry and the out-of-process decoders. `Page` coordinates; this class does the
// work of turning retained `SourceBuffer` samples into `gfx::Surface` pixels.
class PageVideo {
 public:
  explicit PageVideo(MediaElements& media) : media_(media) {}

  gfx::SurfaceRegistry& Surfaces() { return surfaces_; }
  const gfx::SurfaceRegistry& Surfaces() const { return surfaces_; }

  void Clear();

  std::optional<gfx::SurfaceId> SurfaceFor(const dom::Element& element) const;

  // Intrinsic size of the last applied video frame, or 0 before any frame.
  int VideoWidth(const dom::Element& element) const;
  int VideoHeight(const dom::Element& element) const;

  // Called when `play()` succeeds on an element attached to a `MediaSource`.
  void StartPlayback(dom::Element& element, media::MediaState& state);

  // Feeds samples, reads frames, and advances playback time. True when a surface generation changed.
  bool AdvancePlayback(dom::Element& element, media::MediaState& state);

  void AppendDecoderDescriptors(util::WaitDescriptorList& out) const;

  bool AdvanceAll(const std::function<media::MediaState*(dom::Element&)>& state_for);

  // Soonest wake for a playing session, so the loop sleeps between frames instead of waiting
  // forever on the decoder's pipe (which is what hung `microbrowser_snapshot` after `play()`).
  std::optional<std::uint32_t> NextDelayMs(std::int64_t now_ms) const;

  // Adds rectangles for surfaces whose generation changed since `last_generations` was updated.
  void AddSurfaceDamage(const gfx::DisplayList& list, std::vector<gfx::IntRect>& damage);

 private:
  struct TrackDecoder {
    std::unique_ptr<DecoderClient> client;
    const media::MediaTrack* track = nullptr;
    media::SourceBufferState* buffer = nullptr;
    std::size_t next_sample = 0;
    bool configured = false;
  };

  struct Session {
    dom::Element* element = nullptr;
    gfx::SurfaceId surface_id = gfx::kNoSurface;
    TrackDecoder video;
    TrackDecoder audio;
    double frame_duration = 1.0 / 30.0;
    std::int64_t last_advance_ms = 0;
    int frame_width = 0;
    int frame_height = 0;
  };

  media::MediaSourceState* SourceFor(const dom::Element& element) const;
  bool ConfigureTrack(TrackDecoder& decoder, media::SourceBufferState& buffer,
                      const media::MediaTrack& track);
  bool FeedSamples(TrackDecoder& decoder, double current_time, double horizon);
  bool ApplyVideoFrame(Session& session, const ipc::FrameMessage& frame);

  MediaElements& media_;
  gfx::SurfaceRegistry surfaces_;
  std::map<const dom::Element*, Session> sessions_;
  std::map<gfx::SurfaceId, std::uint64_t> last_generations_;
};

}  // namespace microbrowser::engine

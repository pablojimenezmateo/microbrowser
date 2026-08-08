#include "engine/PageVideo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

#include "dom/Node.h"
#include "gfx/DisplayList.h"
#include "ipc/DecoderMessage.h"
#include "media/CodecId.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

ipc::WireCodec ToWireCodec(media::CodecId codec) {
  switch (codec) {
    case media::CodecId::H264:
      return ipc::WireCodec::H264;
    case media::CodecId::Vp9:
      return ipc::WireCodec::Vp9;
    case media::CodecId::Av1:
      return ipc::WireCodec::Av1;
    case media::CodecId::Aac:
      return ipc::WireCodec::Aac;
    case media::CodecId::Opus:
      return ipc::WireCodec::Opus;
  }
  return ipc::WireCodec::Av1;
}

bool IsVideoBuffer(const media::SourceBufferState& buffer) {
  const std::string_view mime = buffer.MimeType();
  return mime.rfind("video/", 0) == 0;
}

bool IsAudioBuffer(const media::SourceBufferState& buffer) {
  const std::string_view mime = buffer.MimeType();
  return mime.rfind("audio/", 0) == 0;
}

std::int64_t SampleTimestampUs(const media::MediaSample& sample, std::uint32_t timescale) {
  if (timescale == 0) {
    return 0;
  }
  return static_cast<std::int64_t>((static_cast<double>(sample.decode_time) * 1'000'000.0) /
                                   static_cast<double>(timescale));
}

std::int64_t NowMs() {
  using Clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

}  // namespace

void PageVideo::Clear() {
  sessions_.clear();
  last_generations_.clear();
  surfaces_ = gfx::SurfaceRegistry{};
}

std::optional<gfx::SurfaceId> PageVideo::SurfaceFor(const dom::Element& element) const {
  const auto found = sessions_.find(&element);
  if (found == sessions_.end() || found->second.surface_id == gfx::kNoSurface) {
    return std::nullopt;
  }
  return found->second.surface_id;
}

int PageVideo::VideoWidth(const dom::Element& element) const {
  const auto found = sessions_.find(&element);
  return found == sessions_.end() ? 0 : found->second.frame_width;
}

int PageVideo::VideoHeight(const dom::Element& element) const {
  const auto found = sessions_.find(&element);
  return found == sessions_.end() ? 0 : found->second.frame_height;
}

media::MediaSourceState* PageVideo::SourceFor(const dom::Element& element) const {
  const std::uint64_t source_id = media_.SourceOf(element);
  return media_.Source(source_id);
}

bool PageVideo::ConfigureTrack(TrackDecoder& decoder, media::SourceBufferState& buffer,
                               const media::MediaTrack& track) {
  const std::optional<media::CodecId> codec = media::CodecFromContainerName(track.codec);
  if (!codec.has_value()) {
    AddPerformanceCounter(PerfCounterId::MediaVideoConfigureFailures);
    return false;
  }
  std::vector<std::uint8_t> extra;
  if (!buffer.CopyCodecExtraData(track, extra)) {
    AddPerformanceCounter(PerfCounterId::MediaVideoConfigureFailures);
    return false;
  }
  decoder.client = std::make_unique<DecoderClient>();
  if (!decoder.client->Configure(ToWireCodec(*codec), extra)) {
    decoder.client.reset();
    AddPerformanceCounter(PerfCounterId::MediaVideoConfigureFailures);
    return false;
  }
  decoder.track = &track;
  decoder.buffer = &buffer;
  decoder.next_sample = 0;
  decoder.configured = true;
  return true;
}

void PageVideo::StartPlayback(dom::Element& element, media::MediaState& state) {
  media::MediaSourceState* source = SourceFor(element);
  if (source == nullptr) {
    return;
  }

  Session session;
  session.element = &element;

  media::SourceBufferState* video_buffer = nullptr;
  media::SourceBufferState* audio_buffer = nullptr;
  const media::MediaTrack* video_track = nullptr;
  const media::MediaTrack* audio_track = nullptr;

  for (std::size_t i = 0; i < source->BufferCount(); ++i) {
    media::SourceBufferState* buffer = source->BufferAt(i);
    if (buffer == nullptr || !buffer->HasInitSegment()) {
      continue;
    }
    for (const media::MediaTrack& track : buffer->Tracks()) {
      if (track.kind == media::TrackKind::Video && video_track == nullptr && IsVideoBuffer(*buffer)) {
        video_track = &track;
        video_buffer = buffer;
      } else if (track.kind == media::TrackKind::Audio && audio_track == nullptr &&
                 IsAudioBuffer(*buffer)) {
        audio_track = &track;
        audio_buffer = buffer;
      }
    }
  }

  if (video_track == nullptr || video_buffer == nullptr) {
    AddPerformanceCounter(PerfCounterId::MediaVideoConfigureFailures);
    return;
  }

  if (!ConfigureTrack(session.video, *video_buffer, *video_track)) {
    return;
  }
  if (audio_track != nullptr && audio_buffer != nullptr) {
    (void)ConfigureTrack(session.audio, *audio_buffer, *audio_track);
  }

  const int width = video_track->width > 0 ? video_track->width : 640;
  const int height = video_track->height > 0 ? video_track->height : 360;
  if (gfx::Surface* surface = surfaces_.Create(gfx::IntSize{width, height})) {
    session.surface_id = surface->Id();
  }

  if (video_track->timescale != 0 && video_track->duration != 0) {
    session.frame_duration =
        static_cast<double>(video_track->duration) / static_cast<double>(video_track->timescale);
    if (!(session.frame_duration > 0.0)) {
      session.frame_duration = 1.0 / 30.0;
    }
  }

  sessions_[&element] = std::move(session);
  AddPerformanceCounter(PerfCounterId::MediaVideoSessions);
  state.AdvanceTo(state.CurrentTime());
}

bool PageVideo::FeedSamples(TrackDecoder& decoder, double current_time, double horizon) {
  if (!decoder.configured || decoder.client == nullptr || decoder.track == nullptr ||
      decoder.buffer == nullptr) {
    return false;
  }
  const std::uint32_t timescale = decoder.track->timescale;
  if (timescale == 0) {
    return false;
  }
  const double scale = 1.0 / static_cast<double>(timescale);
  std::vector<media::MediaSample> ordered;
  for (const media::SourceBufferState::RetainedSegment& segment : decoder.buffer->Segments()) {
    for (const media::MediaSample& sample : segment.samples) {
      if (sample.track_id == decoder.track->id) {
        ordered.push_back(sample);
      }
    }
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const media::MediaSample& a, const media::MediaSample& b) {
              return a.decode_time < b.decode_time;
            });
  bool fed = false;
  for (std::size_t i = decoder.next_sample; i < ordered.size(); ++i) {
    const media::MediaSample& sample = ordered[i];
    const double at = static_cast<double>(sample.decode_time) * scale;
    if (at + 0.001 < current_time) {
      decoder.next_sample = i + 1;
      continue;
    }
    if (at > horizon) {
      break;
    }
    std::vector<std::uint8_t> bytes;
    if (!decoder.buffer->CopySampleBytes(sample, bytes)) {
      continue;
    }
    if (!decoder.client->PushSample(SampleTimestampUs(sample, timescale), sample.is_sync, bytes)) {
      return fed;
    }
    decoder.next_sample = i + 1;
    fed = true;
    AddPerformanceCounter(PerfCounterId::MediaDecoderSamplesFed);
  }
  return fed;
}

bool PageVideo::ApplyVideoFrame(Session& session, const ipc::FrameMessage& frame) {
  // Audio frames carry sample_count; one DecoderClient is one stream (ADR 0031).
  if (frame.sample_count != 0 || frame.width == 0 || frame.height == 0) {
    return false;
  }
  const std::uint64_t expected =
      static_cast<std::uint64_t>(frame.width) * static_cast<std::uint64_t>(frame.height);
  if (frame.bytes.size() != expected * 4u) {
    return false;
  }

  gfx::Surface* surface = surfaces_.Find(session.surface_id);
  // Track metadata width/height is often missing or wrong (youtube WebM inits
  // report 0; we then guessed 640x360). The decoded frame is authoritative, and
  // Surface::Update refuses any other pixel count -- which left currentTime at 0
  // despite media.decoder_frames climbing.
  if (surface == nullptr || surface->Size().width != static_cast<int>(frame.width) ||
      surface->Size().height != static_cast<int>(frame.height)) {
    surface = surfaces_.Create(
        gfx::IntSize{static_cast<int>(frame.width), static_cast<int>(frame.height)});
    if (surface == nullptr) {
      return false;
    }
    session.surface_id = surface->Id();
  }

  std::vector<std::uint32_t> pixels(expected);
  for (std::size_t i = 0; i < expected; ++i) {
    const std::size_t o = i * 4u;
    const std::uint8_t r = frame.bytes[o];
    const std::uint8_t g = frame.bytes[o + 1];
    const std::uint8_t b = frame.bytes[o + 2];
    const std::uint8_t a = frame.bytes[o + 3];
    pixels[i] = (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }
  if (!surface->Update(pixels)) {
    return false;
  }
  session.frame_width = static_cast<int>(frame.width);
  session.frame_height = static_cast<int>(frame.height);
  AddPerformanceCounter(PerfCounterId::MediaDecoderFramesApplied);
  return true;
}

bool PageVideo::AdvancePlayback(dom::Element& element, media::MediaState& state) {
  const auto found = sessions_.find(&element);
  if (found == sessions_.end() || state.Paused()) {
    return false;
  }
  Session& session = found->second;
  if (session.video.client == nullptr) {
    return false;
  }

  const std::int64_t now_ms = NowMs();
  const std::int64_t frame_ms =
      std::max<std::int64_t>(1, static_cast<std::int64_t>(session.frame_duration * 1000.0));
  const bool due =
      session.last_advance_ms == 0 || now_ms - session.last_advance_ms >= frame_ms;

  const double current = state.CurrentTime();
  if (due) {
    session.last_advance_ms = now_ms;
    const double horizon = current + 2.0;
    (void)FeedSamples(session.video, current, horizon);
    if (session.audio.configured && session.audio.client != nullptr) {
      (void)FeedSamples(session.audio, current, horizon);
    }
  }

  bool updated = false;
  double latest_time = current;
  std::string decode_error;
  for (const ipc::FrameMessage& frame : session.video.client->PollFrames(&decode_error)) {
    AddPerformanceCounter(PerfCounterId::MediaDecoderFrames);
    if (!ApplyVideoFrame(session, frame)) {
      continue;
    }
    updated = true;
    const double at = static_cast<double>(frame.timestamp_us) / 1'000'000.0;
    if (at > latest_time) {
      latest_time = at;
    }
  }
  if (!decode_error.empty()) {
    AddPerformanceCounter(PerfCounterId::MediaDecoderErrors);
  }
  if (session.audio.client != nullptr) {
    (void)session.audio.client->PollFrames();
  }

  // Prefer the frame's own timestamp. Advancing only on the paced tick left
  // currentTime at 0 while media.decoder_frames_applied climbed -- early
  // arrivals updated the surface and returned without touching the clock.
  if (updated) {
    if (latest_time > current) {
      state.AdvanceTo(latest_time);
    } else if (due) {
      state.AdvanceTo(current + session.frame_duration);
    }
  }
  return updated;
}

bool PageVideo::AdvanceAll(const std::function<media::MediaState*(dom::Element&)>& state_for) {
  bool changed = false;
  std::vector<dom::Element*> elements;
  elements.reserve(sessions_.size());
  for (const auto& entry : sessions_) {
    elements.push_back(entry.second.element);
  }
  for (dom::Element* element : elements) {
    if (element == nullptr) {
      continue;
    }
    if (media::MediaState* state = state_for(*element)) {
      changed = AdvancePlayback(*element, *state) || changed;
    }
  }
  return changed;
}

std::optional<std::uint32_t> PageVideo::NextDelayMs(std::int64_t now_ms) const {
  std::optional<std::uint32_t> soonest;
  for (const auto& [element, session] : sessions_) {
    (void)element;
    if (session.video.client == nullptr || !session.video.configured ||
        session.video.track == nullptr || session.video.buffer == nullptr) {
      continue;
    }
    // No more coded frames to push: do not pace the loop on an empty pump.
    // The decoder pipe stays in AppendDecoderDescriptors, so a late Frame still
    // wakes. Without this, play() held microbrowser_snapshot's post-load drain
    // for the full 20s wall budget after every sample was already fed.
    std::size_t sample_count = 0;
    for (const media::SourceBufferState::RetainedSegment& segment :
         session.video.buffer->Segments()) {
      for (const media::MediaSample& sample : segment.samples) {
        if (sample.track_id == session.video.track->id) {
          ++sample_count;
        }
      }
    }
    if (session.video.next_sample >= sample_count) {
      continue;
    }
    const std::int64_t frame_ms =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(session.frame_duration * 1000.0));
    std::uint32_t delay = 0;
    if (session.last_advance_ms != 0) {
      const std::int64_t elapsed = now_ms - session.last_advance_ms;
      if (elapsed < frame_ms) {
        delay = static_cast<std::uint32_t>(frame_ms - elapsed);
      }
    }
    soonest = soonest.has_value() ? std::min(*soonest, delay) : delay;
  }
  return soonest;
}

void PageVideo::AppendDecoderDescriptors(util::WaitDescriptorList& out) const {
  for (const auto& [element, session] : sessions_) {
    (void)element;
    if (session.video.client != nullptr) {
      if (const std::optional<util::WaitDescriptor> interest = session.video.client->Interest()) {
        out.push_back(*interest);
      }
    }
    if (session.audio.client != nullptr) {
      if (const std::optional<util::WaitDescriptor> interest = session.audio.client->Interest()) {
        out.push_back(*interest);
      }
    }
  }
}

void PageVideo::AddSurfaceDamage(const gfx::DisplayList& list, std::vector<gfx::IntRect>& damage) {
  for (const gfx::SurfacePlacement& placement : gfx::SurfacePlacements(list)) {
    const gfx::Surface* surface = surfaces_.Find(placement.surface);
    if (surface == nullptr) {
      continue;
    }
    const std::uint64_t generation = surface->Generation();
    const auto previous = last_generations_.find(placement.surface);
    if (previous == last_generations_.end() || previous->second != generation) {
      damage.push_back(placement.destination);
      last_generations_[placement.surface] = generation;
    }
  }
}

}  // namespace microbrowser::engine

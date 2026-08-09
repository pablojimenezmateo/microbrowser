#include "media/MediaSourceState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "media/CodecId.h"
#include "media/Matroska.h"
#include "media/Ump.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::media {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// What Append needs from either demuxer: tracks (init) and samples (media), with durations filled.
struct ParsedSegment {
  std::vector<MediaTrack> tracks;
  std::vector<MediaSample> samples;
  std::uint32_t timescale = 0;
};

// Matroska samples carry no duration. Coded-frame processing needs one to place a range, so each
// sample lasts until the next on the same track (or one tick past the last). Guessing a frame rate
// would put frames at times nothing encoded; abutting neighbours is what the container already said.
void FillMissingDurations(std::vector<MediaSample>& samples) {
  if (samples.empty()) {
    return;
  }
  std::vector<std::size_t> order(samples.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    if (samples[a].track_id != samples[b].track_id) {
      return samples[a].track_id < samples[b].track_id;
    }
    return samples[a].decode_time < samples[b].decode_time;
  });
  for (std::size_t i = 0; i < order.size(); ++i) {
    MediaSample& sample = samples[order[i]];
    if (sample.duration != 0) {
      continue;
    }
    std::uint32_t duration = 1;
    if (i + 1 < order.size() && samples[order[i + 1]].track_id == sample.track_id) {
      const std::uint64_t next = samples[order[i + 1]].decode_time;
      if (next > sample.decode_time) {
        const std::uint64_t delta = next - sample.decode_time;
        duration = delta > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(delta);
      }
    }
    sample.duration = duration;
  }
}

std::optional<ParsedSegment> TryIsoBmff(std::span<const std::byte> bytes) {
  const IsoBmffFile parsed = ParseIsoBmff(bytes);
  if (!parsed.Ok()) {
    return std::nullopt;
  }
  ParsedSegment out;
  out.tracks = parsed.tracks;
  out.samples = parsed.samples;
  for (const MediaTrack& track : out.tracks) {
    if (track.timescale != 0) {
      out.timescale = track.timescale;
      break;
    }
  }
  return out;
}

std::optional<ParsedSegment> TryMatroska(std::span<const std::byte> bytes) {
  if (!IsMatroska(bytes)) {
    return std::nullopt;
  }
  const std::optional<MatroskaFile> parsed = ParseMatroska(bytes);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  ParsedSegment out;
  out.tracks = parsed->tracks;
  out.samples = parsed->samples;
  FillMissingDurations(out.samples);
  for (const MediaTrack& track : out.tracks) {
    if (track.timescale != 0) {
      out.timescale = track.timescale;
      break;
    }
  }
  // A media-only cluster has no TrackEntry; timescale must already be known on the buffer.
  // An EBML header with neither tracks nor samples is not a successful segment -- treating it as
  // Ok starved the init path and let the player believe an append landed.
  if (out.tracks.empty() && out.samples.empty()) {
    return std::nullopt;
  }
  return out;
}

std::optional<ParsedSegment> ParseAppendBytes(std::span<const std::byte> bytes,
                                              std::vector<std::byte>& owned_ump) {
  if (auto iso = TryIsoBmff(bytes)) {
    return iso;
  }
  if (auto mkv = TryMatroska(bytes)) {
    return mkv;
  }
  // Last resort: YouTube UMP wrapping media the player failed to unwrap before appendBuffer.
  std::optional<std::vector<std::byte>> media = ExtractUmpMedia(bytes);
  if (!media.has_value()) {
    return std::nullopt;
  }
  owned_ump = std::move(*media);
  const std::span<const std::byte> inner(owned_ump.data(), owned_ump.size());
  if (auto iso = TryIsoBmff(inner)) {
    return iso;
  }
  return TryMatroska(inner);
}

}  // namespace

void SourceBufferState::SetTimestampOffset(double seconds) {
  if (std::isfinite(seconds)) {
    timestamp_offset_ = seconds;
  }
}

void SourceBufferState::SetAppendWindow(double start, double end) {
  if (std::isfinite(start) && start >= 0.0) {
    append_window_start_ = start;
  }
  // Infinity is the initial value and a legal one -- "no end" -- so it is stored as zero and read as
  // "unbounded" rather than compared against. A NaN end is ignored, which leaves the previous value
  // rather than making every later frame fail a comparison with it.
  if (std::isinf(end) && end > 0.0) {
    append_window_end_ = 0.0;
  } else if (std::isfinite(end) && end > append_window_start_) {
    append_window_end_ = end;
  }
}

void SourceBufferState::AddFrame(double start, double end) {
  const double offset_start = start + timestamp_offset_;
  const double offset_end = end + timestamp_offset_;
  if (!std::isfinite(offset_start) || !std::isfinite(offset_end) || offset_end <= offset_start) {
    return;
  }
  // The append window, and frames outside it are *dropped* rather than clamped. Clamping would move
  // a frame to a presentation time it was not encoded for, which is a worse answer than not having
  // it: the player can fetch a missing frame and cannot detect a moved one.
  if (offset_end <= append_window_start_) {
    return;
  }
  if (append_window_end_ > 0.0 && offset_start >= append_window_end_) {
    return;
  }
  buffered_.Add(offset_start, offset_end);
}

AppendResult SourceBufferState::Append(std::span<const std::byte> bytes, std::size_t quota_bytes) {
  if (updating_) {
    return AppendResult::AlreadyUpdating;
  }
  // True for the duration of this call only. Page-visible `updating` (between `appendBuffer`
  // returning and `updateend`) is held on the binding wrapper -- the algorithm itself is sync, and
  // leaving this set until `TakeEvents` would make a second `Append` in the unit tests refuse.
  updating_ = true;
  if (bytes.empty()) {
    // A zero-length append is a no-op that still fires the event pair, which is what a player
    // polling with an empty buffer expects rather than an error.
    events_.push_back("updatestart");
    events_.push_back("update");
    events_.push_back("updateend");
    updating_ = false;
    return AppendResult::Ok;
  }
  // **The quota is checked before the bytes are copied**, not after. Checking afterwards means the
  // allocation the quota exists to prevent has already happened.
  if (bytes_held_ > quota_bytes || bytes.size() > quota_bytes - bytes_held_) {
    events_.push_back("updatestart");
    events_.push_back("error");
    events_.push_back("updateend");
    AddPerformanceCounter(PerfCounterId::MediaSourceQuotaRefusals);
    updating_ = false;
    return AppendResult::QuotaExceeded;
  }

  std::vector<std::byte> ump_owned;
  std::optional<ParsedSegment> parsed = ParseAppendBytes(bytes, ump_owned);
  // When UMP unwrapped, store the *inner* bytes so sample offsets stay valid.
  const std::span<const std::byte> stored =
      ump_owned.empty() ? bytes : std::span<const std::byte>(ump_owned.data(), ump_owned.size());
  if (!parsed.has_value()) {
    events_.push_back("updatestart");
    events_.push_back("error");
    events_.push_back("updateend");
    AddPerformanceCounter(PerfCounterId::MediaSourceAppendFailures);
    updating_ = false;
    return AppendResult::ParseFailed;
  }

  events_.push_back("updatestart");
  // An initialization segment: tracks and, crucially, the timescale. A segment carrying tracks
  // *replaces* what was there, which is what a player changing representation mid-stream does.
  if (!parsed->tracks.empty()) {
    tracks_ = parsed->tracks;
    timescale_ = parsed->timescale;
    if (timescale_ == 0) {
      // No usable timescale anywhere. Every timestamp in every later segment would be meaningless,
      // so this is a failed initialization rather than a successful one with a guess in it.
      tracks_.clear();
      events_.push_back("error");
      events_.push_back("updateend");
      AddPerformanceCounter(PerfCounterId::MediaSourceAppendFailures);
      updating_ = false;
      return AppendResult::ParseFailed;
    }
    AddPerformanceCounter(PerfCounterId::MediaSourceInitSegments);
    if (!init_segment_.empty()) {
      bytes_held_ -= init_segment_.size();
    }
    init_segment_.assign(stored.begin(), stored.end());
    bytes_held_ += init_segment_.size();
    segment_views_.clear();
  }

  if (!parsed->samples.empty()) {
    if (timescale_ == 0) {
      // Media before initialization. Refused rather than buffered at a guessed timescale: a frame at
      // the wrong time is indistinguishable from a frame at the right one until playback.
      events_.push_back("error");
      events_.push_back("updateend");
      AddPerformanceCounter(PerfCounterId::MediaSourceAppendFailures);
      updating_ = false;
      return AppendResult::ParseFailed;
    }
    // **The division happens once per timestamp, on an integer that is already exact.** Dividing
    // `decode_time` and `duration` separately and adding the results is the obvious way to write this
    // and it is wrong: seven frames of 40 ticks accumulate to a value a few ulps away from 280 ticks
    // divided once, so the eighth frame does not abut the seventh and `buffered` reports a gap of
    // 2e-17 seconds. A player reads that gap and spends a request on it. Found by a test that appended
    // two adjacent segments and got two ranges.
    const double scale = 1.0 / static_cast<double>(timescale_);
    const auto seconds = [scale](std::uint64_t ticks) {
      return static_cast<double>(ticks) * scale;
    };
    // Coded frame processing, and the overlap half of it: the frames in this append *replace* whatever
    // occupied their times. A player re-appending a segment at a lower bitrate depends on this --
    // without it the old range survives and `buffered` describes a mixture of two representations.
    double first = std::numeric_limits<double>::infinity();
    double last = -std::numeric_limits<double>::infinity();
    for (const MediaSample& sample : parsed->samples) {
      first = std::min(first, seconds(sample.decode_time));
      last = std::max(last, seconds(sample.decode_time + sample.duration));
    }
    if (std::isfinite(first) && std::isfinite(last) && last > first) {
      Remove(first + timestamp_offset_, last + timestamp_offset_);
    }
    for (const MediaSample& sample : parsed->samples) {
      AddFrame(seconds(sample.decode_time), seconds(sample.decode_time + sample.duration));
    }
    Segment segment;
    segment.bytes.assign(stored.begin(), stored.end());
    segment.samples = std::move(parsed->samples);
    const std::size_t frame_count = segment.samples.size();
    bytes_held_ += segment.bytes.size();
    segments_.push_back(std::move(segment));
    segment_views_.clear();
    AddPerformanceCounter(PerfCounterId::MediaSourceFramesBuffered, frame_count);
  }
  events_.push_back("update");
  events_.push_back("updateend");
  AddPerformanceCounter(PerfCounterId::MediaSourceAppends);
  updating_ = false;
  return AppendResult::Ok;
}

void SourceBufferState::Remove(double start, double end) {
  buffered_.Remove(start, end);
  // The bytes go too, and this is the half a first draft leaves out: a `remove` that shrank
  // `buffered` without freeing anything would make the quota unrecoverable, so a player told to evict
  // would evict, retry, and be refused again forever.
  //
  // A segment is dropped only when *none* of its frames survive in `buffered`, because a segment is
  // the unit its sample offsets are relative to -- there is no way to free half of one.
  const double scale = timescale_ == 0 ? 0.0 : 1.0 / static_cast<double>(timescale_);
  std::vector<Segment> kept;
  kept.reserve(segments_.size());
  std::size_t held = 0;
  for (Segment& segment : segments_) {
    bool any_alive = false;
    for (const MediaSample& sample : segment.samples) {
      const double at = static_cast<double>(sample.decode_time) * scale + timestamp_offset_;
      if (buffered_.Contains(at)) {
        any_alive = true;
        break;
      }
    }
    if (any_alive) {
      held += segment.bytes.size();
      kept.push_back(std::move(segment));
    } else {
      AddPerformanceCounter(PerfCounterId::MediaSourceSegmentsEvicted);
    }
  }
  segments_ = std::move(kept);
  bytes_held_ = held;
}

void SourceBufferState::Abort() {
  // The specification's abort: the parser state and the append window reset, and nothing is
  // un-appended. Frames already processed stay processed -- a page calling `abort` is cancelling a
  // request, not undoing one.
  updating_ = false;
  append_window_start_ = 0.0;
  append_window_end_ = 0.0;
  events_.push_back("abort");
  events_.push_back("updateend");
}

bool SourceBufferState::ChangeType(std::string mime_type) {
  if (updating_) {
    return false;
  }
  if (!IsSupportedMediaSourceType(mime_type)) {
    return false;
  }
  // Subsequent appends must include a new initialization segment for the new
  // type. Clearing tracks/timescale is what makes a bare media segment fail
  // until that init arrives -- the same rule `Append` already enforces.
  mime_type_ = std::move(mime_type);
  tracks_.clear();
  timescale_ = 0;
  if (!init_segment_.empty()) {
    bytes_held_ -= init_segment_.size();
    init_segment_.clear();
  }
  return true;
}

std::vector<std::string_view> SourceBufferState::TakeEvents() {
  std::vector<std::string_view> taken;
  taken.swap(events_);
  return taken;
}

void MediaSourceState::Attach() {
  if (state_ != ReadyState::Closed) {
    return;
  }
  state_ = ReadyState::Open;
  events_.push_back("sourceopen");
}

void MediaSourceState::Detach() {
  if (state_ == ReadyState::Closed) {
    return;
  }
  state_ = ReadyState::Closed;
  // Every buffer goes with it, which is what makes "no MediaSource outlives the element it was
  // attached to" a property of the type rather than a rule a caller follows.
  buffers_.clear();
  duration_set_ = false;
  duration_ = 0.0;
  events_.push_back("sourceclose");
}

void MediaSourceState::EndOfStream() {
  if (state_ != ReadyState::Open) {
    return;
  }
  state_ = ReadyState::Ended;
  events_.push_back("sourceended");
}

void MediaSourceState::ReopenForAppend() {
  if (state_ == ReadyState::Ended) {
    state_ = ReadyState::Open;
    // No event: the specification's "ended" -> "open" transition on append fires nothing, because a
    // player did not ask for it and a `sourceopen` here would make one re-run its setup.
  }
}

SourceBufferState* MediaSourceState::AddSourceBuffer(const std::string& mime_type) {
  if (state_ != ReadyState::Open || buffers_.size() >= kMaxBuffers) {
    return nullptr;
  }
  // The codec allowlist, and it is the same one the decoder process is bound by (ADR 0031 §3): a
  // type this browser cannot decode is refused *here*, before a page fills a buffer with bytes
  // nothing will ever read. Two answers to "can we play this" would be two answers to give a page.
  if (!IsSupportedMediaSourceType(mime_type)) {
    return nullptr;
  }
  buffers_.push_back(std::make_unique<SourceBufferState>(mime_type));
  AddPerformanceCounter(PerfCounterId::MediaSourceBuffersCreated);
  return buffers_.back().get();
}

void MediaSourceState::RemoveSourceBuffer(SourceBufferState* buffer) {
  const auto found = std::find_if(
      buffers_.begin(), buffers_.end(),
      [buffer](const std::unique_ptr<SourceBufferState>& held) { return held.get() == buffer; });
  if (found != buffers_.end()) {
    buffers_.erase(found);
  }
}

SourceBufferState* MediaSourceState::BufferAt(std::size_t index) {
  return index < buffers_.size() ? buffers_[index].get() : nullptr;
}

const SourceBufferState* MediaSourceState::BufferAt(std::size_t index) const {
  return index < buffers_.size() ? buffers_[index].get() : nullptr;
}

double MediaSourceState::Duration() const {
  if (duration_set_) {
    return duration_;
  }
  // Derived from what is buffered when a page has not said. Not NaN: the specification's initial
  // duration is NaN, and a page reading NaN before its first append is correct -- but that is the
  // *element's* duration, which `MediaState` answers. This is the source's own view, and the largest
  // buffered end is the only honest number here.
  double largest = 0.0;
  for (const std::unique_ptr<SourceBufferState>& buffer : buffers_) {
    largest = std::max(largest, buffer->Buffered().LargestEnd());
  }
  return largest;
}

void MediaSourceState::SetDuration(double seconds) {
  if (state_ != ReadyState::Open || !std::isfinite(seconds) || seconds < 0.0) {
    return;
  }
  duration_ = seconds;
  duration_set_ = true;
}

std::size_t MediaSourceState::BytesHeld() const {
  std::size_t total = 0;
  for (const std::unique_ptr<SourceBufferState>& buffer : buffers_) {
    total += buffer->BytesHeld();
  }
  return total;
}

std::vector<std::string_view> MediaSourceState::TakeEvents() {
  std::vector<std::string_view> taken;
  taken.swap(events_);
  return taken;
}

std::span<const SourceBufferState::RetainedSegment> SourceBufferState::Segments() const {
  if (segment_views_.size() != segments_.size()) {
    segment_views_.clear();
    segment_views_.reserve(segments_.size());
    for (const Segment& segment : segments_) {
      segment_views_.push_back(RetainedSegment{
          std::span<const std::byte>(segment.bytes.data(), segment.bytes.size()),
          std::span<const MediaSample>(segment.samples.data(), segment.samples.size())});
    }
  }
  return std::span<const RetainedSegment>(segment_views_.data(), segment_views_.size());
}

bool SourceBufferState::CopySampleBytes(const MediaSample& sample,
                                        std::vector<std::uint8_t>& out) const {
  for (const Segment& segment : segments_) {
    for (const MediaSample& candidate : segment.samples) {
      if (candidate.track_id != sample.track_id || candidate.offset != sample.offset ||
          candidate.size != sample.size || candidate.decode_time != sample.decode_time) {
        continue;
      }
      if (sample.size > segment.bytes.size() || sample.offset > segment.bytes.size() - sample.size) {
        return false;
      }
      const auto* begin = reinterpret_cast<const std::uint8_t*>(segment.bytes.data() + sample.offset);
      out.assign(begin, begin + sample.size);
      return true;
    }
  }
  return false;
}

bool SourceBufferState::CopyCodecExtraData(const MediaTrack& track,
                                           std::vector<std::uint8_t>& out) const {
  if (track.codec_config_size == 0) {
    out.clear();
    return true;
  }
  if (track.codec_config_offset > init_segment_.size() ||
      track.codec_config_size > init_segment_.size() - track.codec_config_offset) {
    return false;
  }
  const auto* begin =
      reinterpret_cast<const std::uint8_t*>(init_segment_.data() + track.codec_config_offset);
  out.assign(begin, begin + track.codec_config_size);
  return true;
}

}  // namespace microbrowser::media

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "media/BufferedRanges.h"
#include "media/IsoBmff.h"

namespace microbrowser::media {

// `MediaSource` and `SourceBuffer`, as state and an algorithm and nothing else.
//
// ADR 0028 §3, session 28. No element, no decoder, no network, no JavaScript -- the same shape
// `MediaState` has, and for the same reason: **the append algorithm is the part that is easy to get
// subtly wrong, and it can only be asserted if it is a pure object.** What a page sees is built on
// top of this in `src/bindings`; what feeds it is `src/engine`.
//
// The thing ADR 0028 warns about is worth repeating where the code is. `appendBuffer` is not "hand
// bytes to the decoder". It runs the coded frame processing algorithm: parse the segment, work out
// each frame's presentation time, remove whatever the new frames overlap, and coalesce the result
// into `buffered`. A player reads `buffered` and decides what to fetch from it, so an error here does
// not show up as a glitch -- it shows up as a player that downloads the wrong thing forever.
//
// **This is also where a page can allocate unbounded memory**, one `appendBuffer` at a time. The
// quota is the only thing between a page and the machine's RAM, and `QuotaExceededError` is not an
// error condition to avoid: it is the specified signal a player is *waiting for*, and how it is told
// to evict. Refusing quietly, or growing, would both break players that handle it correctly.

// What a `SourceBuffer` did with an append.
enum class AppendResult : std::uint8_t {
  Ok,
  QuotaExceeded,   // -> QuotaExceededError, and the player evicts and retries
  ParseFailed,     // -> the `error` event and the source ends up in "ended"
  NotOpen,         // -> InvalidStateError: the MediaSource is closed or ended
  AlreadyUpdating, // -> InvalidStateError: an append is already in flight
};

// One `SourceBuffer`.
//
// Holds *descriptions* rather than samples: a coded frame here is a byte range in the append the page
// made, exactly as `IsoBmff` produces them, because this module may not name anything that could
// decode one. The bytes themselves are retained -- they have to be, a page appends them once and the
// decoder reads them later -- but nothing here looks inside them.
class SourceBufferState {
 public:
  // The MIME type the page asked for, kept as the string it used. Whether it is *supported* is
  // decided by `media::CodecIdFromMimeType` before one of these is created, so by the time it exists
  // the answer is yes.
  explicit SourceBufferState(std::string mime_type) : mime_type_(std::move(mime_type)) {}

  const std::string& MimeType() const { return mime_type_; }
  const BufferedRanges& Buffered() const { return buffered_; }
  bool Updating() const { return updating_; }
  // Whether an initialization segment has been received. Until one has, a media segment is a parse
  // error rather than data -- there is no timescale to convert its timestamps with, and guessing one
  // would put frames at times nothing asked for.
  bool HasInitSegment() const { return !tracks_.empty(); }
  const std::vector<MediaTrack>& Tracks() const { return tracks_; }

  // `timestampOffset`: what a player sets to splice a segment in at a different time than its own
  // timestamps say. Applied when frames are added, not when they are parsed, so that setting it
  // between appends does what a player expects.
  double TimestampOffset() const { return timestamp_offset_; }
  void SetTimestampOffset(double seconds);

  // The append window, which a player uses to clip a segment that overlaps a period boundary. Frames
  // wholly outside it are dropped rather than clamped -- clamping would move a frame to a time it was
  // not encoded for.
  void SetAppendWindow(double start, double end);

  // How many bytes this buffer is holding, which is what the quota is against.
  std::size_t BytesHeld() const { return bytes_held_; }

  // Starts an append. Parses, runs coded frame processing, and updates `buffered`.
  //
  // Synchronous here and asynchronous to a page: this returns the answer, and the binding layer
  // reports `updating` and fires `updatestart`/`update`/`updateend` around it. That split is
  // deliberate -- the *algorithm* is not asynchronous, only its observable sequencing is, and mixing
  // the two would make the algorithm untestable.
  AppendResult Append(std::span<const std::byte> bytes, std::size_t quota_bytes);

  // `SourceBuffer.remove`, and the eviction a player performs when the quota was exceeded.
  void Remove(double start, double end);

  // `SourceBuffer.abort`: the current append is discarded. Nothing is un-appended -- the
  // specification's abort resets the parser state and the append window, and does not roll back
  // frames that were already processed.
  void Abort();

  // Events this buffer produced, in order, taken by the caller. Same contract as
  // `MediaState::TakeEvents`: nothing here dispatches anything.
  std::vector<std::string_view> TakeEvents();

  // Whether the last append's frames were retained. Exposed for the media element's readiness ladder,
  // which is driven by whether the buffer holds the current time.
  bool Contains(double time) const { return buffered_.Contains(time); }

 private:
  void AddFrame(double start, double end);

  std::string mime_type_;
  BufferedRanges buffered_;
  std::vector<MediaTrack> tracks_;
  // The bytes a page appended, kept per append rather than concatenated: the ranges in a
  // `MediaSample` are relative to the append they came from, and concatenating would make every
  // offset in every earlier append wrong.
  struct Segment {
    std::vector<std::byte> bytes;
    std::vector<MediaSample> samples;
  };
  std::vector<Segment> segments_;
  std::vector<std::string_view> events_;
  double timestamp_offset_ = 0.0;
  double append_window_start_ = 0.0;
  // Positive infinity is the specification's initial value, and it is not a sentinel: a player can
  // set a finite end and set it back.
  double append_window_end_ = 0.0;
  std::size_t bytes_held_ = 0;
  bool updating_ = false;
  // The timescale of the first track in the initialization segment, which every timestamp in every
  // later media segment is divided by. Zero means "no initialization segment yet".
  std::uint32_t timescale_ = 0;
};

// `MediaSource`, and the object a `URL.createObjectURL` names.
class MediaSourceState {
 public:
  // The specification's three, with its own spelling. `Closed` is where one starts and where it
  // returns when the element detaches.
  enum class ReadyState : std::uint8_t { Closed, Open, Ended };

  ReadyState State() const { return state_; }

  // Attaching to a media element is what opens a MediaSource -- not construction. A page that
  // constructs one and appends to it without attaching gets `InvalidStateError`, which is the
  // specified behaviour and the reason `Append` checks.
  void Attach();
  void Detach();

  // `endOfStream()`: no more data is coming. The duration becomes what is buffered, and the element's
  // readiness can reach "have enough data" -- which it cannot while a source is open, because more
  // might arrive.
  void EndOfStream();
  // An append while `ended` puts the source back to `open`, which is what the specification says and
  // what a live stream does every time it gets another segment after a lull. Without it a player that
  // called `endOfStream` optimistically could never append again.
  void ReopenForAppend();

  // `addSourceBuffer`. Null when the type is not one this browser can decode, which the caller turns
  // into `NotSupportedError`, or when the source is not open.
  SourceBufferState* AddSourceBuffer(const std::string& mime_type);
  void RemoveSourceBuffer(SourceBufferState* buffer);

  std::size_t BufferCount() const { return buffers_.size(); }
  SourceBufferState* BufferAt(std::size_t index);

  // `duration`. Explicitly set by a page, or derived from what is buffered when it has not been.
  double Duration() const;
  void SetDuration(double seconds);

  // The total across every buffer, which is what the quota is enforced against -- per *source*, not
  // per buffer, because a page with an audio and a video buffer can otherwise hold twice the limit.
  std::size_t BytesHeld() const;

  std::vector<std::string_view> TakeEvents();

  // How many bytes one MediaSource may hold. ADR 0028 §3's quota, and the number is a decision:
  // 150MB is around two minutes of 1080p at 10Mbps, against the ten to thirty seconds a player
  // actually buffers ahead. Generous enough that no correct player hits it, small enough that a page
  // appending in a loop is stopped in under a second of wall time rather than after it has taken the
  // machine's memory.
  static constexpr std::size_t kQuotaBytes = 150u * 1024u * 1024u;

  // How many source buffers one MediaSource may have. A real stream has two -- audio and video --
  // and a bound exists because `addSourceBuffer` is callable in a loop.
  static constexpr std::size_t kMaxBuffers = 8;

 private:
  ReadyState state_ = ReadyState::Closed;
  std::vector<std::unique_ptr<SourceBufferState>> buffers_;
  std::vector<std::string_view> events_;
  double duration_ = 0.0;
  bool duration_set_ = false;
};

}  // namespace microbrowser::media

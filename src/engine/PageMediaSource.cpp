// MSE, from the page's side.
//
// ADR 0028 §3, session 28. Its own translation unit for the reason PageMedia.cpp is one, and the seam
// is the same: everything here is a lookup plus a call, and everything that *decides* anything is in
// `src/media` where it can be tested without a document.
//
// Two properties this file exists to hold:
//
//   * **An id resolves to nothing when the thing it named is gone.** A page can keep a `SourceBuffer`
//     past its `MediaSource`'s close, and `MediaElements::Buffer` answers null for that -- so every
//     method below is a null check away from being a use-after-free, and the null check is the whole
//     of it.
//   * **The quota is asked of the *source*, not the buffer.** A page with an audio buffer and a video
//     buffer would otherwise hold twice the limit, which is the obvious way around a per-buffer quota.

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bindings/Media.h"
#include "dom/Node.h"
#include "engine/Page.h"
#include "media/CodecId.h"
#include "media/MediaSourceState.h"

namespace microbrowser::engine {

std::uint64_t Page::CreateMediaSource() { return media_.CreateSource(); }

std::string Page::CreateObjectUrl(std::uint64_t source_id) {
  return media_.CreateObjectUrl(source_id);
}

void Page::RevokeObjectUrl(const std::string& url) { media_.RevokeObjectUrl(url); }

bool Page::AttachMediaSource(dom::Element& element, const std::string& url) {
  const std::uint64_t id = media_.SourceForUrl(url);
  media::MediaSourceState* source = media_.Source(id);
  if (source == nullptr) {
    return false;
  }
  // Attaching is what opens a source -- not construction. That is the specification's rule and it is
  // load-bearing: a page appends only after `sourceopen`, so a source that opened on construction
  // would let a page append before an element existed to play it.
  //
  // `BeginLoad` is deliberately *not* here. youtube points `video.src` at a fresh MediaSource before
  // it has any SourceBuffers; wiping `readyState` on that speculative attach made every successful
  // SABR session look empty the moment the player allocated the next source. The load starts when
  // the first `addSourceBuffer` commits the new source (see `AddSourceBuffer`).
  //
  // Leaving `NETWORK_NO_SOURCE` after a successful attach is wrong for a different reason: youtube
  // calls `play()` before the first buffer, and `NotSupportedError` from that transient state is
  // recorded as `fmt.unplayable` even though MSE later buffers the whole video (TD-0020).
  media_.AttachSource(element, id);
  source->Attach();
  if (media::MediaState* state = MediaStateFor(element)) {
    state->ResourceSelected();
  }
  return true;
}

std::uint64_t Page::SourceForObjectUrl(const std::string& url) const {
  return media_.SourceForUrl(url);
}

int Page::SourceReadyState(std::uint64_t source_id) const {
  const media::MediaSourceState* source = media_.Source(source_id);
  return source == nullptr ? 0 : static_cast<int>(source->State());
}

double Page::SourceDuration(std::uint64_t source_id) const {
  const media::MediaSourceState* source = media_.Source(source_id);
  return source == nullptr ? 0.0 : source->Duration();
}

void Page::SetSourceDuration(std::uint64_t source_id, double seconds) {
  if (media::MediaSourceState* source = media_.Source(source_id)) {
    source->SetDuration(seconds);
  }
}

void Page::EndOfStream(std::uint64_t source_id) {
  if (media::MediaSourceState* source = media_.Source(source_id)) {
    source->EndOfStream();
  }
}

std::uint64_t Page::AddSourceBuffer(std::uint64_t source_id, const std::string& mime_type,
                                    bindings::MediaController::AddBufferError& error) {
  error = bindings::MediaController::AddBufferError::None;
  // **A source id of zero is the type check on its own**, which is how `MediaSource.isTypeSupported`
  // is answered without creating anything -- and therefore how it cannot disagree with what
  // `addSourceBuffer` will do. One allowlist, asked twice.
  if (!media::IsSupportedMediaSourceType(mime_type)) {
    error = bindings::MediaController::AddBufferError::NotSupported;
    return 0;
  }
  media::MediaSourceState* source = media_.Source(source_id);
  if (source == nullptr) {
    error = bindings::MediaController::AddBufferError::InvalidState;
    return 0;
  }
  media::SourceBufferState* buffer = source->AddSourceBuffer(mime_type);
  if (buffer == nullptr) {
    // The type already passed, so this is the source not being open or being full.
    error = bindings::MediaController::AddBufferError::InvalidState;
    return 0;
  }
  // First buffer on this source is when the resource is committed. Until then a speculative
  // `video.src = createObjectURL(new MediaSource())` must not wipe a working readyState.
  if (source->BufferCount() == 1) {
    if (dom::Element* element = media_.ElementForSource(source_id)) {
      if (media::MediaState* state = MediaStateFor(*element)) {
        state->BeginLoad();
        FlushMediaEvents(*element);
      }
    }
  }
  return media_.RegisterBuffer(source_id, buffer);
}

void Page::RemoveSourceBuffer(std::uint64_t source_id, std::uint64_t buffer_id) {
  media::MediaSourceState* source = media_.Source(source_id);
  media::SourceBufferState* buffer = media_.Buffer(buffer_id);
  if (source != nullptr && buffer != nullptr) {
    // Decoders hold raw pointers into the buffer's tracks and samples. Drop
    // them before the unique_ptr erase below frees that memory.
    video_.DetachBuffer(buffer);
    source->RemoveSourceBuffer(buffer);
  }
}

int Page::AppendToSourceBuffer(std::uint64_t buffer_id, std::string_view bytes) {
  media::SourceBufferState* buffer = media_.Buffer(buffer_id);
  if (buffer == nullptr) {
    return static_cast<int>(media::AppendResult::NotOpen);
  }
  const std::uint64_t source_id = media_.SourceOfBuffer(buffer_id);
  media::MediaSourceState* source = media_.Source(source_id);
  if (source == nullptr || source->State() == media::MediaSourceState::ReadyState::Closed) {
    return static_cast<int>(media::AppendResult::NotOpen);
  }
  // An append while `ended` reopens the source, which is the specification's rule and what a live
  // stream does after a lull. Refusing would be the obvious reading of "ended" and it would break
  // every player that calls `endOfStream` optimistically.
  source->ReopenForAppend();
  // The quota that is left across the *whole source*, so two buffers cannot each hold the limit. It is
  // computed rather than passed because only this layer can see both buffers.
  const std::size_t held = source->BytesHeld();
  const std::size_t remaining = held >= media::MediaSourceState::kQuotaBytes
                                    ? 0
                                    : media::MediaSourceState::kQuotaBytes - held;
  const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(bytes.data()),
                                        bytes.size());
  const media::AppendResult result = buffer->Append(span, buffer->BytesHeld() + remaining);
  // Point the element at the source that is receiving data. A player that swapped `video.src` to a
  // newer MediaSource while still appending to this one must see `buffered`/`readyState` on these
  // buffers -- otherwise it keeps recreating empty sources forever.
  if (result == media::AppendResult::Ok) {
    if (dom::Element* element = media_.RebindElementToSource(source_id)) {
      media_.SetPlaybackSource(*element, source_id);
    }
  } else if (media_.ElementForSource(source_id) == nullptr) {
    (void)media_.RebindElementToSource(source_id);
  }
  // A successful append may have made enough data available to play. Asked here rather than inside the
  // buffer because readiness is the *element's* and one source can feed only one element.
  // Events are *not* flushed here: see `FlushMediaEventsForBuffer`, reached after updateend delivery.
  UpdateMediaReadinessFromSource(source_id);
  return static_cast<int>(result);
}

void Page::RemoveFromSourceBuffer(std::uint64_t buffer_id, double start, double end) {
  if (media::SourceBufferState* buffer = media_.Buffer(buffer_id)) {
    buffer->Remove(start, end);
  }
}

void Page::AbortSourceBuffer(std::uint64_t buffer_id) {
  if (media::SourceBufferState* buffer = media_.Buffer(buffer_id)) {
    buffer->Abort();
  }
}

bindings::MediaController::AddBufferError Page::ChangeSourceBufferType(
    std::uint64_t buffer_id, const std::string& mime_type) {
  media::SourceBufferState* buffer = media_.Buffer(buffer_id);
  if (buffer == nullptr) {
    return bindings::MediaController::AddBufferError::InvalidState;
  }
  if (!media::IsSupportedMediaSourceType(mime_type)) {
    return bindings::MediaController::AddBufferError::NotSupported;
  }
  if (!buffer->ChangeType(mime_type)) {
    // Supported type but mid-append (or otherwise refused).
    return bindings::MediaController::AddBufferError::InvalidState;
  }
  return bindings::MediaController::AddBufferError::None;
}

void Page::SetTimestampOffset(std::uint64_t buffer_id, double seconds) {
  if (media::SourceBufferState* buffer = media_.Buffer(buffer_id)) {
    buffer->SetTimestampOffset(seconds);
  }
}

double Page::TimestampOffset(std::uint64_t buffer_id) const {
  const media::SourceBufferState* buffer = media_.Buffer(buffer_id);
  return buffer == nullptr ? 0.0 : buffer->TimestampOffset();
}

void Page::SetAppendWindow(std::uint64_t buffer_id, double start, double end) {
  if (media::SourceBufferState* buffer = media_.Buffer(buffer_id)) {
    buffer->SetAppendWindow(start, end);
  }
}

bool Page::SourceBufferUpdating(std::uint64_t buffer_id) const {
  const media::SourceBufferState* buffer = media_.Buffer(buffer_id);
  return buffer != nullptr && buffer->Updating();
}

std::vector<double> Page::SourceBufferBuffered(std::uint64_t buffer_id) const {
  std::vector<double> flat;
  const media::SourceBufferState* buffer = media_.Buffer(buffer_id);
  if (buffer == nullptr) {
    return flat;
  }
  // Flattened here rather than at the seam, so that what crosses is a vector of doubles and nothing
  // else. `TimeRanges` is an index-based API on the page's side anyway.
  for (const media::BufferedRanges::Range& range : buffer->Buffered().Ranges()) {
    flat.push_back(range.start);
    flat.push_back(range.end);
  }
  return flat;
}

std::vector<double> Page::MediaBuffered(const dom::Element& element) const {
  std::vector<double> flat;
  auto flatten = [&](std::uint64_t source_id) {
    flat.clear();
    const media::MediaSourceState* source = media_.Source(source_id);
    if (source == nullptr) {
      return;
    }
    media::BufferedRanges merged;
    for (std::size_t i = 0; i < source->BufferCount(); ++i) {
      const media::SourceBufferState* buffer = source->BufferAt(i);
      if (buffer == nullptr) {
        continue;
      }
      for (const media::BufferedRanges::Range& range : buffer->Buffered().Ranges()) {
        merged.Add(range.start, range.end);
      }
    }
    for (const media::BufferedRanges::Range& range : merged.Ranges()) {
      flat.push_back(range.start);
      flat.push_back(range.end);
    }
  };
  // Prefer the source that last received an append when the attached URL is an empty speculative
  // MediaSource the player has not filled yet.
  flatten(media_.PlaybackSourceOf(element));
  if (!flat.empty()) {
    return flat;
  }
  flatten(media_.SourceOf(element));
  return flat;
}

bool Page::IsLiveSourceBuffer(std::uint64_t buffer_id) const {
  return media_.Buffer(buffer_id) != nullptr;
}

std::vector<std::string> Page::TakeSourceBufferEvents(std::uint64_t buffer_id) {
  std::vector<std::string> out;
  if (media::SourceBufferState* buffer = media_.Buffer(buffer_id)) {
    for (const std::string_view type : buffer->TakeEvents()) {
      out.emplace_back(type);
    }
  }
  return out;
}

std::vector<std::string> Page::TakeMediaSourceEvents(std::uint64_t source_id) {
  std::vector<std::string> out;
  if (media::MediaSourceState* source = media_.Source(source_id)) {
    for (const std::string_view type : source->TakeEvents()) {
      out.emplace_back(type);
    }
  }
  return out;
}

void Page::UpdateMediaReadinessFromSource(std::uint64_t source_id) {
  media::MediaSourceState* source = media_.Source(source_id);
  dom::Element* element = media_.ElementForSource(source_id);
  if (source == nullptr || element == nullptr) {
    return;
  }
  media::MediaState* state = MediaStateFor(*element);
  if (state == nullptr) {
    return;
  }
  // **This is the one place MSE and the element's state machine meet, and it is one direction.** The
  // source reports what it holds; the state machine decides what that means. `MediaState` climbs its
  // ladder from a single number -- seconds buffered ahead of the current position -- which is what
  // keeps the ladder monotone and explicable, and a source that reached into it would be a second
  // thing deciding readiness.
  const double at = state->CurrentTime();
  double ahead = 0.0;
  bool any_metadata = false;
  for (std::size_t i = 0; i < source->BufferCount(); ++i) {
    const media::SourceBufferState* buffer = source->BufferAt(i);
    if (buffer == nullptr) {
      continue;
    }
    any_metadata = any_metadata || buffer->HasInitSegment();
    // The end of the range containing the current position, which is how far playback can run without
    // stalling. A buffer that does not contain the position contributes nothing -- not its largest
    // end, which would be a promise about data on the far side of a gap.
    for (const media::BufferedRanges::Range& range : buffer->Buffered().Ranges()) {
      // Seconds of media at or after the playhead. Requiring `at` to fall *inside* a range made
      // youtube look unbuffered whenever the first WebM cluster started a few frames after 0.
      if (range.end > at) {
        ahead = std::max(ahead, range.end - at);
      }
    }
  }
  if (!any_metadata) {
    return;  // No initialization segment yet, so the duration is not known and nothing can be said.
  }
  // The *smallest* ahead across buffers would be the honest number for a stream with audio and video,
  // because playback stalls on whichever runs out first. Taking the largest here is a deliberate
  // simplification and it is written down rather than left to be found: with one buffer the two are
  // the same; with two they are not, and that is the next sharpening.
  state->MetadataArrived(source->Duration());
  state->BufferedAhead(ahead);
  if (source->State() == media::MediaSourceState::ReadyState::Ended && ahead <= 0.0) {
    state->ReachedEnd();
  }
  // Do not FlushMediaEvents here -- callers that must deliver synchronously (attach, play) flush
  // themselves; appendBuffer flushes from the updateend microtask.
}

void Page::FlushMediaEventsForBuffer(std::uint64_t buffer_id) {
  const std::uint64_t source_id = media_.SourceOfBuffer(buffer_id);
  if (dom::Element* element = media_.ElementForSource(source_id)) {
    FlushMediaEvents(*element);
  }
}

}  // namespace microbrowser::engine

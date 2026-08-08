#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::bindings {

// What `HTMLMediaElement`'s properties and methods are answered through.
//
// ADR 0028 §1, and the same inversion the other seams use: declared here, implemented by
// `src/engine`. This module may see `util`, `js`, `dom` and `html` -- **not `media`** -- so
// nothing here can name `media::MediaState`, hold a ring buffer, or reach a device. The state
// machine is the specification's and it lives on the far side; this side only asks it questions.
//
// The questions are per *element* rather than per source object, because that is what the API
// is: `video.currentTime` is a property of the element, and two `<video>` elements pointing at
// one file are two independent players. An element pointer is the key on both sides.
class MediaController {
 public:
  // Why `play()` was refused. Three answers rather than a bool because a page handles them
  // differently -- `NotAllowedError` means show a play button and `NotSupportedError` means show
  // an error -- and collapsing them makes a video that could have played look broken.
  enum class PlayResult : std::uint8_t { Started, NotAllowed, NotSupported };

  MediaController() = default;
  MediaController(const MediaController&) = delete;
  MediaController& operator=(const MediaController&) = delete;
  virtual ~MediaController() = default;

  virtual PlayResult Play(dom::Element& element) = 0;
  virtual void Pause(dom::Element& element) = 0;
  // A seek. The position moves immediately -- which is what the API promises and what a scrubber
  // depends on -- and the readiness drop that follows is the state machine's business.
  virtual void Seek(dom::Element& element, double seconds) = 0;
  virtual void SetMuted(dom::Element& element, bool muted) = 0;
  virtual void SetVolume(dom::Element& element, double volume) = 0;

  // The readable state. Doubles and ints rather than enums this module declares, because the
  // *numbers* are the API -- a page writes `video.readyState >= 3` -- and a second enumeration
  // of the same four values is one more place for them to stop matching.
  virtual double CurrentTime(const dom::Element& element) const = 0;
  virtual double Duration(const dom::Element& element) const = 0;
  virtual double Volume(const dom::Element& element) const = 0;
  virtual int ReadyState(const dom::Element& element) const = 0;
  virtual int NetworkState(const dom::Element& element) const = 0;
  virtual bool Paused(const dom::Element& element) const = 0;
  virtual bool Ended(const dom::Element& element) const = 0;
  virtual bool Muted(const dom::Element& element) const = 0;
  // Intrinsic decoded size. Zero until a frame has arrived; pages size the
  // element from these (youtube's player chrome reads them after `loadedmetadata`).
  virtual int VideoWidth(const dom::Element& element) const = 0;
  virtual int VideoHeight(const dom::Element& element) const = 0;
  // Whether this element is a media element at all. Asked before anything else, so that
  // `document.body.play` is undefined rather than a function that answers about nothing.
  virtual bool IsMedia(const dom::Element& element) const = 0;

  // --- MSE (ADR 0028 §3) -----------------------------------------------------------------------
  //
  // A `MediaSource` is named by an opaque id rather than by a pointer, and that is the same decision
  // the socket and storage seams made for the same reason: an id is a number a page cannot forge into
  // a pointer, and the table it indexes lives on the far side of the seam with the document. An id of
  // zero is "no such source", which is what a page holding a revoked object URL gets.
  //
  // `mime_type` and the codec allowlist are checked on the far side too. Nothing here can name
  // `media::CodecId`, so this module cannot form an opinion about whether a stream is playable --
  // which is the property that keeps a *second* answer to "can we play this" from existing.

  // `new MediaSource()`. Returns its id.
  virtual std::uint64_t CreateMediaSource() = 0;
  // `URL.createObjectURL(source)` -- the id becomes a `blob:` URL string, registered against this
  // document. Empty when the id is not a live source.
  virtual std::string CreateObjectUrl(std::uint64_t source_id) = 0;
  // `URL.revokeObjectURL(url)`. After it the URL names nothing, which is what a page relies on to
  // stop a source being attachable a second time.
  virtual void RevokeObjectUrl(const std::string& url) = 0;

  // `video.src = blobUrl` resolved: attaches whatever source that URL names to this element, which is
  // what moves the source from "closed" to "open". False when the URL names nothing.
  virtual bool AttachMediaSource(dom::Element& element, const std::string& url) = 0;
  // Which source an object URL names, so that the binding layer can find the wrapper to fire
  // `sourceopen` at. Zero when the URL names nothing, which is what a revoked URL does.
  virtual std::uint64_t SourceForObjectUrl(const std::string& url) const = 0;

  // `readyState` on the source: 0 closed, 1 open, 2 ended. The numbers rather than an enum, for the
  // reason `MediaController::ReadyState` gives.
  virtual int SourceReadyState(std::uint64_t source_id) const = 0;
  virtual double SourceDuration(std::uint64_t source_id) const = 0;
  virtual void SetSourceDuration(std::uint64_t source_id, double seconds) = 0;
  virtual void EndOfStream(std::uint64_t source_id) = 0;

  // `addSourceBuffer`. Returns a buffer id, or zero when the source is not open, the type is not
  // supported, or there are already too many -- three refusals the caller turns into two different
  // exceptions, which is why the reason comes back separately.
  enum class AddBufferError : std::uint8_t { None, NotSupported, InvalidState };
  virtual std::uint64_t AddSourceBuffer(std::uint64_t source_id, const std::string& mime_type,
                                        AddBufferError& error) = 0;
  virtual void RemoveSourceBuffer(std::uint64_t source_id, std::uint64_t buffer_id) = 0;

  // `appendBuffer`. The bytes are copied on the far side, because the ArrayBuffer they came from is
  // the page's and can be neutered or resized before the append is looked at again.
  //
  // The result is `AppendResult`'s numbering, kept as an int for the reason above: 0 ok, 1 quota,
  // 2 parse failed, 3 not open, 4 already updating.
  virtual int AppendToSourceBuffer(std::uint64_t buffer_id, std::string_view bytes) = 0;
  virtual void RemoveFromSourceBuffer(std::uint64_t buffer_id, double start, double end) = 0;
  virtual void AbortSourceBuffer(std::uint64_t buffer_id) = 0;
  virtual void SetTimestampOffset(std::uint64_t buffer_id, double seconds) = 0;
  virtual double TimestampOffset(std::uint64_t buffer_id) const = 0;
  virtual void SetAppendWindow(std::uint64_t buffer_id, double start, double end) = 0;
  virtual bool SourceBufferUpdating(std::uint64_t buffer_id) const = 0;

  // `buffered`, as a flat list of alternating starts and ends. Flat rather than a pair list because
  // this crosses a module boundary and a vector of doubles is the narrowest thing that carries it --
  // and because `TimeRanges` is an index-based API on the page's side anyway.
  virtual std::vector<double> SourceBufferBuffered(std::uint64_t buffer_id) const = 0;

  // `HTMLMediaElement.buffered` — the union of every attached SourceBuffer's
  // ranges. Flat start/end pairs, same shape as `SourceBufferBuffered`. Empty
  // when the element is not attached to a MediaSource (a plain `src=` file has
  // no MSE ranges here yet).
  virtual std::vector<double> MediaBuffered(const dom::Element& element) const = 0;

  // Whether an id still names something. A page can hold a `SourceBuffer` after its `MediaSource`
  // closed, and every method on it must then throw `InvalidStateError` rather than answer.
  virtual bool IsLiveSourceBuffer(std::uint64_t buffer_id) const = 0;

  // The events each object produced, taken. Same contract as everything else that crosses this seam:
  // the far side decides *which* events happened and this side decides how they are delivered, so
  // neither can be wrong about the other's job.
  virtual std::vector<std::string> TakeSourceBufferEvents(std::uint64_t buffer_id) = 0;
  virtual std::vector<std::string> TakeMediaSourceEvents(std::uint64_t source_id) = 0;

  // Deliver any media-element readiness events queued by a SourceBuffer append (`canplay`, …).
  // Called from the same microtask that fires `updateend`, never from inside `appendBuffer` itself:
  // a sync flush runs `DispatchEventTo` → `DrainMicrotasks`, which would re-enter deferred
  // `updateend` handlers before `appendBuffer` returns.
  virtual void FlushMediaEventsForBuffer(std::uint64_t buffer_id) = 0;
};

}  // namespace microbrowser::bindings

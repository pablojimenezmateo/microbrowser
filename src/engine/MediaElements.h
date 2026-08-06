#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "media/MediaSourceState.h"
#include "media/MediaState.h"

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::engine {

// One state machine per media element, and the volume beside it.
//
// ADR 0028 §1. This exists because the architecture lint said so, and the lint was right: these
// members were on `Page`, which pushed it to holding members from five modules -- and the message
// is "split the coordination rather than widening the class". The split is real rather than
// cosmetic. What `Page` does with media is *coordinate*: it reads the document's user activation,
// asks this for a state machine, and fires the events at script. What this does is own the map.
//
// Created on first use rather than per element in the tree: a feed with fifty `<video>` elements
// gets fifty state machines only if a page touches fifty, and an untouched element reads as the
// specification's initial state anyway.
class MediaElements {
 public:
  // Null for an element that is not `<video>` or `<audio>`. The caller decides what to do about
  // that -- the binding throws, the engine ignores -- because "not a media element" means
  // different things to the two of them.
  media::MediaState* For(const dom::Element& element, bool has_source);
  const media::MediaState* Find(const dom::Element& element) const;

  // Volume is here rather than in `MediaState` because it changes nothing about readiness or
  // playback: it is a number the output stage reads, and putting it in the state machine would
  // mean a transition table with a case for "the user moved a slider".
  void SetVolume(const dom::Element& element, double volume);
  double Volume(const dom::Element& element) const;

  // A navigation. The elements are gone, so their state machines are too -- and a machine that
  // outlived its element would answer about a `<video>` nobody can see.
  void Clear();

  // --- MSE (ADR 0028 §3) ------------------------------------------------------------------------
  //
  // The `MediaSource` and `SourceBuffer` tables, and the object URL registry, live here for the same
  // reason the element states do: they are per *document*, they die with a navigation, and `Page` is
  // at its member budget. An id is the only handle that crosses to `src/bindings`.
  //
  // Ids are never reused. A monotonic counter costs eight bytes and removes a whole class of bug: a
  // page holding a stale `SourceBuffer` whose id had been handed to a new buffer would append into
  // somebody else's stream, and every check for liveness would pass.
  std::uint64_t CreateSource();
  media::MediaSourceState* Source(std::uint64_t id);
  const media::MediaSourceState* Source(std::uint64_t id) const;

  // The object URL registry. Per-document and revoked with it, which is what ADR 0028 §3 asks for:
  // a `blob:` URL that outlived its document would be a name for a source belonging to a page that
  // is gone.
  std::string CreateObjectUrl(std::uint64_t source_id);
  void RevokeObjectUrl(const std::string& url);
  std::uint64_t SourceForUrl(const std::string& url) const;

  // A `SourceBuffer` id resolves to a buffer *and* the source that owns it, because every method on a
  // buffer has to check the source's state and a buffer does not know its own owner.
  media::SourceBufferState* Buffer(std::uint64_t id);
  const media::SourceBufferState* Buffer(std::uint64_t id) const;
  std::uint64_t RegisterBuffer(std::uint64_t source_id, media::SourceBufferState* buffer);
  void ForgetBuffersOf(std::uint64_t source_id);

  // Which source an element is attached to, and the reverse. Zero for an element playing a plain
  // `<video src="…mp4">`, which is most of them.
  void AttachSource(dom::Element& element, std::uint64_t source_id);
  std::uint64_t SourceOf(const dom::Element& element) const;
  // The reverse, which the readiness update needs: an append happened on a buffer and the element that
  // has to hear about it is found from the source. Null when no element is attached, which is a source
  // a page built and never used.
  dom::Element* ElementForSource(std::uint64_t source_id) const;
  std::uint64_t SourceOfBuffer(std::uint64_t buffer_id) const;

 private:
  std::map<const dom::Element*, media::MediaState> states_;
  std::map<const dom::Element*, double> volumes_;
  std::map<std::uint64_t, media::MediaSourceState> sources_;
  // Buffer id -> (source id, pointer). The pointer is owned by the source, so it is only valid while
  // the source is -- which is why `Buffer` looks the source up first and returns null when it is gone
  // rather than dereferencing what it remembered.
  struct BufferRef {
    std::uint64_t source_id = 0;
    media::SourceBufferState* buffer = nullptr;
  };
  std::map<std::uint64_t, BufferRef> buffers_;
  std::map<std::string, std::uint64_t> object_urls_;
  // Keyed by the element, and the pointer is non-const because the readiness update has to call the
  // element's own state machine. Cleared on navigation with everything else, so it never outlives what
  // it points at.
  std::map<dom::Element*, std::uint64_t> attached_;
  std::uint64_t next_id_ = 0;
};

}  // namespace microbrowser::engine

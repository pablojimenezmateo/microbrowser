#include "engine/MediaElements.h"

#include <iterator>

#include "util/PerformanceCounters.h"

#include <utility>

#include "dom/Node.h"

namespace microbrowser::engine {

namespace {

bool IsMediaTag(const dom::Element& element) {
  return element.TagName() == "video" || element.TagName() == "audio";
}

}  // namespace

media::MediaState* MediaElements::For(const dom::Element& element, bool has_source) {
  if (!IsMediaTag(element)) {
    return nullptr;
  }
  const auto found = states_.find(&element);
  if (found != states_.end()) {
    return &found->second;
  }
  media::MediaState fresh;
  if (has_source) {
    fresh.BeginLoad();
  } else {
    // No source is `NO_SOURCE` immediately, which is what makes `play()` on an empty element a
    // `NotSupportedError` rather than a promise that never settles.
    fresh.FailNoSource();
  }
  return &states_.emplace(&element, std::move(fresh)).first->second;
}

const media::MediaState* MediaElements::Find(const dom::Element& element) const {
  const auto found = states_.find(&element);
  return found == states_.end() ? nullptr : &found->second;
}

void MediaElements::SetVolume(const dom::Element& element, double volume) {
  // Clamped rather than refused: `video.volume = 2` is a page overreaching, not a page in error,
  // and the specification clamps. A throw would break a page that works everywhere else.
  volumes_[&element] = volume < 0.0 ? 0.0 : (volume > 1.0 ? 1.0 : volume);
}

double MediaElements::Volume(const dom::Element& element) const {
  const auto found = volumes_.find(&element);
  return found == volumes_.end() ? 1.0 : found->second;
}

void MediaElements::Clear() {
  states_.clear();
  volumes_.clear();
  // The MSE tables go too, and that is ADR 0028 §3's "revoked with the document": a `blob:` URL or a
  // `MediaSource` that outlived its navigation would be a name for a stream belonging to a page that
  // is gone -- and a page can hold up to the quota in one, so leaking them leaks 150MB per navigation.
  //
  // `next_id_` is deliberately *not* reset. Ids are never reused across a document either, because a
  // stale wrapper from the old page would otherwise resolve to a new page's buffer.
  sources_.clear();
  buffers_.clear();
  object_urls_.clear();
  attached_.clear();
}


// --- MSE (ADR 0028 §3) --------------------------------------------------------------------------

std::uint64_t MediaElements::CreateSource() {
  const std::uint64_t id = ++next_id_;
  sources_.emplace(id, media::MediaSourceState{});
  return id;
}

media::MediaSourceState* MediaElements::Source(std::uint64_t id) {
  const auto found = sources_.find(id);
  return found == sources_.end() ? nullptr : &found->second;
}

const media::MediaSourceState* MediaElements::Source(std::uint64_t id) const {
  const auto found = sources_.find(id);
  return found == sources_.end() ? nullptr : &found->second;
}

std::string MediaElements::CreateObjectUrl(std::uint64_t source_id) {
  if (Source(source_id) == nullptr) {
    return std::string();
  }
  // A `blob:` URL, and the opaque part is a counter rather than random.
  //
  // **That is a decision and it is defensible here in a way it would not be for a Blob.** What this
  // URL names is a `MediaSource` in *this* document's own table; there is no cross-document lookup,
  // no storage, and no way for another origin to hand this string anywhere it would be resolved. So
  // its unguessability buys nothing, and a counter is one fewer thing that has to be seeded. A Blob
  // URL, which a page can put in an `<img>` and which other code may treat as a capability, would be
  // a different question -- and there is no Blob in this browser, which is why the question is not
  // being answered here.
  const std::string url = "blob:microbrowser/" + std::to_string(++next_id_);
  object_urls_[url] = source_id;
  util::AddPerformanceCounter(util::PerfCounterId::MediaObjectUrlsCreated);
  return url;
}

void MediaElements::RevokeObjectUrl(const std::string& url) { object_urls_.erase(url); }

std::uint64_t MediaElements::SourceForUrl(const std::string& url) const {
  const auto found = object_urls_.find(url);
  return found == object_urls_.end() ? 0 : found->second;
}

std::uint64_t MediaElements::RegisterBuffer(std::uint64_t source_id,
                                            media::SourceBufferState* buffer) {
  if (buffer == nullptr) {
    return 0;
  }
  const std::uint64_t id = ++next_id_;
  buffers_[id] = BufferRef{source_id, buffer};
  return id;
}

media::SourceBufferState* MediaElements::Buffer(std::uint64_t id) {
  const auto found = buffers_.find(id);
  if (found == buffers_.end()) {
    return nullptr;
  }
  // **The source is looked up before the pointer is used.** The buffer is owned by the source, so a
  // remembered pointer is only valid while the source is -- and a closed source has already destroyed
  // it. Returning null here is what turns "the page kept a SourceBuffer too long" into an
  // `InvalidStateError` instead of a use-after-free.
  const media::MediaSourceState* source = Source(found->second.source_id);
  if (source == nullptr) {
    return nullptr;
  }
  // And the buffer must still be *on* that source: `removeSourceBuffer` destroys one without closing
  // the source, which is the other way a remembered pointer goes stale.
  media::MediaSourceState* mutable_source = Source(found->second.source_id);
  for (std::size_t i = 0; i < mutable_source->BufferCount(); ++i) {
    if (mutable_source->BufferAt(i) == found->second.buffer) {
      return found->second.buffer;
    }
  }
  return nullptr;
}

const media::SourceBufferState* MediaElements::Buffer(std::uint64_t id) const {
  const auto found = buffers_.find(id);
  if (found == buffers_.end()) {
    return nullptr;
  }
  const media::MediaSourceState* source = Source(found->second.source_id);
  if (source == nullptr) {
    return nullptr;
  }
  // The const path cannot walk `BufferAt`, which is non-const, so it answers on the source's
  // liveness alone -- one condition weaker than the mutable path, and the difference only shows for a
  // buffer removed from a live source. Every *mutating* call goes through the mutable path, so the
  // weaker check can only make a read answer about a buffer that is about to be reported dead.
  return found->second.buffer;
}

void MediaElements::ForgetBuffersOf(std::uint64_t source_id) {
  for (auto it = buffers_.begin(); it != buffers_.end();) {
    it = it->second.source_id == source_id ? buffers_.erase(it) : std::next(it);
  }
}

void MediaElements::AttachSource(dom::Element& element, std::uint64_t source_id) {
  attached_[&element] = source_id;
}

dom::Element* MediaElements::ElementForSource(std::uint64_t source_id) const {
  for (const auto& [element, id] : attached_) {
    if (id == source_id) {
      return element;
    }
  }
  return nullptr;
}

std::uint64_t MediaElements::SourceOfBuffer(std::uint64_t buffer_id) const {
  const auto found = buffers_.find(buffer_id);
  return found == buffers_.end() ? 0 : found->second.source_id;
}

std::uint64_t MediaElements::SourceOf(const dom::Element& element) const {
  // `const_cast` on the *key* only, and only to look up: the map is keyed by a non-const pointer
  // because the readiness update calls the element's state machine, and asking "is this element
  // attached?" is a const question about a mutable key. Nothing here writes through it.
  const auto found = attached_.find(const_cast<dom::Element*>(&element));
  return found == attached_.end() ? 0 : found->second;
}

}  // namespace microbrowser::engine

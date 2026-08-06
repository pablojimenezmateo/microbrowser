#include "engine/MediaElements.h"

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
}

}  // namespace microbrowser::engine

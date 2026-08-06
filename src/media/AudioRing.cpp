#include "media/AudioRing.h"

#include <algorithm>

namespace microbrowser::media {

std::size_t AudioRing::WritableFrames() const {
  const std::size_t size = samples_.size();
  // The producer's own cursor is relaxed -- it wrote it -- and the consumer's is acquired
  // so that space it freed is really free.
  const std::size_t write = write_.load(std::memory_order_relaxed);
  const std::size_t read = read_.load(std::memory_order_acquire);
  const std::size_t used = write >= read ? write - read : size - read + write;
  const std::size_t free_samples = size - used - static_cast<std::size_t>(channels_);
  return free_samples / static_cast<std::size_t>(channels_);
}

std::size_t AudioRing::Write(std::span<const float> interleaved) {
  const std::size_t channels = static_cast<std::size_t>(channels_);
  const std::size_t offered = interleaved.size() / channels;
  const std::size_t frames = std::min(offered, WritableFrames());
  if (frames == 0) {
    return 0;
  }
  std::size_t at = write_.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < frames * channels; ++i) {
    samples_[at] = interleaved[i];
    at = at + 1 == samples_.size() ? 0 : at + 1;
  }
  // **The release.** Every sample above must be visible to the consumer before the cursor
  // that admits it. This one line is the whole synchronisation, and reversing it or making
  // it relaxed makes the consumer read samples that have not landed.
  write_.store(at, std::memory_order_release);
  return frames;
}

std::size_t AudioRing::ReadableFrames() const {
  const std::size_t size = samples_.size();
  const std::size_t write = write_.load(std::memory_order_acquire);
  const std::size_t read = read_.load(std::memory_order_relaxed);
  const std::size_t used = write >= read ? write - read : size - read + write;
  return used / static_cast<std::size_t>(channels_);
}

std::size_t AudioRing::Read(std::span<float> out) {
  const std::size_t channels = static_cast<std::size_t>(channels_);
  const std::size_t wanted = out.size() / channels;
  // The acquire that pairs with the producer's release.
  const std::size_t write = write_.load(std::memory_order_acquire);
  std::size_t at = read_.load(std::memory_order_relaxed);
  const std::size_t size = samples_.size();
  const std::size_t used = write >= at ? write - at : size - at + write;
  const std::size_t frames = std::min(wanted, used / channels);

  for (std::size_t i = 0; i < frames * channels; ++i) {
    out[i] = samples_[at];
    at = at + 1 == size ? 0 : at + 1;
  }
  // Silence for the rest. A device callback must always fill its block: a short answer is
  // a click, which is what an underrun sounds like when nobody handled it.
  for (std::size_t i = frames * channels; i < wanted * channels; ++i) {
    out[i] = 0.0f;
  }
  if (frames < wanted) {
    underruns_.fetch_add(1, std::memory_order_relaxed);
  }
  read_.store(at, std::memory_order_release);
  return frames;
}

}  // namespace microbrowser::media

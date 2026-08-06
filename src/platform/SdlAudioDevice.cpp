#include "platform/SdlAudioDevice.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>

#include "media/AudioRing.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::platform {

namespace {

// 48kHz stereo float: what a decoder produces and what every device on every platform this
// targets accepts. Asking for the device's own preference instead would mean resampling,
// which is a codec-adjacent problem and belongs with the decoder rather than here.
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

// How much the callback moves at once. A fixed on-stack buffer, because the callback may not
// allocate -- see the header for why that is a rule rather than a preference.
constexpr std::size_t kCallbackFrames = 1024;

void SDLCALL FeedDevice(void* userdata, SDL_AudioStream* stream, int additional, int) {
  auto* ring = static_cast<media::AudioRing*>(userdata);
  if (ring == nullptr || additional <= 0) {
    return;
  }
  // Whole frames only: handing SDL a partial frame would put one channel a sample ahead of
  // the other for the rest of the stream.
  const std::size_t wanted_frames =
      static_cast<std::size_t>(additional) / (sizeof(float) * static_cast<std::size_t>(kChannels));
  std::array<float, kCallbackFrames * kChannels> block{};
  std::size_t remaining = wanted_frames;
  while (remaining > 0) {
    const std::size_t chunk = remaining < kCallbackFrames ? remaining : kCallbackFrames;
    // `Read` pads with silence and counts the underrun, so this always has a whole block to
    // hand over -- which is what stops an empty ring from becoming a click.
    ring->Read(std::span<float>(block.data(), chunk * kChannels));
    SDL_PutAudioStreamData(stream, block.data(),
                           static_cast<int>(chunk * kChannels * sizeof(float)));
    remaining -= chunk;
  }
}

}  // namespace

SdlAudioDevice::~SdlAudioDevice() { Stop(); }

bool SdlAudioDevice::Start(media::AudioRing& ring) {
  if (stream_ != nullptr) {
    return true;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return false;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_F32;
  spec.channels = kChannels;
  spec.freq = kSampleRate;
  ring_ = &ring;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedDevice, &ring);
  if (stream_ == nullptr) {
    // No device is not an error: a machine with no sound card, a container, a test host. A
    // browser that plays video silently is better than one that refuses to play it.
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    ring_ = nullptr;
    util::AddPerformanceCounter(util::PerfCounterId::AudioDeviceUnavailable);
    return false;
  }
  sample_rate_ = kSampleRate;
  channels_ = kChannels;
  SDL_ResumeAudioStreamDevice(stream_);
  util::AddPerformanceCounter(util::PerfCounterId::AudioDevicesOpened);
  return true;
}

void SdlAudioDevice::Stop() {
  if (stream_ == nullptr) {
    return;
  }
  // Destroying the stream closes the device it was opened with and **joins**: SDL guarantees
  // no further callback after this returns, which is what makes the ring safe to destroy
  // afterwards. A caller that reversed the two would have a use-after-free this object
  // cannot prevent, which is why the interface says so.
  SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
  ring_ = nullptr;
  sample_rate_ = 0;
  channels_ = 0;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

std::uint64_t SdlAudioDevice::QueuedFrames() const {
  if (stream_ == nullptr) {
    return 0;
  }
  const int queued = SDL_GetAudioStreamQueued(stream_);
  if (queued <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(queued) /
         (sizeof(float) * static_cast<std::uint64_t>(channels_ > 0 ? channels_ : kChannels));
}

}  // namespace microbrowser::platform

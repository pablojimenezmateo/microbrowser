#pragma once

#include <cstdint>

#include "media/AudioSink.h"

struct SDL_AudioStream;

namespace microbrowser::platform {

// The audio device, and the only place in this browser that knows one exists.
//
// ADR 0028 §4. It implements `media::AudioSink`, which is declared in `src/media` because
// `src/engine` may not name this module -- so the engine asks for playback through the
// interface and never learns what SDL is.
//
// **The thread is SDL's, and that is the design rather than a compromise.** SDL3 calls a
// callback on its own audio thread when the device wants samples; the callback here drains
// the ring and does nothing else -- no allocation, no logging, no lock. What that buys is
// exactly ADR 0028 §4's ownership statement without a thread of our own to join: opening the
// device creates the thread, `Stop` destroys it, and "no audio thread when nothing is
// playing" becomes "no open device", which is a state this object can be asked about.
//
// What the callback must never do is worth stating as a rule, because every one of these has
// been a real bug in some player: it must not allocate (the allocator may be held by the
// thread that is trying to stop it), must not take a lock (a lock in an audio callback is a
// click), must not touch a document or a decoder (it cannot see them, by module contract),
// and must always fill its whole block (a short answer is a click too -- `AudioRing::Read`
// pads with silence for that reason).
class SdlAudioDevice : public media::AudioSink {
 public:
  SdlAudioDevice() = default;
  ~SdlAudioDevice() override;

  bool Start(media::AudioRing& ring) override;
  void Stop() override;
  bool IsRunning() const override { return stream_ != nullptr; }

  // What the device asked for, once it is open. Zero when it is not: a caller that built a
  // clock from this before starting would have built one at the wrong rate.
  int SampleRate() const { return sample_rate_; }
  int Channels() const { return channels_; }

  // How many frames the device is holding, for `PlaybackClock::SetBufferedFrames` -- the
  // difference between the position written and the position heard.
  std::uint64_t QueuedFrames() const;

 private:
  SDL_AudioStream* stream_ = nullptr;
  media::AudioRing* ring_ = nullptr;
  int sample_rate_ = 0;
  int channels_ = 0;
};

}  // namespace microbrowser::platform

#pragma once

#include <cstdint>

namespace microbrowser::media {

// Where playback has got to, in the media's own time.
//
// ADR 0028 §4: "video follows the audio clock, not the other way round", and this is that
// clock. It is driven by **frames the device actually consumed** rather than by wall time,
// which is the whole point: a wall clock and an audio device drift apart, and a video
// synchronised to the wall clock drifts out of lip-sync at a rate nobody can predict.
//
// Three properties, and the first two are what a caller is allowed to rely on:
//
//   * **It never goes backwards** while playing. A seek moves it, and a seek is an explicit
//     act; anything else that moved it backwards would make a video re-present a frame it
//     had already shown.
//   * **It stops when the device stops.** A paused element's clock is frozen, not
//     "advancing more slowly", so a paused video does not eventually decide it is behind.
//   * **It reports the *presented* position**, which is behind what has been written: the
//     device holds a buffer, and reporting the written position would run video ahead of
//     the sound by exactly that much. Subtracting the latency is the one piece of
//     arithmetic here that a naive clock omits.
class PlaybackClock {
 public:
  // A sample rate of zero is refused into one: a clock that divided by it would answer
  // infinity, and every caller of a media clock divides.
  explicit PlaybackClock(int sample_rate) : sample_rate_(sample_rate > 0 ? sample_rate : 48000) {}

  int SampleRate() const { return sample_rate_; }

  // Frames the device consumed. Called from wherever the ring is drained, which is the
  // audio thread -- so this object belongs to that thread and the engine reads a *copy*.
  void FramesConsumed(std::uint64_t frames) { consumed_ += frames; }

  // How many frames sit in the device's buffer, unheard. Subtracted from the position, and
  // it is a *set* rather than an add because the device reports its own occupancy and a
  // running total of a level would be meaningless.
  void SetBufferedFrames(std::uint64_t frames) { buffered_ = frames; }

  // Seconds of media time. What a video frame's presentation timestamp is compared against.
  double CurrentTimeSeconds() const {
    const std::uint64_t presented = consumed_ > buffered_ ? consumed_ - buffered_ : 0u;
    return seek_base_seconds_ + static_cast<double>(presented) / static_cast<double>(sample_rate_);
  }

  // A seek. The frame counter restarts, because the frames consumed before a seek say
  // nothing about the position after one -- and keeping them would make the clock jump
  // forward by the whole of what had already played.
  void SeekTo(double seconds) {
    seek_base_seconds_ = seconds < 0.0 ? 0.0 : seconds;
    consumed_ = 0;
    buffered_ = 0;
  }

 private:
  int sample_rate_ = 48000;
  std::uint64_t consumed_ = 0;
  std::uint64_t buffered_ = 0;
  double seek_base_seconds_ = 0.0;
};

}  // namespace microbrowser::media

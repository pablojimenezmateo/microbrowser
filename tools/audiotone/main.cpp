// Plays a tone. The honest intermediate for ADR 0028 §4.
//
// An `<audio src="...mp3">` cannot play until there is a decoder (session 27's codec
// decision), so this is what verifies the half that exists: the ring, the device, the
// callback that drains one into the other, and the playback clock that counts what was
// heard. It is a tool rather than a test because the assertion is "it sounds like a sine
// wave", and no test can make that.
//
// What it *can* be checked against without ears: `audio.devices_opened`, the underrun count,
// and the clock's position against wall time -- which is the whole point of the clock, since
// the two drifting apart is exactly what a wall-clock-driven player gets wrong.
//
//   ./build/microbrowser/microbrowser_audiotone [seconds] [hz]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "media/AudioRing.h"
#include "media/PlaybackClock.h"
#include "platform/SdlAudioDevice.h"

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
  const double hz = argc > 2 ? std::atof(argv[2]) : 440.0;

  // A quarter of a second of buffer: enough that the feeding loop below can be lazy, small
  // enough that an underrun shows up as one rather than being hidden by depth.
  microbrowser::media::AudioRing ring(12000, 2);
  microbrowser::platform::SdlAudioDevice device;
  if (!device.Start(ring)) {
    std::fprintf(stderr, "no audio device: nothing to hear, which is not an error\n");
    return 0;
  }
  microbrowser::media::PlaybackClock clock(device.SampleRate());
  std::fprintf(stderr, "device open: %d Hz, %d channels\n", device.SampleRate(),
               device.Channels());

  const int rate = device.SampleRate();
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t generated = 0;
  const std::uint64_t total = static_cast<std::uint64_t>(seconds * rate);
  std::vector<float> block;
  while (generated < total) {
    const std::size_t room = ring.WritableFrames();
    if (room == 0) {
      // The device has not drained yet. Sleeping here is fine and is not a busy wait: this
      // is a tool's feeding loop standing in for a decoder, and a decoder is driven by the
      // engine's own turn rather than by this.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const std::size_t frames =
        static_cast<std::size_t>(std::min<std::uint64_t>(room, total - generated));
    block.clear();
    for (std::size_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(generated + i) / rate;
      // Quiet on purpose: a full-scale tone out of a browser is a bad surprise.
      const float sample = static_cast<float>(0.15 * std::sin(2.0 * 3.14159265358979 * hz * t));
      block.push_back(sample);
      block.push_back(sample);
    }
    generated += ring.Write(block);
    clock.SetFramesConsumed(ring.FramesRead());
    clock.SetBufferedFrames(device.QueuedFrames());
  }
  // Underruns *during* playback, taken before the tail. After the tone ends the device keeps
  // asking and the ring keeps padding with silence, so the trailing count is expected and
  // says nothing about whether the feeding kept up -- reporting the two together would hide
  // the number that matters behind the one that does not.
  const std::uint32_t while_playing = ring.TakeUnderruns();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  clock.SetFramesConsumed(ring.FramesRead());
  clock.SetBufferedFrames(device.QueuedFrames());
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::fprintf(stderr,
               "generated %.2fs, heard %.2fs, wall %.2fs, underruns %u while playing, %u in "
               "the tail after it ended\n",
               static_cast<double>(generated) / rate, clock.CurrentTimeSeconds(), wall,
               while_playing, ring.TakeUnderruns());
  device.Stop();
  std::fprintf(stderr, "device closed; running=%d\n", static_cast<int>(device.IsRunning()));
  return 0;
}

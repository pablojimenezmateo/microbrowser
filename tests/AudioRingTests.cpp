// The buffer the audio thread and the engine thread share.
//
// ADR 0028 §4. Two kinds of assertion here, and the second kind is the reason this file
// exists: the ordinary ones about frames in and frames out, and **one that runs two real
// threads under TSan**, because the memory ordering in `AudioRing` cannot be tested into
// existence by a single-threaded test -- a relaxed store would pass every assertion below
// it and still tear on a machine with a weaker memory model.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "media/AudioRing.h"
#include "media/PlaybackClock.h"
#include "platform/SdlAudioDevice.h"

namespace microbrowser::tests {

void RegisterAudioRingTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AudioRing/AFullBufferIsNotAnEmptyOne", [] {
    // The classic ring-buffer bug: with `write == read` meaning empty, a full buffer must
    // leave one frame unused or full and empty are the same state -- and a consumer drains
    // a full buffer as silence.
    media::AudioRing ring(4, 2);
    ExpectEqInt(static_cast<long long>(ring.CapacityFrames()), 4, "four frames");
    const std::vector<float> block(8, 0.5f);  // four stereo frames
    ExpectEqInt(static_cast<long long>(ring.Write(block)), 4, "all four fit");
    ExpectEqInt(static_cast<long long>(ring.WritableFrames()), 0, "and now nothing does");
    ExpectEqInt(static_cast<long long>(ring.ReadableFrames()), 4,
                "while all four are readable, which is what distinguishes full from empty");
  });

  AddTest(tests, "AudioRing/WritesWholeFramesOnlyAndReportsWhatFit", [] {
    // A partial write is normal: the caller keeps the rest. Refusing to write anything
    // until it all fits would stall a stream behind one oversized chunk.
    media::AudioRing ring(3, 2);
    const std::vector<float> five_frames(10, 1.0f);
    ExpectEqInt(static_cast<long long>(ring.Write(five_frames)), 3, "three of five frames");
    std::vector<float> out(4, -1.0f);
    ExpectEqInt(static_cast<long long>(ring.Read(out)), 2, "two frames read");
    Expect(out.at(0) == 1.0f && out.at(3) == 1.0f, "with real samples");
    ExpectEqInt(static_cast<long long>(ring.Write(five_frames)), 2, "and the space came back");
  });

  AddTest(tests, "AudioRing/AnUnderrunIsSilenceAndACountRatherThanAShortBuffer", [] {
    // A device callback must always fill its whole block. A short answer is a click, which
    // is what an underrun sounds like when nobody handled it -- and the count is what tells
    // the engine whether to grow the buffer or slow the decoder.
    media::AudioRing ring(8, 1);
    const std::vector<float> two(2, 0.25f);
    ring.Write(two);
    std::vector<float> out(6, -1.0f);
    ExpectEqInt(static_cast<long long>(ring.Read(out)), 2, "two real frames");
    Expect(out.at(0) == 0.25f && out.at(1) == 0.25f, "the real ones are the samples");
    Expect(out.at(2) == 0.0f && out.at(5) == 0.0f, "and the rest is silence, not stale memory");
    ExpectEqInt(static_cast<long long>(ring.TakeUnderruns()), 1, "counted once");
    ExpectEqInt(static_cast<long long>(ring.TakeUnderruns()), 0, "and taking it clears it");
  });

  AddTest(tests, "AudioRing/WrapsWithoutSplittingAFrame", [] {
    // Cursors are in samples rather than frames precisely so a wrap cannot land mid-frame.
    // If it could, one channel would lead the other by a sample for the rest of the stream
    // -- which is audible as a phase shift rather than as a failure.
    media::AudioRing ring(3, 2);
    for (int round = 0; round < 20; ++round) {
      const std::vector<float> frame = {static_cast<float>(round), static_cast<float>(-round)};
      ExpectEqInt(static_cast<long long>(ring.Write(frame)), 1, "one frame in");
      std::vector<float> out(2, 999.0f);
      ExpectEqInt(static_cast<long long>(ring.Read(out)), 1, "one frame out");
      Expect(out.at(0) == static_cast<float>(round) && out.at(1) == static_cast<float>(-round),
             "left and right stay together across every wrap");
    }
  });

  AddTest(tests, "PlaybackClock/ReportsWhatWasHeardRatherThanWhatWasWritten", [] {
    // The device holds a buffer, so the written position is ahead of the audible one.
    // Reporting the written position runs video ahead of the sound by exactly that much,
    // and that is the arithmetic a naive clock omits.
    media::PlaybackClock clock(48000);
    clock.SetFramesConsumed(48000);
    Expect(clock.CurrentTimeSeconds() == 1.0, "a second of frames is a second");
    clock.SetBufferedFrames(24000);
    Expect(clock.CurrentTimeSeconds() == 0.5,
           "half of which has not been heard yet, so the position is half a second");
  });

  AddTest(tests, "PlaybackClock/NeverGoesBackwardsExceptOnASeek", [] {
    // A clock that moved backwards would make a video re-present a frame it had already
    // shown. A seek is the one explicit exception, and it restarts the frame count --
    // keeping it would make the clock jump forward by everything that had already played.
    media::PlaybackClock clock(44100);
    double last = clock.CurrentTimeSeconds();
    std::uint64_t total = 0;
    for (int block = 0; block < 50; ++block) {
      total += 441;
      clock.SetFramesConsumed(total);
      clock.SetBufferedFrames(block % 7 == 0 ? 1000u : 0u);
      const double now = clock.CurrentTimeSeconds();
      Expect(now >= last || block % 7 == 0,
             "the position advances, and only a change in device occupancy can hold it");
      last = now;
    }
    clock.SeekTo(30.0);
    Expect(clock.CurrentTimeSeconds() == 30.0, "a seek is exactly where it was told");
    // The ring's counter keeps rising across a seek, so the clock has to subtract where it
    // stood -- otherwise the next update jumps forward by everything already played.
    clock.SetFramesConsumed(total + 44100);
    Expect(clock.CurrentTimeSeconds() == 31.0, "and time runs from there, not from zero");
  });

  AddTest(tests, "PlaybackClock/ARateOfZeroIsRefusedIntoOne", [] {
    // Every caller of a media clock divides by the sample rate. A zero would answer
    // infinity, and a video comparing a timestamp against infinity presents nothing.
    media::PlaybackClock clock(0);
    ExpectEqInt(clock.SampleRate(), 48000, "refused into a real rate");
    clock.SetFramesConsumed(48000);
    Expect(clock.CurrentTimeSeconds() == 1.0, "which answers rather than dividing by zero");
  });

  AddTest(tests, "PlaybackClock/ATotalIsIdempotentWhereAnIncrementWouldDrift", [] {
    // The ring reports a *total*, so reading it twice or missing a turn answers the same
    // position. With an increment, a caller that polled twice would double-count and the
    // clock would run ahead of the sound -- which is the bug the total exists to prevent.
    media::AudioRing ring(256, 2);
    const std::vector<float> frames(200, 0.0f);  // 100 stereo frames
    ring.Write(frames);
    std::vector<float> out(200, 0.0f);
    ring.Read(out);
    ExpectEqInt(static_cast<long long>(ring.FramesRead()), 100, "the ring counted them");
    media::PlaybackClock clock(48000);
    clock.SetFramesConsumed(ring.FramesRead());
    const double once = clock.CurrentTimeSeconds();
    clock.SetFramesConsumed(ring.FramesRead());
    Expect(clock.CurrentTimeSeconds() == once, "and asking twice does not move it");
    // Silence padded into an underrun is not part of the stream, so it must not advance the
    // clock past what the media contains.
    std::vector<float> empty_block(400, 0.0f);
    ring.Read(empty_block);
    ExpectEqInt(static_cast<long long>(ring.FramesRead()), 100,
                "an underrun's silence was never in the stream and is not counted");
  });

  AddTest(tests, "AudioSink/TheDeviceAgreesWithItselfAboutWhetherItIsRunning", [] {
    // A test host may or may not have a sound card, and this asserts the same thing either
    // way -- which is the only honest shape for a test that touches a device. What it is
    // really checking is the *state machine*: `Start` and `IsRunning` cannot disagree,
    // `Stop` is idempotent, and nothing is queued when nothing is open.
    //
    // No device is not an error, deliberately: a browser that plays video silently on a
    // machine with no sound card is better than one that refuses to play it.
    platform::SdlAudioDevice device;
    media::AudioRing ring(1024, 2);
    Expect(!device.IsRunning(), "not running before it is started");
    ExpectEqInt(static_cast<long long>(device.QueuedFrames()), 0, "and holding nothing");
    const bool started = device.Start(ring);
    Expect(device.IsRunning() == started, "running exactly when Start said so");
    if (started) {
      Expect(device.SampleRate() > 0 && device.Channels() > 0,
             "and a rate a clock can be built from");
    } else {
      ExpectEqInt(device.SampleRate(), 0,
                  "or no rate at all, rather than a plausible-looking default a clock would "
                  "silently use");
    }
    device.Stop();
    Expect(!device.IsRunning(), "stopped");
    device.Stop();
    Expect(!device.IsRunning(), "and stopping twice is not an error");
    ExpectEqInt(static_cast<long long>(device.QueuedFrames()), 0, "with nothing left queued");
  });

  AddTest(tests, "AudioRing/TwoRealThreadsHandOffWithoutTearing", [] {
    // **The assertion this file exists for.** Under TSan this is what proves the
    // release/acquire pairing rather than the comment claiming it; without threads, a
    // relaxed store passes every other test here.
    //
    // The data is a counting sequence, so a torn handoff is not merely "wrong samples" but
    // a *detectable* discontinuity: the consumer knows exactly which value must come next.
    media::AudioRing ring(64, 1);
    constexpr int kFrames = 20000;
    std::atomic<bool> producer_done{false};
    std::atomic<int> mismatches{0};
    std::atomic<int> consumed{0};

    std::thread consumer([&ring, &producer_done, &mismatches, &consumed] {
      float expected = 0.0f;
      std::vector<float> block(16, 0.0f);
      while (true) {
        const std::size_t got = ring.Read(block);
        for (std::size_t i = 0; i < got; ++i) {
          if (block[i] != expected) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          }
          expected += 1.0f;
        }
        consumed.fetch_add(static_cast<int>(got), std::memory_order_relaxed);
        if (got == 0 && producer_done.load(std::memory_order_acquire) &&
            ring.ReadableFrames() == 0) {
          break;
        }
      }
    });

    std::vector<float> pending;
    for (int frame = 0; frame < kFrames; ++frame) {
      pending.push_back(static_cast<float>(frame));
      // Offered in small chunks with the leftover kept, which is how a decoder feeds a
      // device: the ring is smaller than the stream by design.
      if (pending.size() >= 8) {
        std::size_t written = 0;
        while (written < pending.size()) {
          const std::size_t took =
              ring.Write(std::span<const float>(pending).subspan(written));
          if (took == 0) {
            std::this_thread::yield();
            continue;
          }
          written += took;
        }
        pending.clear();
      }
    }
    while (!pending.empty()) {
      const std::size_t took = ring.Write(pending);
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(took));
      if (took == 0) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
    consumer.join();

    ExpectEqInt(static_cast<long long>(mismatches.load()), 0,
                "every sample arrived in order and none was read before it was written");
    ExpectEqInt(static_cast<long long>(consumed.load()), kFrames,
                "and every frame arrived exactly once");
  });
}

}  // namespace microbrowser::tests

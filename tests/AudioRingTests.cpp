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

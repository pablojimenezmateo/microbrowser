#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microbrowser::media {

// The one piece of memory the audio thread and the engine thread both touch.
//
// ADR 0028 §4, and **the ownership statement comes before the code** because
// `AGENTS.md` requires one of any thread and this is the object that statement is about:
//
//   * The **audio thread owns** the device handle, this buffer's read cursor, and the
//     playback clock. It reads; it never writes the write cursor and never allocates.
//   * The **engine thread owns** the write cursor and the storage. It writes decoded
//     samples in and it is the only thing that ever resizes -- which it may do only while
//     no audio thread exists.
//   * It **borrows nothing**. Samples arrive by handoff: a value copied in, never a
//     pointer to a decoder's output, so the audio thread cannot reach a document, a
//     decoder or the heap.
//   * It is **joined** when the last playing element stops. The buffer outliving the
//     thread is fine; the thread outliving the buffer is what the join prevents.
//
// Single producer, single consumer, and that is not a simplification -- it is what makes
// the whole thing correct with two atomics and no lock. A second writer would need one,
// and a lock in an audio callback is a click in the output. If a second producer ever
// appears, the answer is a second ring rather than a mutex here.
//
// **What the memory ordering means, since it is the part that cannot be tested into
// existence.** The producer writes samples, then publishes the new write cursor with a
// *release*; the consumer reads the cursor with an *acquire* and then reads the samples.
// That pairing is what makes the sample writes visible before the cursor that admits
// them. Reversed -- or made relaxed -- the consumer can read the cursor for samples that
// have not landed, and the output is a fragment of whatever was in that memory before.
class AudioRing {
 public:
  // Interleaved float samples: what every audio device on every platform this targets
  // accepts, and what a decoder produces. A frame is `channels` samples.
  //
  // Sized in *frames* so that a caller cannot ask for a capacity that is not a whole
  // number of frames -- which would make one channel lead the other by a sample for the
  // rest of the stream.
  AudioRing(std::size_t frames, int channels)
      : channels_(channels < 1 ? 1 : channels),
        // One extra frame: with a power-of-two-free ring, `write == read` has to mean
        // *empty*, so a full buffer must leave one frame unused. Without it, full and
        // empty are the same state and the consumer drains a full buffer as silence.
        samples_((frames + 1) * static_cast<std::size_t>(channels_ < 1 ? 1 : channels_), 0.0f) {}

  int Channels() const { return channels_; }
  std::size_t CapacityFrames() const {
    return samples_.size() / static_cast<std::size_t>(channels_) - 1u;
  }

  // --- the producer's half: the engine thread only ---------------------------
  //
  // How many whole frames can be written now. A snapshot: the consumer only ever makes
  // this larger, so a producer that acts on a stale answer under-writes rather than
  // over-writes -- which is the direction that cannot corrupt.
  std::size_t WritableFrames() const;

  // Writes as many whole frames of `interleaved` as fit, and returns how many. A partial
  // write is normal: the caller keeps the rest and offers it again when the device has
  // drained. Refusing to write anything until it all fits would stall a stream behind one
  // oversized chunk.
  std::size_t Write(std::span<const float> interleaved);

  // --- the consumer's half: the audio thread only ---------------------------
  //
  // Fills `out` with frames, padding with silence when there are not enough, and returns
  // how many real frames it got. **Silence rather than a short buffer** because a device
  // callback must always produce its whole block: a short answer is a click, and a click
  // is what an underrun sounds like when nobody handled it.
  std::size_t Read(std::span<float> out);

  std::size_t ReadableFrames() const;

  // Underruns since the last check, as a counter the engine reads and clears. A count
  // rather than a flag, because "we ran out three times" and "we ran out once" call for
  // different responses -- a bigger buffer versus a slower decoder.
  std::uint32_t TakeUnderruns() { return underruns_.exchange(0, std::memory_order_relaxed); }

 private:
  int channels_ = 2;
  // Never resized while an audio thread exists; see the ownership statement.
  std::vector<float> samples_;
  // In samples, not frames, so the arithmetic that indexes `samples_` has no multiply in
  // it -- and so a wrapped cursor cannot land mid-frame.
  std::atomic<std::size_t> write_{0};
  std::atomic<std::size_t> read_{0};
  std::atomic<std::uint32_t> underruns_{0};
};

}  // namespace microbrowser::media

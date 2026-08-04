#include <cstddef>
#include <cstdint>
#include <span>

#include "media/IsoBmff.h"

// The ISO-BMFF demuxer, fed arbitrary bytes.
//
// ADR 0013 keeps container parsing in this project rather than handing it to a
// media framework, on the argument that the container is the layer that decides
// what the codec is asked to decode. That argument only holds if this layer is
// itself trustworthy, and a self-describing format made entirely of lengths the
// file chose is exactly the shape that rewards fuzzing: every box declares how
// big it is, and every one of those numbers is an attacker's.
//
// Landed on the same commit as the parser, which AGENTS.md requires of anything
// that reads hostile bytes -- a fuzz target added later is a fuzz target that
// starts from a corpus of bugs already shipped.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);

  // Sniffing must be safe on its own: it runs before anyone has decided the
  // bytes are a media file at all.
  const bool sniffed = microbrowser::media::LooksLikeIsoBmff(input);
  (void)sniffed;

  const microbrowser::media::IsoBmffFile file = microbrowser::media::ParseIsoBmff(input);
  if (!file.Ok()) {
    return 0;
  }

  // **Every reported range must be inside the input.** This is the assertion
  // that matters: a demuxer's output is a set of pointers handed to a decoder,
  // and a range that escapes the buffer is a read past the end in whatever
  // process the decoder lives in.
  //
  // The check is exact and runs on every sample. Touching the bytes -- which is
  // what makes a sanitizer say so rather than leaving it for the decoder to
  // discover -- is capped, because samples may legitimately overlap and a
  // parse may report tens of thousands of them: an uncapped loop here is
  // quadratic in the input and drops the fuzzer to one execution per second,
  // which is a harness that finds nothing while looking busy. Measured, not
  // guessed: it was one execution per second before this cap.
  constexpr std::size_t kMaxBytesTouched = 1u << 16;
  std::size_t touched = 0;
  volatile std::uint8_t sink = 0;
  for (const microbrowser::media::MediaSample& sample : file.samples) {
    if (sample.offset > size || sample.size > size - sample.offset) {
      // Not a graceful failure -- deliberately. A parse that reported an
      // out-of-bounds range while claiming success is the single worst outcome
      // this module has, so it aborts rather than returning and letting the
      // run look clean.
      __builtin_trap();
    }
    for (std::size_t i = 0; i < sample.size && touched < kMaxBytesTouched; ++i, ++touched) {
      sink = static_cast<std::uint8_t>(sink ^ data[sample.offset + i]);
    }
  }
  for (const microbrowser::media::MediaTrack& track : file.tracks) {
    if (track.codec_config_size == 0) {
      continue;
    }
    if (track.codec_config_offset > size ||
        track.codec_config_size > size - track.codec_config_offset) {
      __builtin_trap();
    }
    for (std::size_t i = 0; i < track.codec_config_size; ++i) {
      sink = static_cast<std::uint8_t>(sink ^ data[track.codec_config_offset + i]);
    }
  }
  (void)sink;
  return 0;
}

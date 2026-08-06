#include <cstddef>
#include <cstdint>
#include <span>

#include "media/MpegTs.h"

// A transport stream, demuxed. ADR 0028 §2.
//
// Every byte is attacker-controlled: an HLS segment is fetched from wherever a playlist pointed, and a
// playlist is a page's own text. And a transport stream is the most self-declared of the three
// containers -- an adaptation field length, a PSI pointer field, a section length, a PES header length
// and a continuity counter are all read from the stream and all used as offsets.
//
// The invariants are the ones a caller depends on, and each is something that would be a
// use-after-bounds in the *caller* rather than here:
//
//   * **Every reported range lies inside the input.** A caller hands these ranges to a decoder; a
//     range that ran past the end would be the parser handing out a read primitive.
//   * **A sample's pieces are in order and do not overlap.** They are concatenated to form an access
//     unit, so an overlap would duplicate bytes and a reversal would scramble a frame.
//   * **`total_size` is the sum of the pieces**, because a caller allocates from it and copies by
//     iterating them -- a mismatch is a heap overflow at the far end.
//   * **The bounds hold.** No more streams than `kMaxTsStreams`, no more samples than `kMaxTsSamples`:
//     a page controls how many segments it appends.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  // Sniffing must answer for any input and must not read past it.
  (void)media::LooksLikeMpegTs(bytes);

  const media::MpegTsFile file = media::ParseMpegTs(bytes);
  if (file.streams.size() > media::kMaxTsStreams || file.samples.size() > media::kMaxTsSamples) {
    __builtin_trap();
  }
  for (const media::MpegTsSample& sample : file.samples) {
    if (sample.pieces.empty()) {
      __builtin_trap();  // a sample with no bytes is not a sample
    }
    std::size_t total = 0;
    std::size_t previous_end = 0;
    for (const media::MpegTsRange& piece : sample.pieces) {
      if (piece.size == 0) {
        __builtin_trap();
      }
      if (piece.offset >= size || piece.size > size - piece.offset) {
        __builtin_trap();  // a range outside the input
      }
      if (piece.offset < previous_end) {
        __builtin_trap();  // out of order, or overlapping the one before
      }
      previous_end = piece.offset + piece.size;
      total += piece.size;
    }
    if (total != sample.total_size) {
      __builtin_trap();
    }
  }
  // A stream reported as decodable must have named one of the five codecs, and one reported as not must
  // not have. The two flags are read together by callers and disagreeing is how a caller ends up
  // configuring a decoder for a codec the container refused.
  for (const media::MpegTsStream& stream : file.streams) {
    if (stream.has_codec && stream.kind == media::TrackKind::Unknown) {
      __builtin_trap();
    }
  }
  return 0;
}

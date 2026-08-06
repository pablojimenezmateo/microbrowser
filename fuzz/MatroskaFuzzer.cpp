#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "media/Matroska.h"

// A WebM file, from a server.
//
// ADR 0028 §2. EBML is the harder of this browser's two containers to fuzz *for*, because both an
// element's id and its size are variable-length integers: a flipped bit in a length byte does not
// corrupt one element, it moves every element after it. And an unknown size is legal, so "runs to
// the end" is a case a hostile file can reach at any depth.
//
// The properties asserted are the ones a player and a decoder act on:
//
//   * **Every sample is inside the file.** A demuxer reports byte ranges (ADR 0013), so an
//     out-of-range one is a read a *decoder* would perform -- the exact shape of the canonical
//     media CVE.
//   * **Counts stay bounded.** A file declaring a million samples must come back refused rather
//     than allocated.
//   * **Every track that comes out is usable**: it has a number and a kind, because a caller
//     iterating tracks must not have to know which ones are real.
//   * **It terminates.** An unknown-size element inside an unknown-size element inside a wrapped
//     offset is where a container parser loops forever, and libFuzzer's timeout is what proves it
//     does not.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(data), size);
  const std::optional<media::MatroskaFile> parsed = media::ParseMatroska(file);
  if (!parsed.has_value()) {
    return 0;
  }
  if (parsed->tracks.size() > 64 || parsed->samples.size() > 200000) {
    __builtin_trap();
  }
  for (const media::MediaSample& sample : parsed->samples) {
    if (sample.offset > size || sample.size > size || sample.offset + sample.size > size) {
      __builtin_trap();
    }
  }
  for (const media::MediaTrack& track : parsed->tracks) {
    if (track.id == 0 || track.kind == media::TrackKind::Unknown) {
      __builtin_trap();
    }
    // A codec configuration is a byte range too, and a decoder is handed it directly.
    if (track.codec_config_offset + track.codec_config_size > size) {
      __builtin_trap();
    }
    // Dimensions size a surface; a rate and a channel count size a buffer.
    if (track.width > 16384 || track.height > 16384 || track.channels > 64) {
      __builtin_trap();
    }
  }
  // The timecode scale divides every timestamp, so a zero would make a player divide by it.
  if (parsed->timecode_scale_ns == 0) {
    __builtin_trap();
  }
  return 0;
}

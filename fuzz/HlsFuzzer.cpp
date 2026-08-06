#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "media/HlsPlaylist.h"

// An `.m3u8` playlist, from a CDN.
//
// ADR 0028 §2. A playlist is a text file that decides **what this browser fetches next**, which is
// what makes it worth fuzzing rather than trusting: every string that comes out of it becomes a URL
// and every number becomes a schedule.
//
// The properties asserted are the ones a player acts on:
//
//   * **Bounds hold.** Neither list exceeds the cap, whatever the file declares.
//   * **Every duration a player is given is schedulable** -- strictly positive and finite. A zero
//     is an infinite loop in a scheduler that advances by duration, and this is where that is
//     guaranteed rather than checked again at the caller.
//   * **A playlist is never both kinds.** A player cannot know whether to fetch a URL or recurse
//     into it, so a file that mixes them must come back as one kind with the rest refused.
//   * **No entry has an empty URL.** An empty one resolves to the playlist's own address, and a
//     player that fetched it would request the playlist as a segment -- forever.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  if (size < 1) {
    return 0;
  }
  // The cap out of the input so the boundary is explored rather than one arbitrary limit.
  const std::size_t cap = static_cast<std::size_t>(data[0]) + 1u;
  const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);

  const std::optional<media::HlsPlaylist> playlist = media::ParseHlsPlaylist(text, cap);
  if (!playlist.has_value()) {
    return 0;
  }
  if (playlist->segments.size() > cap || playlist->variants.size() > cap) {
    __builtin_trap();
  }
  if (!playlist->segments.empty() && !playlist->variants.empty()) {
    __builtin_trap();  // both kinds at once is unplayable
  }
  for (const media::HlsSegment& segment : playlist->segments) {
    if (segment.url.empty()) {
      __builtin_trap();
    }
    if (!(segment.duration_seconds > 0.0) || segment.duration_seconds >= 86400.0) {
      __builtin_trap();
    }
  }
  for (const media::HlsVariant& variant : playlist->variants) {
    if (variant.url.empty()) {
      __builtin_trap();
    }
    // A resolution is used to size a surface, so a number no allocation should be derived from must
    // not reach a caller.
    if (variant.width < 0 || variant.height < 0 || variant.width > 16384 || variant.height > 16384) {
      __builtin_trap();
    }
  }
  return 0;
}

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::media {

// An `.m3u8` playlist, parsed. ADR 0028 §2.
//
// HLS is a *playlist format* rather than a container: a text file naming segments that are
// themselves fMP4 or MPEG-TS. That makes this the cheapest piece of the media stack and one of the
// most widely used -- reddit's own front page references two `.m3u8` playlists, which is the fact
// that put it in this session rather than in a Plex-only one.
//
// Two kinds of playlist, and telling them apart is the parser's first job because everything after
// it differs:
//
//   * A **master** playlist lists *variants* -- the same content at different bitrates -- each with
//     its own playlist URL. A player picks one.
//   * A **media** playlist lists *segments*, in order, with durations.
//
// A file that is neither, or that claims to be both, is refused. A player that guessed would fetch
// a variant list as though it were segments and play its own playlist as video.
//
// Every number here comes from a text file a stranger wrote, so every one is bounded and every
// overflow saturates. The counts especially: a playlist declaring a million segments is a
// playlist, not an attack, but it is also not one this browser will hold -- so the bound is a
// refusal with a reason rather than an allocation.
struct HlsVariant {
  std::string url;
  // Bits per second, as declared. Zero when absent: `BANDWIDTH` is required by the specification
  // and a variant without one is still a variant, so it is kept and sorted last rather than
  // dropped.
  std::uint64_t bandwidth = 0;
  std::string codecs;
  int width = 0;
  int height = 0;
};

struct HlsSegment {
  std::string url;
  double duration_seconds = 0.0;
  // A discontinuity *before* this segment: the timeline restarts, so a decoder has to be reset.
  // Carried because a player that ignored it would decode the next segment's frames against the
  // previous one's timestamps.
  bool discontinuity = false;
};

struct HlsPlaylist {
  enum class Kind : std::uint8_t { Unknown, Master, Media };

  Kind kind = Kind::Unknown;
  std::vector<HlsVariant> variants;
  std::vector<HlsSegment> segments;
  // `#EXT-X-TARGETDURATION`, which a player uses to decide how often to reload a live playlist.
  double target_duration_seconds = 0.0;
  // `#EXT-X-ENDLIST`: the playlist is complete. Its absence means **live**, and the difference is
  // not cosmetic -- a live playlist must be reloaded and a complete one must not be, so a parser
  // that lost this would either poll a finished stream forever or play a live one once and stop.
  bool complete = false;
  // Whether anything was refused. A playlist is *usable* with some entries dropped -- that is how
  // HLS is extended -- so this is a signal for the console rather than a failure.
  bool had_refusals = false;
};

// Nothing when the bytes are not a playlist at all: HLS requires `#EXTM3U` on the first line, and a
// file without it is something else -- an HTML error page a CDN served with the wrong status is the
// common case, and playing it as a playlist is how a player ends up requesting segments named
// `<html>`.
std::optional<HlsPlaylist> ParseHlsPlaylist(std::string_view text,
                                            std::size_t max_entries = 20000);

}  // namespace microbrowser::media

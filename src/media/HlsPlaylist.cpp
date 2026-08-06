#include "media/HlsPlaylist.h"

#include <algorithm>

#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::media {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// A line, without its terminator. Both `\n` and `\r\n` are in the wild and a parser that kept the
// `\r` would produce segment URLs with a carriage return in them -- which a server answers with a
// 400 and which is invisible in a log.
std::string_view NextLine(std::string_view text, std::size_t& at) {
  const std::size_t end = text.find('\n', at);
  std::string_view line = end == std::string_view::npos ? text.substr(at) : text.substr(at, end - at);
  at = end == std::string_view::npos ? text.size() : end + 1;
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

// One attribute out of a comma-separated attribute list, honouring quotes.
//
// `CODECS="avc1.4d401e,mp4a.40.2"` is the reason this cannot be a split on commas: the value
// itself contains one, and a parser that split first would produce a codec list of two broken
// halves and a variant it then refuses.
std::optional<std::string_view> Attribute(std::string_view list, std::string_view name) {
  std::size_t at = 0;
  while (at < list.size()) {
    const std::size_t equals = list.find('=', at);
    if (equals == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view key = util::TrimAscii(list.substr(at, equals - at));
    std::size_t value_start = equals + 1;
    std::size_t value_end = 0;
    if (value_start < list.size() && list[value_start] == '"') {
      ++value_start;
      value_end = list.find('"', value_start);
      if (value_end == std::string_view::npos) {
        return std::nullopt;  // an unterminated quote: the rest of the line is not attributes
      }
      at = value_end + 1;
      const std::size_t comma = list.find(',', at);
      at = comma == std::string_view::npos ? list.size() : comma + 1;
    } else {
      value_end = list.find(',', value_start);
      at = value_end == std::string_view::npos ? list.size() : value_end + 1;
      if (value_end == std::string_view::npos) {
        value_end = list.size();
      }
    }
    if (util::EqualsAsciiCaseInsensitive(key, name)) {
      return util::TrimAscii(list.substr(value_start, value_end - value_start));
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<HlsPlaylist> ParseHlsPlaylist(std::string_view text, std::size_t max_entries) {
  std::size_t at = 0;
  const std::string_view first = util::TrimAscii(NextLine(text, at));
  if (first != "#EXTM3U") {
    // Not a playlist. An HTML error page a CDN served with a 200 is the common case, and playing
    // it as a playlist is how a player ends up requesting a segment named `<html>`.
    AddPerformanceCounter(PerfCounterId::HlsRefusals);
    return std::nullopt;
  }

  HlsPlaylist playlist;
  // Attributes seen but not yet attached: an `#EXTINF` or `#EXT-X-STREAM-INF` describes the *next*
  // URL line, so both are held until it arrives. A file whose last line is a tag with no URL after
  // it simply drops it, which is what every player does.
  std::optional<double> pending_duration;
  std::optional<HlsVariant> pending_variant;
  bool pending_discontinuity = false;

  while (at < text.size()) {
    const std::string_view line = util::TrimAscii(NextLine(text, at));
    if (line.empty()) {
      continue;
    }
    if (line.front() != '#') {
      // A URL line, and which list it joins depends on what preceded it. This is the one place the
      // two kinds of playlist are distinguished, and it is done from *evidence* rather than from a
      // tag: a file with `#EXTINF` lines is a media playlist whatever else it claims.
      if (pending_variant.has_value()) {
        if (playlist.segments.empty() && playlist.variants.size() < max_entries) {
          pending_variant->url = std::string(line);
          playlist.variants.push_back(std::move(*pending_variant));
          playlist.kind = HlsPlaylist::Kind::Master;
        } else {
          // Either the file mixes both kinds -- which is invalid and unplayable, because a player
          // cannot know whether to fetch or to recurse -- or it is over the bound.
          playlist.had_refusals = true;
        }
        pending_variant.reset();
        continue;
      }
      if (pending_duration.has_value()) {
        if (playlist.variants.empty() && playlist.segments.size() < max_entries) {
          playlist.segments.push_back(
              HlsSegment{std::string(line), *pending_duration, pending_discontinuity});
          playlist.kind = HlsPlaylist::Kind::Media;
        } else {
          playlist.had_refusals = true;
        }
        pending_duration.reset();
        pending_discontinuity = false;
        continue;
      }
      // A bare URL with no tag before it. Refused rather than guessed: in a media playlist it
      // would be a segment of unknown duration, which a player cannot schedule.
      playlist.had_refusals = true;
      continue;
    }

    if (util::StartsWithAsciiCaseInsensitive(line, "#EXTINF:")) {
      // `#EXTINF:<duration>,<title>` -- the title is free text and is ignored, but it has to be
      // *skipped* rather than parsed into the duration.
      const std::string_view rest = line.substr(8);
      const std::size_t comma = rest.find(',');
      const std::string_view number = comma == std::string_view::npos ? rest : rest.substr(0, comma);
      const std::optional<double> parsed = util::ParseDouble(util::TrimAscii(number));
      // A negative or absurd duration is refused rather than clamped: a player schedules from it,
      // and a zero-length segment is an infinite loop in a scheduler that advances by duration.
      if (parsed.has_value() && *parsed > 0.0 && *parsed < 86400.0) {
        pending_duration = *parsed;
      } else {
        playlist.had_refusals = true;
      }
      continue;
    }
    if (util::StartsWithAsciiCaseInsensitive(line, "#EXT-X-STREAM-INF:")) {
      HlsVariant variant;
      const std::string_view attributes = line.substr(18);
      if (const std::optional<std::string_view> bandwidth = Attribute(attributes, "BANDWIDTH")) {
        if (const std::optional<int> parsed = util::ParseInt(*bandwidth); parsed.has_value() &&
                                                                        *parsed > 0) {
          variant.bandwidth = static_cast<std::uint64_t>(*parsed);
        }
      }
      if (const std::optional<std::string_view> codecs = Attribute(attributes, "CODECS")) {
        variant.codecs = std::string(*codecs);
      }
      if (const std::optional<std::string_view> resolution = Attribute(attributes, "RESOLUTION")) {
        const std::size_t cross = resolution->find('x');
        if (cross != std::string_view::npos) {
          const std::optional<int> w = util::ParseInt(resolution->substr(0, cross));
          const std::optional<int> h = util::ParseInt(resolution->substr(cross + 1));
          // Bounded, because a resolution is used to pick a variant and to size a surface: a
          // declared 2-billion-pixel width is a number no allocation should be derived from.
          if (w.has_value() && h.has_value() && *w > 0 && *h > 0 && *w <= 16384 && *h <= 16384) {
            variant.width = *w;
            variant.height = *h;
          }
        }
      }
      pending_variant = std::move(variant);
      continue;
    }
    if (util::StartsWithAsciiCaseInsensitive(line, "#EXT-X-TARGETDURATION:")) {
      if (const std::optional<double> parsed = util::ParseDouble(line.substr(22));
          parsed.has_value() && *parsed > 0.0 && *parsed < 86400.0) {
        playlist.target_duration_seconds = *parsed;
      }
      continue;
    }
    if (util::EqualsAsciiCaseInsensitive(line, "#EXT-X-ENDLIST")) {
      // The playlist is complete. Its *absence* means live, which is why this is a flag rather
      // than an assumption: a live playlist must be reloaded and a complete one must not be.
      playlist.complete = true;
      continue;
    }
    if (util::EqualsAsciiCaseInsensitive(line, "#EXT-X-DISCONTINUITY")) {
      pending_discontinuity = true;
      continue;
    }
    // Every other tag is ignored, which is how HLS is extended and is required rather than lax:
    // a parser that refused unknown tags would refuse every playlist written after it.
  }

  if (playlist.kind == HlsPlaylist::Kind::Unknown) {
    // `#EXTM3U` and nothing usable. Distinguished from "not a playlist" because it *is* one --
    // an empty live playlist is legal and a player reloads it -- so the caller gets a playlist
    // with nothing in it rather than nothing at all.
    AddPerformanceCounter(PerfCounterId::HlsEmptyPlaylists);
  }
  AddPerformanceCounter(PerfCounterId::HlsPlaylistsParsed);
  return playlist;
}

}  // namespace microbrowser::media

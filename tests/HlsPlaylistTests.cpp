// `.m3u8` playlists.
//
// ADR 0028 §2. The fixtures are the shapes the target sites actually serve -- reddit's front page
// references two playlists, and Plex serves one per transcode -- and the assertions worth reading
// are the refusals: what a player must *not* be told, because a playlist is a text file a stranger
// wrote and everything after the parse is a fetch.

#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "media/HlsPlaylist.h"

namespace microbrowser::tests {

namespace {

using media::HlsPlaylist;
using media::ParseHlsPlaylist;

}  // namespace

void RegisterHlsPlaylistTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Hls/AMasterPlaylistIsItsVariants", [] {
    // The shape a player fetches first. `CODECS` holds a comma, which is why attributes cannot be
    // parsed by splitting on commas -- a parser that did would produce two broken halves.
    const std::optional<HlsPlaylist> playlist = ParseHlsPlaylist(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=1280x720,CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
        "720p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=480000,RESOLUTION=640x360,CODECS=\"avc1.4d401e\"\n"
        "360p.m3u8\n");
    Expect(playlist.has_value(), "it parsed");
    Expect(playlist->kind == HlsPlaylist::Kind::Master, "as a master playlist");
    ExpectEqInt(static_cast<long long>(playlist->variants.size()), 2, "two variants");
    ExpectEqString(playlist->variants.at(0).url, "720p.m3u8", "the first URL");
    ExpectEqInt(static_cast<long long>(playlist->variants.at(0).bandwidth), 1280000, "its bitrate");
    ExpectEqInt(playlist->variants.at(0).width, 1280, "its width");
    ExpectEqString(playlist->variants.at(0).codecs, "avc1.4d401f,mp4a.40.2",
                   "and its codec list, comma and all");
    Expect(playlist->segments.empty(), "with no segments, because it names none");
  });

  AddTest(tests, "Hls/AMediaPlaylistIsItsSegmentsAndKnowsWhetherItIsLive", [] {
    // `#EXT-X-ENDLIST` is the difference between a stream to reload and one to play once. A parser
    // that lost it would either poll a finished stream forever or play a live one and stop.
    const std::optional<HlsPlaylist> complete = ParseHlsPlaylist(
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
        "#EXTINF:6.006,\nseg0.ts\n"
        "#EXT-X-DISCONTINUITY\n#EXTINF:5.994,title here\nseg1.ts\n"
        "#EXT-X-ENDLIST\n");
    Expect(complete.has_value() && complete->kind == HlsPlaylist::Kind::Media, "a media playlist");
    ExpectEqInt(static_cast<long long>(complete->segments.size()), 2, "two segments");
    Expect(complete->segments.at(0).duration_seconds > 6.0, "the first duration");
    Expect(!complete->segments.at(0).discontinuity, "no discontinuity before the first");
    // The title after the comma is free text and must be skipped rather than parsed as a number.
    Expect(complete->segments.at(1).duration_seconds > 5.9, "the second, despite its title");
    Expect(complete->segments.at(1).discontinuity,
           "and the discontinuity attaches to the segment after it, which is what a decoder reset "
           "keys off");
    Expect(complete->complete, "ENDLIST means complete");
    Expect(complete->target_duration_seconds == 6.0, "and the reload interval is known");

    const std::optional<HlsPlaylist> live =
        ParseHlsPlaylist("#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\nseg9.ts\n");
    Expect(live.has_value() && !live->complete,
           "no ENDLIST means live, which is a playlist to reload rather than one to finish");
  });

  AddTest(tests, "Hls/SomethingThatIsNotAPlaylistIsRefused", [] {
    // **The refusal that matters most.** A CDN serving an HTML error page with a 200 is the common
    // case, and playing it as a playlist is how a player ends up requesting a segment named
    // `<html>`.
    Expect(!ParseHlsPlaylist("<!DOCTYPE html><html><body>404</body></html>").has_value(),
           "HTML is not a playlist");
    Expect(!ParseHlsPlaylist("").has_value(), "nor is nothing");
    Expect(!ParseHlsPlaylist("#EXT-X-TARGETDURATION:6\n#EXTINF:6,\na.ts\n").has_value(),
           "and neither is a file that forgot #EXTM3U, however playlist-shaped the rest is");
  });

  AddTest(tests, "Hls/AnEmptyPlaylistIsAPlaylistRatherThanAFailure", [] {
    // A live playlist can legitimately be empty -- the encoder has not produced a segment yet --
    // and a player reloads it. Answering "not a playlist" would make that a permanent error.
    const std::optional<HlsPlaylist> empty = ParseHlsPlaylist("#EXTM3U\n#EXT-X-TARGETDURATION:6\n");
    Expect(empty.has_value(), "it is a playlist");
    Expect(empty->kind == HlsPlaylist::Kind::Unknown, "of no determined kind yet");
    Expect(empty->segments.empty() && empty->variants.empty(), "and it names nothing");
  });

  AddTest(tests, "Hls/UnknownTagsAreIgnoredBecauseThatIsHowHlsIsExtended", [] {
    // A parser that refused unknown tags would refuse every playlist written after it. This one has
    // encryption, a media sequence and a program date -- none of which this browser reads -- and
    // the segments still come out.
    const std::optional<HlsPlaylist> playlist = ParseHlsPlaylist(
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-MEDIA-SEQUENCE:42\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-08-06T00:00:00Z\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:2,\nseg42.m4s\n#EXT-X-ENDLIST\n");
    Expect(playlist.has_value(), "it parsed");
    ExpectEqInt(static_cast<long long>(playlist->segments.size()), 1, "one segment");
    ExpectEqString(playlist->segments.at(0).url, "seg42.m4s", "and it is the right one");
  });

  AddTest(tests, "Hls/ADurationAPlayerCannotScheduleIsRefused", [] {
    // A player advances by duration, so a zero or negative one is an infinite loop in a scheduler
    // and an absurd one is a stall. Refused rather than clamped: a clamp invents a schedule the
    // playlist did not describe.
    const std::optional<HlsPlaylist> playlist = ParseHlsPlaylist(
        "#EXTM3U\n#EXTINF:0,\nzero.ts\n#EXTINF:-5,\nnegative.ts\n"
        "#EXTINF:notanumber,\nnonsense.ts\n#EXTINF:4,\ngood.ts\n#EXT-X-ENDLIST\n");
    Expect(playlist.has_value(), "the playlist survives");
    ExpectEqInt(static_cast<long long>(playlist->segments.size()), 1, "with only the usable segment");
    ExpectEqString(playlist->segments.at(0).url, "good.ts", "which is the one that had a duration");
    Expect(playlist->had_refusals, "and it says something was refused");
  });

  AddTest(tests, "Hls/APlaylistThatMixesBothKindsIsNotPlayedAsEither", [] {
    // A file with variants *and* segments is invalid and unplayable: a player cannot know whether
    // to fetch a URL or to recurse into it. The first kind seen wins and the rest is refused, which
    // is a playable subset rather than a guess.
    const std::optional<HlsPlaylist> playlist = ParseHlsPlaylist(
        "#EXTM3U\n#EXTINF:4,\nseg.ts\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000\nvariant.m3u8\n#EXT-X-ENDLIST\n");
    Expect(playlist.has_value() && playlist->kind == HlsPlaylist::Kind::Media, "media, as it began");
    ExpectEqInt(static_cast<long long>(playlist->segments.size()), 1, "one segment");
    Expect(playlist->variants.empty(), "and the variant was not taken as one");
    Expect(playlist->had_refusals, "with the refusal reported");
  });

  AddTest(tests, "Hls/TheEntryCountIsBounded", [] {
    // A playlist declaring a hundred thousand segments is a playlist rather than an attack, and it
    // is also not one this browser will hold -- so the bound is a refusal with a reason rather than
    // an allocation.
    std::string text = "#EXTM3U\n";
    for (int i = 0; i < 50; ++i) {
      text += "#EXTINF:4,\nseg" + std::to_string(i) + ".ts\n";
    }
    const std::optional<HlsPlaylist> playlist = ParseHlsPlaylist(text, 10);
    Expect(playlist.has_value(), "it parsed");
    ExpectEqInt(static_cast<long long>(playlist->segments.size()), 10, "up to the bound");
    Expect(playlist->had_refusals, "and said the rest was dropped");
  });

  AddTest(tests, "Hls/CarriageReturnsAreNotPartOfAUrl", [] {
    // Both line endings are in the wild. A parser that kept the `\r` produces segment URLs with a
    // carriage return in them -- which a server answers with a 400 and which is invisible in a log.
    const std::optional<HlsPlaylist> playlist =
        ParseHlsPlaylist("#EXTM3U\r\n#EXTINF:4,\r\nseg.ts\r\n#EXT-X-ENDLIST\r\n");
    Expect(playlist.has_value(), "it parsed");
    ExpectEqInt(static_cast<long long>(playlist->segments.size()), 1, "one segment");
    ExpectEqString(playlist->segments.at(0).url, "seg.ts", "with a clean URL");
    Expect(playlist->complete, "and the tag matched despite the terminator");
  });
}

}  // namespace microbrowser::tests

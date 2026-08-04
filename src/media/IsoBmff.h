#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace microbrowser::media {

// ISO base media file format: MP4, and the fragmented MP4 that DASH streams.
//
// **Ours, on purpose.** ADR 0013 splits the media stack in two and puts the
// line here: a video codec is third-party, sandboxed and out of process,
// because a hand-written video decoder is a memory-safety catastrophe with a
// decade of CVEs already written for it. A container parser is the opposite
// trade -- it is small, and it is *the layer that decides what the codec is
// asked to decode*. Owning it is what keeps the codec's input constrained; a
// media framework that did both would take that away.
//
// So nothing here decodes anything. A demux produces descriptions: what tracks
// exist, what codec each one claims, and where in the file each sample's bytes
// are. The bytes themselves are never copied, examined, or interpreted. That is
// enforced by the module contract as much as by intent -- `src/media` may name
// `util` and nothing else, so a demuxer that started decoding could not name a
// type to decode into.
//
// Every byte is attacker-controlled, and the rules are the ones the PNG decoder
// states, applied to a format whose whole structure is self-declared lengths:
//
//   * Every box size is widened to 64 bits before it is compared, and checked
//     against the bytes that actually remain rather than against another
//     declared size.
//   * A box whose size is smaller than its own header fails the parse. That is
//     the non-advancing box, and it is how a box walker hangs.
//   * Nesting is bounded, because boxes contain boxes and a file can nest them
//     as deep as it likes.
//   * Entry counts are checked against the bytes left before anything is
//     reserved, so a table claiming four billion samples is refused rather
//     than sized for.
//
// What is parsed: `ftyp`, the `moov` track hierarchy down to the sample
// description, and the `moof` fragment boxes that carry sample positions in a
// DASH stream. Progressive MP4's `stco`/`stsz`/`stsc` sample tables are *not*
// parsed, and that is a scope decision rather than an oversight -- ADR 0007
// names fragmented MP4 over DASH as what the target site actually serves, and
// the two sample-position schemes share no code.

// What a codec is, as far as the container knows.
//
// A four-character code, kept as the string the file used. Deliberately not an
// enum: the container's job is to report what the file claims, and mapping that
// to "which decoder" is a decision for whoever owns the decoder process -- ADR
// 0013 has not chosen one, and an enum here would be this module guessing at
// that choice.
enum class TrackKind : std::uint8_t {
  Unknown,
  Video,
  Audio,
};

struct MediaTrack {
  std::uint32_t id = 0;
  TrackKind kind = TrackKind::Unknown;
  // The `stsd` sample entry's four-character code: "avc1", "vp09", "av01",
  // "mp4a", "Opus", and so on.
  std::string codec;
  // Ticks per second for this track's timestamps. Zero if the file did not say,
  // which makes every duration in it meaningless -- callers check.
  std::uint32_t timescale = 0;
  std::uint64_t duration = 0;

  // Video only, from the sample entry. Zero for an audio track.
  std::uint16_t width = 0;
  std::uint16_t height = 0;

  // Audio only. Zero for a video track.
  std::uint16_t channels = 0;
  std::uint16_t sample_size = 0;
  std::uint32_t sample_rate = 0;

  // The codec's own configuration record -- `avcC`, `vpcC`, `av1C`, `esds`.
  // A byte range into the file rather than a copy, for the same reason samples
  // are: this module does not know what is in it and must not pretend to.
  std::size_t codec_config_offset = 0;
  std::size_t codec_config_size = 0;
};

// One encoded sample, as a place in the file.
//
// A range, never the bytes. Copying them here would mean this module holding a
// buffer whose only consumer is a decoder in another process, and the whole
// point of the split is that the bytes go there without passing through
// anything that might look at them.
struct MediaSample {
  std::uint32_t track_id = 0;
  std::size_t offset = 0;
  std::size_t size = 0;
  std::uint64_t decode_time = 0;
  std::uint32_t duration = 0;
  bool is_sync = false;
};

struct IsoBmffFile {
  // From `ftyp`. Empty for a file that had none, which is legal for a fragment
  // but not for an initialization segment.
  std::string major_brand;
  std::vector<MediaTrack> tracks;
  // Samples from every `moof` in the input, in the order they appear.
  std::vector<MediaSample> samples;
  // Empty on success. A short constant reason -- never derived from the input
  // bytes, so it can be logged without echoing an attacker's string.
  const char* error = nullptr;

  bool Ok() const { return error == nullptr; }
};

// How deep boxes may nest. Real files reach about six
// (`moov`/`trak`/`mdia`/`minf`/`stbl`/`stsd`/sample entry); the bound is
// generous against that and small enough that the recursive walk cannot
// exhaust a stack.
inline constexpr int kMaxBoxDepth = 16;

// How many tracks and samples a single parse may produce. Both exist to bound
// memory against a file that declares more than it contains.
//
// 65536 samples is about eighteen minutes of sixty-frames-a-second video in a
// single fragment, against the two to ten seconds a real DASH segment holds --
// generous by three orders of magnitude and still only a couple of megabytes of
// MediaSample. The first draft of this was 2^20, which is the same bound with
// an extra sixteen times the memory and no extra file it accepts; the number
// matters because a page controls how many segments it asks for.
inline constexpr std::size_t kMaxTracks = 64;
inline constexpr std::size_t kMaxSamples = 1u << 16;

// Parses `bytes` as an ISO base media file.
//
// `bytes` must be the whole thing being described: sample offsets are absolute
// within it, so handing over a suffix would produce ranges that point at the
// wrong place rather than an error.
IsoBmffFile ParseIsoBmff(std::span<const std::byte> bytes);

// True when `bytes` begins with something that looks like an ISO base media
// file: a plausible first box whose type is one this format starts with.
//
// The right way to pick a demuxer, for the reason LooksLikePng is the right way
// to pick an image decoder -- a Content-Type is a claim by the server, and the
// bytes are what will actually be parsed.
bool LooksLikeIsoBmff(std::span<const std::byte> bytes);

}  // namespace microbrowser::media

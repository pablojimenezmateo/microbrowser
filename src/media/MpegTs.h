#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "media/CodecId.h"
#include "media/IsoBmff.h"

namespace microbrowser::media {

// MPEG-2 transport streams, demuxed. ADR 0028 §2, session 29, and the container HLS carries.
//
// This is here because of one measurement: HLS is what Plex serves when it transcodes, and an HLS
// segment is a `.ts` file. The playlist parser landed in session 26 and pointed at segments this
// module could not read.
//
// **A transport stream is a different kind of format from the other two, and the difference decides
// the whole design.** ISO-BMFF and Matroska are trees of self-describing boxes; a transport stream is
// a *packet multiplex* designed for a lossy broadcast channel. That means:
//
//   * **Fixed 188-byte packets** with a sync byte, so the parser resynchronises rather than fails --
//     a stream that starts mid-packet is normal and a player that refused one would refuse a live
//     tune-in.
//   * **The structure is discovered from inside the stream.** A PAT (on PID 0) names the PMT's PID;
//     the PMT names each elementary stream's PID and type. Until both have been seen, nothing in the
//     stream can be interpreted -- and either may appear *after* media packets, or be repeated with
//     different contents mid-stream.
//   * **A payload spans packets**, with a continuity counter that a broadcast may skip. A PES packet
//     is reassembled from however many transport packets it took, and a discontinuity means the one
//     being assembled is *discarded* rather than joined across the hole: half a frame stitched to
//     half of a later one is a frame no decoder can reject.
//   * **Timestamps are 33 bits at 90kHz**, so they wrap after nine hours and a bit -- which a live
//     stream reaches. This parser reports them as the stream carried them, wrap included: unwrapping
//     needs to know where the previous segment ended, which is the caller's state and not this one's.
//     Stated rather than silently ignored, because a caller that assumed monotonic timestamps would
//     see time jump backwards once every nine hours.
//
// The output is ADR 0013's shape -- descriptions and *places in the input*, never bytes -- but not the
// same struct the other two use, and the note on `MpegTsSample` says why. Nothing here looks inside a
// PES payload; the codec is taken from the PMT's stream-type byte and nothing else.

// One elementary stream the PMT named.
struct MpegTsStream {
  std::uint16_t pid = 0;
  // The PMT's `stream_type`. Kept as the number the stream used, for the reason `MediaTrack::codec`
  // keeps the four-character code: this module reports what the file claims.
  std::uint8_t stream_type = 0;
  TrackKind kind = TrackKind::Unknown;
  // The codec that stream type means, when it is one of the five ADR 0031 allows. Absent for
  // everything else -- MPEG-2 video, AC-3, a private stream, a type nobody has assigned -- which is
  // the refusal happening at the container rather than at the decoder.
  bool has_codec = false;
  CodecId codec = CodecId::H264;
};

// One piece of an access unit: a range in the input.
struct MpegTsRange {
  std::size_t offset = 0;
  std::size_t size = 0;
};

// One access unit, as **several** ranges.
//
// This is where a transport stream stops resembling the other two containers, and it took a failing
// test to see it. In MP4 and Matroska a sample is one contiguous range and `MediaSample` says so. In a
// transport stream an access unit is carried across however many 188-byte packets it takes, and every
// one of those packets has a **four-byte header in front of its payload** -- so the payload pieces are
// *never* adjacent in the file. A single-range `MediaSample` cannot describe one, and the first draft
// of this parser tried: it computed "are the pieces contiguous?", the answer was always no for
// anything spanning two packets, and it silently dropped every access unit larger than 184 bytes --
// which is every video frame in every real stream.
//
// So the pieces are a list, in order, and the caller concatenates them when it feeds a decoder. That
// keeps this module's line intact: it still reports *places* in the input and copies nothing.
struct MpegTsSample {
  std::uint16_t pid = 0;
  // 90kHz. `decode_time` is the DTS, or the PTS when the stream carries no DTS -- which is what the
  // specification says and what a stream with no reordering means.
  std::uint64_t decode_time = 0;
  std::uint64_t presentation_time = 0;
  std::vector<MpegTsRange> pieces;
  std::size_t total_size = 0;
};

struct MpegTsFile {
  std::vector<MpegTsStream> streams;
  // Access units, in the order their first packet appeared. One per PES packet, which for video is one
  // access unit and for audio is a small number of frames -- the stream decides, and this reports what
  // it found.
  std::vector<MpegTsSample> samples;
  // The first PCR seen, in 90kHz ticks, or absent. A player needs it to map a segment's timestamps
  // onto the playlist's timeline, because an HLS segment's PTS does not start at zero.
  bool has_first_pcr = false;
  std::uint64_t first_pcr = 0;
  // How many packets were dropped, and why. Signals rather than failures: a transport stream is
  // designed to be read through damage, and a segment with one bad packet plays with one gap.
  std::size_t resyncs = 0;
  std::size_t discontinuities = 0;
  std::size_t unreadable_packets = 0;
  // Set only when nothing at all could be made of the input -- no sync byte, or no PAT and PMT. A
  // caller distinguishes "not a transport stream" from "a damaged one".
  const char* error = nullptr;

  bool Ok() const { return error == nullptr; }
};

// A transport packet is 188 bytes. The other two legal sizes -- 192 with a 4-byte timestamp prefix
// (M2TS) and 204 with Reed-Solomon parity -- are **not** accepted, and that is a scope decision with
// a reason: HLS serves 188, and accepting three packet sizes means guessing which one a stream is
// from its content, which is the kind of guess that reads a stream at the wrong stride and produces
// plausible garbage.
inline constexpr std::size_t kTsPacketSize = 188;

// How many streams one program may have, and how many samples one parse may produce. Both bound
// memory against a stream that declares more than it contains, for the reason `kMaxSamples` exists in
// IsoBmff.h -- and here the numbers matter more, because a page controls how many segments it asks
// for.
inline constexpr std::size_t kMaxTsStreams = 32;
inline constexpr std::size_t kMaxTsSamples = 1u << 16;

// Timestamps are 33 bits at 90kHz.
inline constexpr std::uint64_t kTsClockHz = 90000;

MpegTsFile ParseMpegTs(std::span<const std::byte> bytes);

// True when `bytes` looks like a transport stream: a sync byte at zero and again one packet later.
//
// **Two, not one.** A single 0x47 at offset zero happens in a great many files by accident; a 0x47 at
// zero *and* at 188 is what a transport stream has and almost nothing else does. The right way to
// pick a demuxer, for the reason `LooksLikeIsoBmff` is.
bool LooksLikeMpegTs(std::span<const std::byte> bytes);

}  // namespace microbrowser::media

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "media/IsoBmff.h"

namespace microbrowser::media {

// WebM and Matroska, demuxed. ADR 0028 §2, and the other half of YouTube's DASH.
//
// EBML rather than ISO-BMFF boxes, and the difference is the whole reason this is a separate
// parser rather than a mode of the other one: an EBML element's *id* and *size* are both
// variable-length integers, a size may be declared **unknown**, and elements nest to arbitrary
// depth. Each of those is a hazard the box format does not have:
//
//   * A variable-length size means the length of the length comes from the file. Reading it wrong
//     shifts every element after it, so the reader refuses a malformed one rather than guessing.
//   * **An unknown size is legal and common** -- a live-streamed `Segment` has one, and so does a
//     `Cluster` in a file being written -- so it cannot be treated as an error. It means "until
//     the next element at this level or the end of the data", which is what this parser does.
//   * Arbitrary nesting needs a depth bound, for the reason ADR 0009's parse bound exists: a file
//     is a stranger's, and recursion on its structure is recursion it controls.
//
// The output is the same shape `IsoBmff` produces -- tracks, and samples as **byte ranges rather
// than bytes** -- because that is the ADR 0013 line this module lives on: it says what is in the
// file and where, and never what it decodes to. A demuxer that copied samples would be a demuxer
// holding a video in memory.
struct MatroskaFile {
  std::vector<MediaTrack> tracks;
  std::vector<MediaSample> samples;
  // `TimecodeScale`, in nanoseconds per tick. Matroska's default is 1,000,000 -- one millisecond --
  // and a file that omits it means that, which is why this is initialised rather than zero: a zero
  // here would make every timestamp in the file meaningless.
  std::uint64_t timecode_scale_ns = 1000000;
  // From `Info`, in scaled ticks. Zero when absent, which is legal for a live stream.
  double duration_ticks = 0.0;
  // Whether anything was refused. A file is usable with an unreadable cluster dropped -- that is
  // how a truncated download plays what arrived -- so this is a signal rather than a failure.
  bool had_refusals = false;
};

// Nothing when the bytes are not EBML at all. A WebM file begins with the EBML header magic, and a
// parser that skipped that check would walk arbitrary bytes as a variable-length integer tree.
std::optional<MatroskaFile> ParseMatroska(std::span<const std::byte> input);

// Whether these bytes start with the EBML header. Cheap, and the reason it is public: a media
// loader can tell a WebM from an MP4 without attempting either.
bool IsMatroska(std::span<const std::byte> input);

}  // namespace microbrowser::media

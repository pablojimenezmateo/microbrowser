#include "media/Matroska.h"

#include <algorithm>
#include <cstring>

#include "media/BoxReader.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"

namespace microbrowser::media {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The ids this parser knows. Written as the specification writes them -- with the marker bits still
// on -- because that is how they appear in a file and how every reference lists them, and stripping
// them here would mean two spellings of the same id.
constexpr std::uint64_t kEbmlHeader = 0x1A45DFA3;
constexpr std::uint64_t kSegment = 0x18538067;
constexpr std::uint64_t kInfo = 0x1549A966;
constexpr std::uint64_t kTimecodeScale = 0x2AD7B1;
constexpr std::uint64_t kDuration = 0x4489;
constexpr std::uint64_t kTracks = 0x1654AE6B;
constexpr std::uint64_t kTrackEntry = 0xAE;
constexpr std::uint64_t kTrackNumber = 0xD7;
constexpr std::uint64_t kTrackType = 0x83;
constexpr std::uint64_t kCodecId = 0x86;
constexpr std::uint64_t kCodecPrivate = 0x63A2;
constexpr std::uint64_t kVideo = 0xE0;
constexpr std::uint64_t kPixelWidth = 0xB0;
constexpr std::uint64_t kPixelHeight = 0xBA;
constexpr std::uint64_t kAudio = 0xE1;
constexpr std::uint64_t kSamplingFrequency = 0xB5;
constexpr std::uint64_t kChannels = 0x9F;
constexpr std::uint64_t kCluster = 0x1F43B675;
constexpr std::uint64_t kTimecode = 0xE7;
constexpr std::uint64_t kSimpleBlock = 0xA3;
constexpr std::uint64_t kBlockGroup = 0xA0;
constexpr std::uint64_t kBlock = 0xA1;

// Nesting depth, and the number is ADR 0009's argument rather than a guess: real files nest four
// deep (Segment/Cluster/BlockGroup/Block), and recursion on a structure a stranger controls needs a
// bound whatever the real depth is.
constexpr int kMaxDepth = 16;
// How many tracks and samples one file may describe. A file declaring a million samples is a file,
// not an attack -- but it is also not one this browser will hold, so the bound is a refusal with a
// reason rather than an allocation.
constexpr std::size_t kMaxTracks = 64;
constexpr std::size_t kMaxSamples = 200000;

// An EBML variable-length integer.
//
// The first byte's leading zeros say how many bytes follow: `1xxxxxxx` is one byte, `01xxxxxx` two,
// and so on to eight. `keep_marker` is the difference between reading an *id* and reading a *size*
// -- an id keeps the marker bit because that is what makes 0xAE and 0x2E different elements, and a
// size strips it because the marker is not part of the number. One function with a flag rather than
// two, so the length arithmetic cannot diverge between them.
struct Vint {
  std::uint64_t value = 0;
  int length = 0;
  bool all_ones = false;  // an unknown size, which is legal and means "until the next element"
};

bool ReadVint(BoxReader& reader, bool keep_marker, Vint& out) {
  const std::uint8_t first = reader.ReadU8();
  if (!reader.Ok() || first == 0) {
    // A zero first byte declares a length of more than eight bytes, which no EBML integer has.
    return false;
  }
  int length = 1;
  std::uint8_t mask = 0x80;
  while ((first & mask) == 0) {
    mask = static_cast<std::uint8_t>(mask >> 1);
    ++length;
  }
  std::uint64_t value = keep_marker ? first : static_cast<std::uint64_t>(first & ~mask);
  bool all_ones = (first & ~mask) == static_cast<std::uint8_t>(~mask & 0x7Fu) ||
                  (length == 1 && first == 0xFF);
  for (int i = 1; i < length; ++i) {
    const std::uint8_t byte = reader.ReadU8();
    if (!reader.Ok()) {
      return false;
    }
    value = (value << 8) | byte;
    all_ones = all_ones && byte == 0xFF;
  }
  out.value = value;
  out.length = length;
  // Only a *size* can be unknown, and only when every data bit is set.
  out.all_ones = !keep_marker && all_ones;
  return true;
}

// An unsigned integer of 1-8 bytes, big-endian, which is how EBML stores every number.
std::uint64_t ReadUint(BoxReader& reader, std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size && i < 8; ++i) {
    value = (value << 8) | reader.ReadU8();
  }
  return value;
}

// An 4- or 8-byte float, which is how `SamplingFrequency` and `Duration` are stored.
double ReadFloat(BoxReader& reader, std::size_t size) {
  if (size == 4) {
    const std::uint32_t bits = static_cast<std::uint32_t>(ReadUint(reader, 4));
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  if (size == 8) {
    const std::uint64_t bits = ReadUint(reader, 8);
    double value = 0.0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  // Any other width is not a float. Zero rather than a guess: a wrong sample rate is audio at the
  // wrong speed, which sounds like a broken decoder.
  return 0.0;
}

// Matroska's track types. 1 and 2 are the only ones this browser has anything to do with; the
// others -- complex, logo, subtitle, buttons -- are kept as Unknown rather than mapped, because a
// subtitle track presented as audio is a track a player would try to decode.
TrackKind KindFromType(std::uint64_t type) {
  if (type == 1) {
    return TrackKind::Video;
  }
  if (type == 2) {
    return TrackKind::Audio;
  }
  return TrackKind::Unknown;
}

struct ParseContext {
  std::span<const std::byte> file;
  MatroskaFile* out = nullptr;
  std::uint64_t cluster_timecode = 0;
};

void ParseElements(ParseContext& context, std::size_t begin, std::size_t end, int depth,
                   MediaTrack* track);

// One `TrackEntry`'s children, and then the track it describes.
void ParseTrackEntry(ParseContext& context, std::size_t begin, std::size_t end, int depth) {
  if (context.out->tracks.size() >= kMaxTracks) {
    context.out->had_refusals = true;
    return;
  }
  MediaTrack track;
  // Matroska timestamps are in `TimecodeScale` units for the whole file rather than per track, so
  // every track's timescale is the file's. Filled here rather than left zero because a zero
  // timescale makes every duration in the file meaningless -- callers check for exactly that.
  track.timescale = context.out->timecode_scale_ns == 0
                        ? 0u
                        : static_cast<std::uint32_t>(1000000000ull / context.out->timecode_scale_ns);
  ParseElements(context, begin, end, depth, &track);
  if (track.id != 0 && track.kind != TrackKind::Unknown) {
    context.out->tracks.push_back(std::move(track));
  } else {
    // A track with no number, or one this browser does not handle. Dropped rather than kept as a
    // placeholder: a caller iterating tracks would otherwise have to know which ones are real.
    context.out->had_refusals = true;
  }
}

void ParseElements(ParseContext& context, std::size_t begin, std::size_t end, int depth,
                   MediaTrack* track) {
  if (depth >= kMaxDepth) {
    context.out->had_refusals = true;
    return;
  }
  std::size_t at = begin;
  while (at < end) {
    BoxReader reader(context.file.subspan(at, end - at));
    Vint id;
    Vint size;
    if (!ReadVint(reader, true, id) || !ReadVint(reader, false, size)) {
      // A malformed length means every element after it is at an unknown offset. Stopping is the
      // only safe answer -- resynchronising would be guessing where the next element starts.
      context.out->had_refusals = true;
      return;
    }
    const std::size_t header = static_cast<std::size_t>(id.length + size.length);
    const std::size_t payload = at + header;
    std::size_t payload_end = 0;
    if (size.all_ones) {
      // An unknown size: legal, and common in a live stream. It runs to the end of the enclosing
      // element, which for the top-level `Segment` is the end of the file.
      payload_end = end;
    } else {
      payload_end = util::SaturatingAdd(payload, static_cast<std::size_t>(size.value));
      if (payload_end > end) {
        // A declared size past the end of its parent. The element is truncated -- a partial
        // download -- so what arrived before it is kept and this is not.
        context.out->had_refusals = true;
        return;
      }
    }

    switch (id.value) {
      case kSegment:
      case kInfo:
      case kTracks:
      case kVideo:
      case kAudio:
        ParseElements(context, payload, payload_end, depth + 1, track);
        break;
      case kTrackEntry:
        ParseTrackEntry(context, payload, payload_end, depth + 1);
        break;
      case kCluster:
        // A cluster's timecode is the base for every block in it, and it is stored *inside* the
        // cluster before the blocks -- so the context carries it and the blocks add to it.
        context.cluster_timecode = 0;
        ParseElements(context, payload, payload_end, depth + 1, track);
        break;
      case kBlockGroup:
        ParseElements(context, payload, payload_end, depth + 1, track);
        break;
      case kTimecodeScale: {
        BoxReader value(context.file.subspan(payload, payload_end - payload));
        const std::uint64_t scale = ReadUint(value, payload_end - payload);
        // Zero would make every timestamp meaningless, so the default stands instead.
        if (scale != 0) {
          context.out->timecode_scale_ns = scale;
        }
        break;
      }
      case kDuration: {
        BoxReader value(context.file.subspan(payload, payload_end - payload));
        context.out->duration_ticks = ReadFloat(value, payload_end - payload);
        break;
      }
      case kTimecode: {
        BoxReader value(context.file.subspan(payload, payload_end - payload));
        context.cluster_timecode = ReadUint(value, payload_end - payload);
        break;
      }
      case kTrackNumber:
      case kTrackType:
      case kCodecId:
      case kCodecPrivate:
      case kPixelWidth:
      case kPixelHeight:
      case kSamplingFrequency:
      case kChannels: {
        if (track == nullptr) {
          break;  // a track property outside a TrackEntry describes nothing
        }
        BoxReader value(context.file.subspan(payload, payload_end - payload));
        const std::size_t length = payload_end - payload;
        if (id.value == kTrackNumber) {
          track->id = static_cast<std::uint32_t>(ReadUint(value, length));
        } else if (id.value == kTrackType) {
          track->kind = KindFromType(ReadUint(value, length));
        } else if (id.value == kCodecId) {
          const std::span<const std::byte> bytes = context.file.subspan(payload, length);
          track->codec.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        } else if (id.value == kCodecPrivate) {
          // A byte range, not a copy: this module does not know what is in a codec's
          // configuration record and must not pretend to. Same rule as a sample.
          track->codec_config_offset = payload;
          track->codec_config_size = length;
        } else if (id.value == kPixelWidth) {
          track->width = static_cast<std::uint16_t>(std::min<std::uint64_t>(
              ReadUint(value, length), 16384));
        } else if (id.value == kPixelHeight) {
          track->height = static_cast<std::uint16_t>(std::min<std::uint64_t>(
              ReadUint(value, length), 16384));
        } else if (id.value == kSamplingFrequency) {
          const double rate = ReadFloat(value, length);
          track->sample_rate = rate > 0.0 && rate < 1e7 ? static_cast<std::uint32_t>(rate) : 0u;
        } else {
          track->channels = static_cast<std::uint16_t>(std::min<std::uint64_t>(
              ReadUint(value, length), 64));
        }
        break;
      }
      case kSimpleBlock:
      case kBlock: {
        // A block's header is a track number (a variable-length integer), a 16-bit timecode
        // relative to the cluster's, and one byte of flags. The frame data follows, and it is
        // reported as a *range* -- the whole point of this module.
        BoxReader block(context.file.subspan(payload, payload_end - payload));
        Vint track_number;
        if (!ReadVint(block, false, track_number)) {
          context.out->had_refusals = true;
          break;
        }
        const std::uint16_t relative = block.ReadU16();
        const std::uint8_t flags = block.ReadU8();
        if (!block.Ok()) {
          context.out->had_refusals = true;
          break;
        }
        const std::size_t frame_offset = payload + static_cast<std::size_t>(track_number.length) + 3u;
        if (frame_offset > payload_end || context.out->samples.size() >= kMaxSamples) {
          context.out->had_refusals = true;
          break;
        }
        MediaSample sample;
        sample.track_id = static_cast<std::uint32_t>(track_number.value);
        sample.offset = frame_offset;
        sample.size = payload_end - frame_offset;
        // Signed 16-bit, and the sign matters: a block may precede its cluster's timecode, and
        // reading it unsigned puts that frame 65 seconds into the future.
        sample.decode_time = context.cluster_timecode +
                             static_cast<std::uint64_t>(static_cast<std::int64_t>(
                                 static_cast<std::int16_t>(relative)));
        // Bit 7 is the keyframe flag on a SimpleBlock. A `Block` inside a `BlockGroup` has no such
        // flag -- its keyframe-ness is the absence of a `ReferenceBlock` -- so it is reported as
        // not a sync sample rather than guessed, which is the answer that makes a seek land on a
        // frame a decoder can start from.
        sample.is_sync = id.value == kSimpleBlock && (flags & 0x80u) != 0;
        context.out->samples.push_back(sample);
        break;
      }
      default:
        break;  // an element this parser does not read, skipped by its declared size
    }

    if (size.all_ones) {
      // An unknown-size element consumed the rest of its parent by definition.
      return;
    }
    const std::size_t next = util::SaturatingAdd(at, header + static_cast<std::size_t>(size.value));
    if (next <= at) {
      // A zero-length element with a zero-length header cannot exist, and a wrapped offset must
      // not become an infinite loop.
      context.out->had_refusals = true;
      return;
    }
    at = next;
  }
}

}  // namespace

bool IsMatroska(std::span<const std::byte> input) {
  if (input.size() < 4) {
    return false;
  }
  BoxReader reader(input);
  return reader.ReadU32() == 0x1A45DFA3u && reader.Ok();
}

std::optional<MatroskaFile> ParseMatroska(std::span<const std::byte> input) {
  if (!IsMatroska(input)) {
    // Without this check, arbitrary bytes are walked as a variable-length integer tree -- which
    // terminates, but only after reporting tracks and samples that are not there.
    AddPerformanceCounter(PerfCounterId::MatroskaRefusals);
    return std::nullopt;
  }
  MatroskaFile file;
  ParseContext context{input, &file, 0};
  ParseElements(context, 0, input.size(), 0, nullptr);
  AddPerformanceCounter(PerfCounterId::MatroskaFilesParsed);
  return file;
}

}  // namespace microbrowser::media

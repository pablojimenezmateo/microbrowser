#include "media/IsoBmff.h"

#include "media/BoxReader.h"

namespace microbrowser::media {

namespace {

// Each optional per-sample field in a `trun` entry is one uint32.
constexpr std::uint64_t kBytesPerTrunField = 4;

// The container boxes this walk descends into. Everything else is skipped
// whole, which is the behaviour the format is designed for: an unknown box is
// not an error, it is a box for a feature this parser does not implement.
bool IsContainer(const std::string& type) {
  return type == "moov" || type == "trak" || type == "mdia" || type == "minf" ||
         type == "stbl" || type == "moof" || type == "traf" || type == "mvex" ||
         type == "edts";
}

// Version and flags, which every "full box" begins with.
struct FullBox {
  std::uint8_t version = 0;
  std::uint32_t flags = 0;
};

FullBox ReadFullBox(BoxReader& reader) {
  FullBox full;
  full.version = reader.ReadU8();
  full.flags = reader.ReadU24();
  return full;
}

// The state threaded through the walk. Separate from IsoBmffFile because some
// of it is scratch that the result has no business carrying -- which track is
// currently being built, and the default sample values a `tfhd` sets for the
// `trun`s after it.
// No track is being assembled.
constexpr std::size_t kNoTrack = static_cast<std::size_t>(-1);

struct ParseState {
  IsoBmffFile* out = nullptr;

  // The track being assembled by the moov walk, **as an index and never as a
  // pointer**.
  //
  // A pointer here is a use-after-free reachable from a file, and the fuzzer
  // found it within two minutes of the parser existing: the walk takes the
  // track it is filling in, then recurses into that track's children, and a
  // `trak` nested inside a `trak` appends to the same vector and reallocates it
  // out from under whatever was held. An index cannot dangle, and resolving it
  // at each use costs an add.
  std::size_t current_track = kNoTrack;

  // Fragment defaults, from `tfhd`. Reset per `traf`.
  std::uint32_t fragment_track_id = 0;
  std::uint64_t fragment_base_offset = 0;
  bool fragment_base_offset_present = false;
  std::uint32_t default_sample_duration = 0;
  std::uint32_t default_sample_size = 0;
  std::uint64_t fragment_decode_time = 0;
  // Where the enclosing `moof` box started in the file. `trun` offsets are
  // relative to it unless `tfhd` said otherwise, and getting this wrong points
  // every sample range at the wrong bytes.
  std::size_t moof_offset = 0;
};

// The track being assembled, resolved now rather than held. Null when there is
// none, which is every box outside a `trak`.
MediaTrack* CurrentTrack(const ParseState& state) {
  if (state.out == nullptr || state.current_track >= state.out->tracks.size()) {
    return nullptr;
  }
  return &state.out->tracks[state.current_track];
}

void ParseTkhd(BoxReader& reader, ParseState& state) {
  MediaTrack* track = CurrentTrack(state);
  if (track == nullptr) {
    return;
  }
  const FullBox full = ReadFullBox(reader);
  if (full.version == 1) {
    reader.Skip(16);  // creation and modification times
    track->id = reader.ReadU32();
  } else {
    reader.Skip(8);
    track->id = reader.ReadU32();
  }
}

void ParseMdhd(BoxReader& reader, ParseState& state) {
  MediaTrack* track = CurrentTrack(state);
  if (track == nullptr) {
    return;
  }
  const FullBox full = ReadFullBox(reader);
  if (full.version == 1) {
    reader.Skip(16);
    track->timescale = reader.ReadU32();
    track->duration = reader.ReadU64();
  } else {
    reader.Skip(8);
    track->timescale = reader.ReadU32();
    track->duration = reader.ReadU32();
  }
}

void ParseHdlr(BoxReader& reader, ParseState& state) {
  MediaTrack* track = CurrentTrack(state);
  if (track == nullptr) {
    return;
  }
  ReadFullBox(reader);
  reader.Skip(4);  // pre_defined
  const std::string handler = reader.ReadFourCC();
  if (handler == "vide") {
    track->kind = TrackKind::Video;
  } else if (handler == "soun") {
    track->kind = TrackKind::Audio;
  }
  // Anything else -- "hint", "meta", "subt" -- stays Unknown rather than being
  // guessed at. A caller that cannot identify a track must not play it.
}

// The sample entry inside `stsd`, which is where the codec finally appears.
//
// Its layout depends on the handler rather than on anything in the box itself,
// which is why this takes the track's kind: a video entry and an audio entry
// have different fixed parts and reading one as the other lands the codec
// configuration offset in the middle of a field.
void ParseSampleEntry(BoxReader& reader, const BoxHeader& header, ParseState& state,
                      std::size_t entry_payload_offset) {
  MediaTrack* track = CurrentTrack(state);
  if (track == nullptr) {
    return;
  }
  track->codec = header.type;

  reader.Skip(6);  // reserved
  reader.Skip(2);  // data_reference_index

  if (track->kind == TrackKind::Video) {
    reader.Skip(16);  // pre_defined and reserved
    track->width = reader.ReadU16();
    track->height = reader.ReadU16();
    reader.Skip(14);  // resolutions, reserved, frame_count
    reader.Skip(32);  // compressorname
    reader.Skip(4);   // depth and pre_defined
  } else if (track->kind == TrackKind::Audio) {
    reader.Skip(8);  // reserved
    track->channels = reader.ReadU16();
    track->sample_size = reader.ReadU16();
    reader.Skip(4);  // pre_defined and reserved
    // 16.16 fixed point; the fractional half is always zero in practice and is
    // dropped rather than rounded, because a sample rate is an integer.
    track->sample_rate = reader.ReadU32() >> 16;
  } else {
    // An entry for a handler this does not model. The codec code is still worth
    // reporting -- it is what a caller needs to say "cannot play this" about --
    // but nothing after the fixed part can be located, so stop.
    return;
  }

  // What follows is the codec configuration box, whose type depends on the
  // codec. Recorded as a range rather than parsed: what is inside it is the
  // decoder's business, and this module cannot name a type to hold it.
  while (reader.Ok() && reader.Remaining() >= 8) {
    const std::size_t child_offset = reader.Offset();
    const BoxHeader child = ReadBoxHeader(reader);
    if (!child.valid) {
      break;
    }
    const std::size_t payload_at = entry_payload_offset + reader.Offset();
    if (child.type == "avcC" || child.type == "hvcC" || child.type == "vpcC" ||
        child.type == "av1C" || child.type == "esds" || child.type == "dOps") {
      track->codec_config_offset = payload_at;
      track->codec_config_size = static_cast<std::size_t>(child.payload_size);
    }
    (void)child_offset;
    if (!reader.Skip(child.payload_size)) {
      break;
    }
  }
}

void ParseStsd(BoxReader& reader, ParseState& state, std::size_t payload_offset) {
  ReadFullBox(reader);
  const std::uint32_t count = reader.ReadU32();
  if (!reader.Ok() || count == 0) {
    return;
  }
  // Only the first entry. A track with several sample descriptions switches
  // codec partway through, which is a case no target needs and which this
  // deliberately does not pretend to handle.
  const std::size_t entry_at = reader.Offset();
  const BoxHeader entry = ReadBoxHeader(reader);
  if (!entry.valid) {
    return;
  }
  BoxReader body = reader.Sub(entry.payload_size);
  ParseSampleEntry(body, entry, state, payload_offset + reader.Offset() -
                                           static_cast<std::size_t>(entry.payload_size));
  (void)entry_at;
}

void ParseTfhd(BoxReader& reader, ParseState& state) {
  const FullBox full = ReadFullBox(reader);
  state.fragment_track_id = reader.ReadU32();

  constexpr std::uint32_t kBaseDataOffsetPresent = 0x000001;
  constexpr std::uint32_t kSampleDescriptionIndexPresent = 0x000002;
  constexpr std::uint32_t kDefaultSampleDurationPresent = 0x000008;
  constexpr std::uint32_t kDefaultSampleSizePresent = 0x000010;
  constexpr std::uint32_t kDefaultSampleFlagsPresent = 0x000020;

  if ((full.flags & kBaseDataOffsetPresent) != 0) {
    state.fragment_base_offset = reader.ReadU64();
    state.fragment_base_offset_present = true;
  }
  if ((full.flags & kSampleDescriptionIndexPresent) != 0) {
    reader.Skip(4);
  }
  if ((full.flags & kDefaultSampleDurationPresent) != 0) {
    state.default_sample_duration = reader.ReadU32();
  }
  if ((full.flags & kDefaultSampleSizePresent) != 0) {
    state.default_sample_size = reader.ReadU32();
  }
  if ((full.flags & kDefaultSampleFlagsPresent) != 0) {
    reader.Skip(4);
  }
}

void ParseTfdt(BoxReader& reader, ParseState& state) {
  const FullBox full = ReadFullBox(reader);
  state.fragment_decode_time = full.version == 1 ? reader.ReadU64() : reader.ReadU32();
}

void ParseTrun(BoxReader& reader, ParseState& state, std::size_t file_size) {
  const FullBox full = ReadFullBox(reader);
  const std::uint32_t count = reader.ReadU32();
  if (!reader.Ok()) {
    return;
  }
  constexpr std::uint32_t kDataOffsetPresent = 0x000001;
  constexpr std::uint32_t kFirstSampleFlagsPresent = 0x000004;
  constexpr std::uint32_t kSampleDurationPresent = 0x000100;
  constexpr std::uint32_t kSampleSizePresent = 0x000200;
  constexpr std::uint32_t kSampleFlagsPresent = 0x000400;
  constexpr std::uint32_t kSampleCompositionOffsetPresent = 0x000800;

  // How many bytes each entry actually occupies, which the flags decide and
  // nothing else does.
  //
  // **This can legitimately be zero.** A fragment whose `tfhd` gave a default
  // duration and size, and which needs no per-sample flags, writes a count and
  // then no entries at all -- which is the common shape for constant-bitrate
  // audio and is what a real DASH segment looks like. Bounding the count
  // against "at least four bytes per entry" therefore rejects a legal file,
  // which is how this was found: the fragment test failed, not a fuzzer.
  std::uint64_t bytes_per_entry = 0;
  for (const std::uint32_t present :
       {kSampleDurationPresent, kSampleSizePresent, kSampleFlagsPresent,
        kSampleCompositionOffsetPresent}) {
    if ((full.flags & present) != 0) {
      bytes_per_entry += kBytesPerTrunField;
    }
  }
  // When the entries do occupy bytes, the count is checked against the ones
  // that remain before anything is reserved: a `trun` claiming four billion
  // samples is four bytes on the wire and gigabytes of vector otherwise.
  if (bytes_per_entry > 0 &&
      static_cast<std::uint64_t>(count) * bytes_per_entry > reader.Remaining()) {
    reader.Fail();
    return;
  }
  // And when they do not, this is the only bound there is -- which is why it is
  // checked unconditionally rather than as a second line of defence.
  if (state.out->samples.size() + count > kMaxSamples) {
    reader.Fail();
    return;
  }

  // Absolute in the file. The base is the enclosing `moof`'s own offset unless
  // `tfhd` gave one, which is the rule that makes a fragment relocatable.
  std::uint64_t cursor =
      state.fragment_base_offset_present ? state.fragment_base_offset : state.moof_offset;
  if ((full.flags & kDataOffsetPresent) != 0) {
    // Signed, and legitimately negative: a `trun` may point back at data that
    // precedes it. Added in 64 bits so that a negative offset larger than the
    // base saturates to "outside the file" rather than wrapping to a huge one.
    const std::int64_t data_offset = reader.ReadI32();
    const std::int64_t based = static_cast<std::int64_t>(cursor) + data_offset;
    if (based < 0) {
      reader.Fail();
      return;
    }
    cursor = static_cast<std::uint64_t>(based);
  }
  std::uint32_t first_flags = 0;
  const bool has_first_flags = (full.flags & kFirstSampleFlagsPresent) != 0;
  if (has_first_flags) {
    first_flags = reader.ReadU32();
  }

  std::uint64_t decode_time = state.fragment_decode_time;
  for (std::uint32_t i = 0; i < count && reader.Ok(); ++i) {
    MediaSample sample;
    sample.track_id = state.fragment_track_id;

    const std::uint32_t duration = (full.flags & kSampleDurationPresent) != 0
                                       ? reader.ReadU32()
                                       : state.default_sample_duration;
    const std::uint32_t size =
        (full.flags & kSampleSizePresent) != 0 ? reader.ReadU32() : state.default_sample_size;
    std::uint32_t flags = 0;
    if ((full.flags & kSampleFlagsPresent) != 0) {
      flags = reader.ReadU32();
    } else if (i == 0 && has_first_flags) {
      flags = first_flags;
    }
    if ((full.flags & kSampleCompositionOffsetPresent) != 0) {
      reader.Skip(4);
    }
    if (!reader.Ok()) {
      return;
    }

    // The range must be inside the file. Both halves are 64-bit here precisely
    // because `offset + size` in a size_t is the overflow that turns a bounds
    // check into a pass -- this is the check whose absence hands a decoder a
    // pointer past the end of the buffer.
    if (cursor > static_cast<std::uint64_t>(file_size) ||
        static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(file_size) - cursor) {
      reader.Fail();
      return;
    }

    sample.offset = static_cast<std::size_t>(cursor);
    sample.size = size;
    sample.duration = duration;
    sample.decode_time = decode_time;
    // Bit 16 of the sample flags is `sample_is_non_sync_sample`. A sync sample
    // is where playback can start, so reading this backwards means seeking to a
    // frame no decoder can begin from.
    sample.is_sync = (flags & 0x00010000u) == 0;

    state.out->samples.push_back(sample);
    cursor += size;
    decode_time += duration;
  }
}

void Walk(BoxReader& reader, ParseState& state, int depth, std::size_t base_offset,
          std::size_t file_size) {
  if (depth > kMaxBoxDepth) {
    reader.Fail();
    return;
  }
  while (reader.Ok() && reader.Remaining() >= 8) {
    const std::size_t box_start = base_offset + reader.Offset();
    const BoxHeader header = ReadBoxHeader(reader);
    if (!header.valid) {
      return;
    }
    const std::size_t payload_offset = base_offset + reader.Offset();
    BoxReader body = reader.Sub(header.payload_size);
    if (!reader.Ok()) {
      return;
    }

    if (header.type == "ftyp") {
      state.out->major_brand = body.ReadFourCC();
    } else if (header.type == "trak") {
      if (state.out->tracks.size() >= kMaxTracks) {
        reader.Fail();
        return;
      }
      state.out->tracks.emplace_back();
      // An index, saved and restored around the recursion. A nested `trak`
      // appends to this same vector, so anything held across the call that is
      // not an index is dangling by the time the call returns.
      const std::size_t outer = state.current_track;
      state.current_track = state.out->tracks.size() - 1;
      Walk(body, state, depth + 1, payload_offset, file_size);
      state.current_track = outer;
      if (!body.Ok()) {
        reader.Fail();
        return;
      }
      continue;
    } else if (header.type == "moof") {
      state.moof_offset = box_start;
      Walk(body, state, depth + 1, payload_offset, file_size);
    } else if (header.type == "traf") {
      // Defaults are per-fragment, so they are cleared on the way in rather
      // than left over from the previous one. A `traf` that omits a default
      // inherits zero, not whatever the last fragment happened to set.
      state.fragment_base_offset = 0;
      state.fragment_base_offset_present = false;
      state.default_sample_duration = 0;
      state.default_sample_size = 0;
      state.fragment_decode_time = 0;
      Walk(body, state, depth + 1, payload_offset, file_size);
    } else if (IsContainer(header.type)) {
      Walk(body, state, depth + 1, payload_offset, file_size);
    } else if (header.type == "tkhd") {
      ParseTkhd(body, state);
    } else if (header.type == "mdhd") {
      ParseMdhd(body, state);
    } else if (header.type == "hdlr") {
      ParseHdlr(body, state);
    } else if (header.type == "stsd") {
      ParseStsd(body, state, payload_offset);
    } else if (header.type == "tfhd") {
      ParseTfhd(body, state);
    } else if (header.type == "tfdt") {
      ParseTfdt(body, state);
    } else if (header.type == "trun") {
      ParseTrun(body, state, file_size);
    }
    // Anything else is skipped whole. `reader.Sub` already advanced past it,
    // which is what makes an unknown box cost nothing rather than being an
    // error -- that is the extensibility the format is built on.

    if (!body.Ok()) {
      // A malformed child fails the whole parse rather than being ignored. A
      // half-read box means the offsets after it are guesses, and a demuxer
      // that hands a decoder a guessed range is the bug this module exists to
      // not have.
      reader.Fail();
      return;
    }
  }
}

}  // namespace

IsoBmffFile ParseIsoBmff(std::span<const std::byte> bytes) {
  IsoBmffFile file;
  if (bytes.empty()) {
    file.error = "empty";
    return file;
  }

  BoxReader reader(bytes);
  ParseState state;
  state.out = &file;
  Walk(reader, state, 0, 0, bytes.size());

  if (!reader.Ok()) {
    file.error = "malformed box structure";
    return file;
  }
  if (file.tracks.empty() && file.samples.empty()) {
    // Structurally fine and describing nothing. Reported as an error because
    // every caller of this wants one or the other, and "parsed successfully,
    // found nothing" is a result that gets checked less often than it should.
    file.error = "no tracks or samples";
    return file;
  }
  return file;
}

bool LooksLikeIsoBmff(std::span<const std::byte> bytes) {
  BoxReader reader(bytes);
  if (reader.Remaining() < 8) {
    return false;
  }
  reader.ReadU32();
  const std::string type = reader.ReadFourCC();
  if (!reader.Ok()) {
    return false;
  }
  // The types a file of this family legitimately begins with. `styp` is a DASH
  // media segment and `moof` a fragment with no segment header, both of which
  // arrive without an `ftyp` of their own.
  return type == "ftyp" || type == "styp" || type == "moov" || type == "moof" ||
         type == "skip" || type == "free";
}

}  // namespace microbrowser::media

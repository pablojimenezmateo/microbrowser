#include "media/MpegTs.h"

#include <algorithm>
#include <map>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::media {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

std::uint8_t At(std::span<const std::byte> bytes, std::size_t index) {
  return static_cast<std::uint8_t>(bytes[index]);
}

// The PMT's `stream_type`, mapped to what this browser can decode.
//
// **The refusal happens here, at the container**, which is ADR 0013's whole argument for owning this
// layer: the stream type is the only thing that decides what the decoder will be asked to decode, so
// a type outside the allowlist is refused before a decoder exists to be handed it. MPEG-2 video
// (0x02), AC-3 (0x81) and the private streams are all common in broadcast transport streams and none
// of them is in ADR 0031's five.
bool CodecForStreamType(std::uint8_t stream_type, TrackKind& kind, CodecId& codec) {
  switch (stream_type) {
    case 0x1B:  // H.264 in a PES
      kind = TrackKind::Video;
      codec = CodecId::H264;
      return true;
    case 0x0F:  // AAC in ADTS
    case 0x11:  // AAC in LATM
      kind = TrackKind::Audio;
      codec = CodecId::Aac;
      return true;
    case 0xA1:  // Opus, as the HLS extension assigns it
      kind = TrackKind::Audio;
      codec = CodecId::Opus;
      return true;
    default:
      break;
  }
  // The kind is still worth reporting for a type this browser cannot decode: a caller that finds an
  // MPEG-2 video stream and no H.264 one knows *why* it cannot play, which is a better answer than an
  // empty stream list.
  switch (stream_type) {
    case 0x01:
    case 0x02:
    case 0x10:
    case 0x24:
    case 0x27:
      kind = TrackKind::Video;
      break;
    case 0x03:
    case 0x04:
    case 0x81:
    case 0x87:
      kind = TrackKind::Audio;
      break;
    default:
      kind = TrackKind::Unknown;
      break;
  }
  return false;
}

// A 33-bit PTS or DTS out of the five bytes a PES header spends on one.
//
// The layout is the specification's and it is deliberately awkward: four bits of flag, three bits of
// timestamp, a marker bit, fifteen bits, a marker, fifteen more. The marker bits are *checked* rather
// than skipped, because a header whose markers are wrong is not a header -- and reading a timestamp
// out of one produces a plausible number at the wrong time, which is the failure this whole file is
// arranged to avoid.
bool ReadTimestamp(std::span<const std::byte> bytes, std::size_t at, std::uint64_t& out) {
  if (at + 5 > bytes.size()) {
    return false;
  }
  const std::uint8_t b0 = At(bytes, at);
  const std::uint8_t b1 = At(bytes, at + 1);
  const std::uint8_t b2 = At(bytes, at + 2);
  const std::uint8_t b3 = At(bytes, at + 3);
  const std::uint8_t b4 = At(bytes, at + 4);
  if ((b2 & 0x01u) == 0 || (b4 & 0x01u) == 0) {
    return false;  // the two marker bits
  }
  out = (static_cast<std::uint64_t>(b0 & 0x0Eu) << 29) |
        (static_cast<std::uint64_t>(b1) << 22) |
        (static_cast<std::uint64_t>(b2 & 0xFEu) << 14) |
        (static_cast<std::uint64_t>(b3) << 7) |
        (static_cast<std::uint64_t>(b4) >> 1);
  return true;
}

// A PES packet being reassembled from however many transport packets it took.
//
// The pieces are a *list*, because they are never adjacent: every packet puts four bytes of header in
// front of its payload. See the note on `MpegTsSample`.
struct Assembly {
  std::vector<MpegTsRange> pieces;
  std::size_t total = 0;
  int continuity = -1;
  bool have_pts = false;
  std::uint64_t pts = 0;
  std::uint64_t dts = 0;
};

// The tables, as the state a stream builds up. `program_map_pids` is a set because a PAT may name
// several programs and a stream may carry several PMTs; every one of them is read, and the *first*
// program's streams are what a player gets -- which is what every player does with a multi-program
// stream, and saying so is better than picking the last one silently.
struct Tables {
  std::vector<std::uint16_t> program_map_pids;
  std::map<std::uint16_t, MpegTsStream> streams;
  bool saw_pat = false;
  bool saw_pmt = false;
};

// A PSI section, which is what a PAT and a PMT both are. The pointer field, the section length, and
// the CRC at the end are all read from the stream, so all three are bounded against what is there.
bool SectionOf(std::span<const std::byte> bytes, std::size_t payload_at, std::size_t payload_end,
               bool payload_start, std::span<const std::byte>& section) {
  if (!payload_start || payload_at >= payload_end) {
    // A section continued from an earlier packet. Not assembled: a PAT or a PMT that does not fit in
    // one packet is legal and vanishingly rare (both are tens of bytes against 184 available), and
    // assembling them would be a second reassembly path for a case no HLS segment produces. Skipped
    // rather than mis-read.
    return false;
  }
  const std::size_t pointer = At(bytes, payload_at);
  const std::size_t table_at = payload_at + 1 + pointer;
  if (table_at + 3 > payload_end) {
    return false;
  }
  const std::size_t length =
      (static_cast<std::size_t>(At(bytes, table_at + 1) & 0x0Fu) << 8) | At(bytes, table_at + 2);
  const std::size_t section_end = table_at + 3 + length;
  if (length < 5 || section_end > payload_end) {
    return false;
  }
  section = bytes.subspan(table_at, section_end - table_at);
  return true;
}

void ReadPat(std::span<const std::byte> section, Tables& tables) {
  if (section.empty() || At(section, 0) != 0x00) {
    return;  // not a program association table
  }
  // 8 bytes of header, then four per program, then four of CRC.
  const std::size_t length =
      (static_cast<std::size_t>(At(section, 1) & 0x0Fu) << 8) | At(section, 2);
  const std::size_t entries_end = 3 + length >= 4 ? 3 + length - 4 : 0;
  for (std::size_t at = 8; at + 4 <= entries_end && at + 4 <= section.size(); at += 4) {
    const std::uint16_t program = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(At(section, at)) << 8) | At(section, at + 1));
    const std::uint16_t pid = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(At(section, at + 2)) & 0x1Fu) << 8) | At(section, at + 3));
    if (program == 0) {
      continue;  // the network information table, which carries no media
    }
    if (std::find(tables.program_map_pids.begin(), tables.program_map_pids.end(), pid) ==
        tables.program_map_pids.end()) {
      tables.program_map_pids.push_back(pid);
    }
  }
  tables.saw_pat = !tables.program_map_pids.empty();
}

void ReadPmt(std::span<const std::byte> section, Tables& tables) {
  if (section.size() < 12 || At(section, 0) != 0x02) {
    return;  // not a program map table
  }
  const std::size_t length =
      (static_cast<std::size_t>(At(section, 1) & 0x0Fu) << 8) | At(section, 2);
  const std::size_t entries_end = 3 + length >= 4 ? 3 + length - 4 : 0;
  const std::size_t program_info_length =
      (static_cast<std::size_t>(At(section, 10) & 0x0Fu) << 8) | At(section, 11);
  std::size_t at = 12 + program_info_length;
  while (at + 5 <= entries_end && at + 5 <= section.size()) {
    const std::uint8_t stream_type = At(section, at);
    const std::uint16_t pid = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(At(section, at + 1)) & 0x1Fu) << 8) | At(section, at + 2));
    const std::size_t descriptors =
        (static_cast<std::size_t>(At(section, at + 3) & 0x0Fu) << 8) | At(section, at + 4);
    at += 5 + descriptors;
    if (tables.streams.size() >= kMaxTsStreams) {
      break;
    }
    MpegTsStream stream;
    stream.pid = pid;
    stream.stream_type = stream_type;
    stream.has_codec = CodecForStreamType(stream_type, stream.kind, stream.codec);
    // A repeated PMT may rename a PID's type mid-stream. The later one wins, which is what a stream
    // that switches representation means by it.
    tables.streams[pid] = stream;
  }
  tables.saw_pmt = !tables.streams.empty();
}

}  // namespace

bool LooksLikeMpegTs(std::span<const std::byte> bytes) {
  if (bytes.size() < kTsPacketSize + 1) {
    return false;
  }
  return At(bytes, 0) == 0x47 && At(bytes, kTsPacketSize) == 0x47;
}

MpegTsFile ParseMpegTs(std::span<const std::byte> bytes) {
  MpegTsFile file;
  if (bytes.size() < kTsPacketSize) {
    file.error = "too short for one transport packet";
    return file;
  }

  // Resynchronisation before anything else. A stream may start mid-packet -- a live tune-in does, and
  // so does a range request -- so the parser finds a sync byte with another one a packet later rather
  // than requiring one at zero. Requiring zero would refuse a stream every player accepts.
  std::size_t at = 0;
  while (at + kTsPacketSize < bytes.size() &&
         !(At(bytes, at) == 0x47 && At(bytes, at + kTsPacketSize) == 0x47)) {
    ++at;
  }
  if (at + kTsPacketSize > bytes.size()) {
    file.error = "no transport packet sync";
    return file;
  }
  if (at != 0) {
    ++file.resyncs;
  }

  Tables tables;
  std::map<std::uint16_t, Assembly> assembling;

  const auto flush = [&file, &tables](std::uint16_t pid, Assembly& assembly) {
    // A PES with no timestamp cannot be placed on a timeline, so it is dropped and counted. Everything
    // else about it may be fine; without a PTS there is nowhere to put it.
    if (assembly.pieces.empty() || !assembly.have_pts) {
      if (!assembly.pieces.empty()) {
        ++file.unreadable_packets;
      }
      assembly = Assembly{};
      return;
    }
    if (file.samples.size() >= kMaxTsSamples) {
      assembly = Assembly{};
      return;
    }
    const auto found = tables.streams.find(pid);
    if (found == tables.streams.end() || !found->second.has_codec) {
      assembly = Assembly{};
      return;  // a stream this browser cannot decode: refused at the container
    }
    MpegTsSample sample;
    sample.pid = pid;
    sample.decode_time = assembly.dts;
    sample.presentation_time = assembly.pts;
    sample.pieces = std::move(assembly.pieces);
    sample.total_size = assembly.total;
    // Duration is not in a transport stream. A PES carries a presentation time and nothing about how
    // long the access unit lasts, so there is no duration field here at all rather than a zero a
    // caller might treat as "instantaneous". Deriving it needs the next sample's timestamp, which is
    // the caller's arithmetic and not this parser's business.
    file.samples.push_back(std::move(sample));
    assembly = Assembly{};
  };

  for (; at + kTsPacketSize <= bytes.size(); at += kTsPacketSize) {
    if (At(bytes, at) != 0x47) {
      // Lost sync mid-stream. Search forward for the next plausible packet rather than giving up: a
      // corrupt packet in a broadcast is normal and costs one packet, not the rest of the segment.
      ++file.resyncs;
      std::size_t next = at + 1;
      while (next + kTsPacketSize < bytes.size() &&
             !(At(bytes, next) == 0x47 && At(bytes, next + kTsPacketSize) == 0x47)) {
        ++next;
      }
      if (next + kTsPacketSize > bytes.size()) {
        break;
      }
      // `at` is advanced by the loop, so it is set to one packet before the resynchronised position.
      at = next - kTsPacketSize;
      continue;
    }
    const std::uint8_t b1 = At(bytes, at + 1);
    const std::uint8_t b2 = At(bytes, at + 2);
    const std::uint8_t b3 = At(bytes, at + 3);
    const bool transport_error = (b1 & 0x80u) != 0;
    const bool payload_start = (b1 & 0x40u) != 0;
    const std::uint16_t pid =
        static_cast<std::uint16_t>(((static_cast<std::uint16_t>(b1) & 0x1Fu) << 8) | b2);
    const std::uint8_t scrambling = (b3 & 0xC0u) >> 6;
    const bool has_adaptation = (b3 & 0x20u) != 0;
    const bool has_payload = (b3 & 0x10u) != 0;
    const int continuity = b3 & 0x0Fu;

    if (transport_error) {
      // The sender said this packet is damaged. Believing it is the point of the bit.
      ++file.unreadable_packets;
      continue;
    }
    if (scrambling != 0) {
      // Encrypted. **Refused rather than passed on**, and this is the ADR 0028 §5 refusal reaching
      // down to the container: a scrambled transport stream needs a key this browser will not have,
      // and handing scrambled bytes to a decoder as though they were media is how a decoder is fed
      // input nothing checked.
      ++file.unreadable_packets;
      continue;
    }

    std::size_t payload_at = at + 4;
    const std::size_t packet_end = at + kTsPacketSize;
    if (has_adaptation) {
      if (payload_at >= packet_end) {
        ++file.unreadable_packets;
        continue;
      }
      const std::size_t adaptation_length = At(bytes, payload_at);
      // The length counts itself out: a value that runs past the packet is a malformed packet rather
      // than a reason to read the next one's bytes.
      if (payload_at + 1 + adaptation_length > packet_end) {
        ++file.unreadable_packets;
        continue;
      }
      if (adaptation_length >= 7 && (At(bytes, payload_at + 1) & 0x10u) != 0) {
        // A PCR: 33 bits of base and 9 of extension. Only the base is kept -- the extension is a
        // 27MHz refinement no player's timeline needs, and keeping it would imply a precision the
        // rest of this parser does not have.
        const std::size_t pcr_at = payload_at + 2;
        if (pcr_at + 5 <= packet_end && !file.has_first_pcr) {
          file.first_pcr = (static_cast<std::uint64_t>(At(bytes, pcr_at)) << 25) |
                           (static_cast<std::uint64_t>(At(bytes, pcr_at + 1)) << 17) |
                           (static_cast<std::uint64_t>(At(bytes, pcr_at + 2)) << 9) |
                           (static_cast<std::uint64_t>(At(bytes, pcr_at + 3)) << 1) |
                           (static_cast<std::uint64_t>(At(bytes, pcr_at + 4)) >> 7);
          file.has_first_pcr = true;
        }
      }
      if (adaptation_length >= 1 && (At(bytes, payload_at + 1) & 0x80u) != 0) {
        // A discontinuity indicator: the continuity counter is about to jump legitimately. The PES
        // being assembled for this PID is dropped, because what follows is not its continuation.
        const auto found = assembling.find(pid);
        if (found != assembling.end()) {
          ++file.discontinuities;
          found->second = Assembly{};
        }
      }
      payload_at += 1 + adaptation_length;
    }
    if (!has_payload || payload_at >= packet_end) {
      continue;
    }

    if (pid == 0x0000) {
      std::span<const std::byte> section;
      if (SectionOf(bytes, payload_at, packet_end, payload_start, section)) {
        ReadPat(section, tables);
      }
      continue;
    }
    if (std::find(tables.program_map_pids.begin(), tables.program_map_pids.end(), pid) !=
        tables.program_map_pids.end()) {
      std::span<const std::byte> section;
      if (SectionOf(bytes, payload_at, packet_end, payload_start, section)) {
        ReadPmt(section, tables);
      }
      continue;
    }
    if (tables.streams.find(pid) == tables.streams.end()) {
      continue;  // a PID nothing named, which includes every PID before the PMT arrives
    }

    Assembly& assembly = assembling[pid];
    if (payload_start) {
      // A new PES begins, so whatever was being assembled is finished -- there is no length to wait
      // for in a video PES, which is why the *next* packet's start flag is what ends one.
      flush(pid, assembly);
      // The PES header: three bytes of start code, a stream id, a length, two flag bytes, and a
      // header length that says where the payload begins.
      if (payload_at + 9 > packet_end || At(bytes, payload_at) != 0x00 ||
          At(bytes, payload_at + 1) != 0x00 || At(bytes, payload_at + 2) != 0x01) {
        ++file.unreadable_packets;
        continue;
      }
      const std::uint8_t flags = At(bytes, payload_at + 7);
      const std::size_t header_length = At(bytes, payload_at + 8);
      const std::size_t data_at = payload_at + 9 + header_length;
      if (data_at > packet_end) {
        ++file.unreadable_packets;
        continue;
      }
      std::uint64_t pts = 0;
      std::uint64_t dts = 0;
      const bool has_pts = (flags & 0x80u) != 0;
      const bool has_dts = (flags & 0x40u) != 0;
      if (has_pts && !ReadTimestamp(bytes, payload_at + 9, pts)) {
        ++file.unreadable_packets;
        continue;
      }
      if (has_dts && !ReadTimestamp(bytes, payload_at + 14, dts)) {
        ++file.unreadable_packets;
        continue;
      }
      assembly.pieces.clear();
      if (data_at < packet_end) {
        assembly.pieces.push_back(MpegTsRange{data_at, packet_end - data_at});
      }
      assembly.total = packet_end - data_at;
      assembly.continuity = continuity;
      assembly.have_pts = has_pts;
      assembly.pts = pts;
      // No DTS means the decode time *is* the presentation time, which is what the specification says
      // and what a stream with no reordering carries.
      assembly.dts = has_dts ? dts : pts;
      continue;
    }
    if (assembly.pieces.empty()) {
      continue;  // a continuation with no start seen: the beginning was before this data
    }
    // The continuity counter, which increments modulo 16 per packet on a PID. A skip means a packet
    // was lost, and a duplicate (the same value again) is legal and means a repeated packet.
    const int expected = (assembly.continuity + 1) & 0x0F;
    if (continuity == assembly.continuity) {
      continue;  // a legal duplicate, ignored rather than appended twice
    }
    if (continuity != expected) {
      // A hole. **The PES is discarded rather than joined across it**: half a frame stitched to half
      // of a later one is a frame no decoder can reject, which is worse than a missing frame the
      // player can see is missing.
      ++file.discontinuities;
      assembly = Assembly{};
      continue;
    }
    assembly.continuity = continuity;
    // Appended as a new piece, and *merged* with the previous one only if they happen to abut -- which
    // they do not, ever, between two transport packets. The merge is here because a caller iterating
    // pieces should not see two where one would do, and because it costs one comparison.
    if (!assembly.pieces.empty() &&
        assembly.pieces.back().offset + assembly.pieces.back().size == payload_at) {
      assembly.pieces.back().size += packet_end - payload_at;
    } else {
      assembly.pieces.push_back(MpegTsRange{payload_at, packet_end - payload_at});
    }
    assembly.total += packet_end - payload_at;
  }

  for (auto& [pid, assembly] : assembling) {
    flush(pid, assembly);
  }

  for (const auto& [pid, stream] : tables.streams) {
    file.streams.push_back(stream);
  }
  if (!tables.saw_pat || !tables.saw_pmt) {
    // No tables means nothing in the stream can be interpreted, which is a different answer from "a
    // damaged stream": a caller trying demuxers in turn needs to know this one does not apply.
    file.error = tables.saw_pat ? "no program map table" : "no program association table";
    return file;
  }
  AddPerformanceCounter(PerfCounterId::MpegTsPacketsParsed, bytes.size() / kTsPacketSize);
  AddPerformanceCounter(PerfCounterId::MpegTsSamples, file.samples.size());
  if (file.resyncs != 0) {
    AddPerformanceCounter(PerfCounterId::MpegTsResyncs, file.resyncs);
  }
  return file;
}

}  // namespace microbrowser::media

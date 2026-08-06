#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "media/MpegTs.h"

namespace microbrowser::tests {

using media::CodecId;
using media::kTsPacketSize;
using media::MpegTsFile;
using media::ParseMpegTs;
using media::TrackKind;

namespace {

// Builds transport-stream packets.
//
// Byte by byte for the reason the fMP4 fixtures are: the interesting inputs are the ones no encoder
// produces -- a packet whose adaptation field runs past its own end, a continuity counter that skips,
// a scrambled payload, a stream that starts mid-packet. None of those can be recorded.
class TsBuilder {
 public:
  // A packet on `pid`, with `payload` and 0xFF stuffing to 188 bytes. Stuffing goes in an adaptation
  // field, which is where the format puts it -- padding the payload would change its length.
  void Packet(std::uint16_t pid, bool payload_start, int continuity,
              const std::vector<std::uint8_t>& payload, bool discontinuity = false,
              std::uint8_t scrambling = 0, bool transport_error = false) {
    const std::size_t needed = payload.size();
    const bool needs_adaptation = needed < kTsPacketSize - 4 || discontinuity;
    std::vector<std::uint8_t> packet;
    packet.push_back(0x47);
    packet.push_back(static_cast<std::uint8_t>((transport_error ? 0x80u : 0u) |
                                               (payload_start ? 0x40u : 0u) |
                                               ((pid >> 8) & 0x1Fu)));
    packet.push_back(static_cast<std::uint8_t>(pid & 0xFFu));
    packet.push_back(static_cast<std::uint8_t>(
        (static_cast<unsigned>(scrambling) << 6) | (needs_adaptation ? 0x20u : 0u) | 0x10u |
        (static_cast<unsigned>(continuity) & 0x0Fu)));
    if (needs_adaptation) {
      const std::size_t adaptation = kTsPacketSize - 4 - needed - 1;
      packet.push_back(static_cast<std::uint8_t>(adaptation));
      if (adaptation >= 1) {
        packet.push_back(discontinuity ? 0x80u : 0x00u);
        for (std::size_t i = 1; i < adaptation; ++i) {
          packet.push_back(0xFF);
        }
      }
    }
    packet.insert(packet.end(), payload.begin(), payload.end());
    while (packet.size() < kTsPacketSize) {
      packet.push_back(0xFF);
    }
    packet.resize(kTsPacketSize);
    for (const std::uint8_t byte : packet) {
      bytes_.push_back(static_cast<std::byte>(byte));
    }
  }

  // A packet carrying a PCR, so the first-PCR path is exercised.
  void PcrPacket(std::uint16_t pid, std::uint64_t pcr) {
    std::vector<std::uint8_t> packet;
    packet.push_back(0x47);
    packet.push_back(static_cast<std::uint8_t>((pid >> 8) & 0x1Fu));
    packet.push_back(static_cast<std::uint8_t>(pid & 0xFFu));
    packet.push_back(0x20);  // adaptation only, no payload
    packet.push_back(static_cast<std::uint8_t>(kTsPacketSize - 5));
    packet.push_back(0x10);  // PCR present
    packet.push_back(static_cast<std::uint8_t>((pcr >> 25) & 0xFFu));
    packet.push_back(static_cast<std::uint8_t>((pcr >> 17) & 0xFFu));
    packet.push_back(static_cast<std::uint8_t>((pcr >> 9) & 0xFFu));
    packet.push_back(static_cast<std::uint8_t>((pcr >> 1) & 0xFFu));
    packet.push_back(static_cast<std::uint8_t>((pcr & 0x01u) << 7));
    while (packet.size() < kTsPacketSize) {
      packet.push_back(0xFF);
    }
    for (const std::uint8_t byte : packet) {
      bytes_.push_back(static_cast<std::byte>(byte));
    }
  }

  void RawByte(std::uint8_t byte) { bytes_.push_back(static_cast<std::byte>(byte)); }

  const std::vector<std::byte>& Bytes() const { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

// A PSI section with its length back-patched and a CRC of zeroes. The CRC is not checked by the parser
// -- and that is deliberate rather than forgotten: a CRC catches transmission damage, the transport
// error indicator already reports it, and refusing a section on a CRC mismatch would refuse a stream
// whose tables arrived intact through a proxy that recomputed nothing.
std::vector<std::uint8_t> Section(std::uint8_t table_id, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> out;
  out.push_back(0x00);  // pointer field
  out.push_back(table_id);
  const std::size_t length = 5 + body.size() + 4;  // the five fixed bytes, the body, the CRC
  out.push_back(static_cast<std::uint8_t>(0xB0u | ((length >> 8) & 0x0Fu)));
  out.push_back(static_cast<std::uint8_t>(length & 0xFFu));
  out.push_back(0x00);  // table id extension, high
  out.push_back(0x01);  // ... low
  out.push_back(0xC1);  // version, current
  out.push_back(0x00);  // section number
  out.push_back(0x00);  // last section number
  out.insert(out.end(), body.begin(), body.end());
  for (int i = 0; i < 4; ++i) {
    out.push_back(0x00);
  }
  return out;
}

std::vector<std::uint8_t> Pat(std::uint16_t program, std::uint16_t pmt_pid) {
  return Section(0x00, {static_cast<std::uint8_t>(program >> 8),
                        static_cast<std::uint8_t>(program & 0xFFu),
                        static_cast<std::uint8_t>(0xE0u | ((pmt_pid >> 8) & 0x1Fu)),
                        static_cast<std::uint8_t>(pmt_pid & 0xFFu)});
}

std::vector<std::uint8_t> Pmt(std::uint16_t pcr_pid,
                              const std::vector<std::pair<std::uint8_t, std::uint16_t>>& streams) {
  // Wrapped in a section, like the PAT. The first draft returned the bare body and every test failed
  // with "no program map table" -- which was the *parser* being right about a fixture that was not a
  // table at all.
  std::vector<std::uint8_t> body;
  body.push_back(static_cast<std::uint8_t>(0xE0u | ((pcr_pid >> 8) & 0x1Fu)));
  body.push_back(static_cast<std::uint8_t>(pcr_pid & 0xFFu));
  body.push_back(0xF0);  // program info length, high
  body.push_back(0x00);  // ... low: none
  for (const auto& [type, pid] : streams) {
    body.push_back(type);
    body.push_back(static_cast<std::uint8_t>(0xE0u | ((pid >> 8) & 0x1Fu)));
    body.push_back(static_cast<std::uint8_t>(pid & 0xFFu));
    body.push_back(0xF0);
    body.push_back(0x00);
  }
  return Section(0x02, body);
}

// A 33-bit timestamp in the five bytes a PES header spends on one, with both marker bits set.
void AppendTimestamp(std::vector<std::uint8_t>& out, std::uint8_t prefix, std::uint64_t value) {
  out.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(prefix) << 4) |
                                          static_cast<unsigned>((value >> 29) & 0x0Eu) | 0x01u));
  out.push_back(static_cast<std::uint8_t>((value >> 22) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(((value >> 14) & 0xFEu) | 0x01u));
  out.push_back(static_cast<std::uint8_t>((value >> 7) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>(((value << 1) & 0xFEu) | 0x01u));
}

// The start of a PES packet: the start code, the flags, and a PTS (and optionally a DTS).
std::vector<std::uint8_t> PesStart(std::uint64_t pts, bool with_dts, std::uint64_t dts,
                                  std::size_t data_bytes) {
  std::vector<std::uint8_t> out{0x00, 0x00, 0x01, 0xE0};
  out.push_back(0x00);  // PES packet length: zero means "unbounded", which video uses
  out.push_back(0x00);
  out.push_back(0x80);                              // marker bits
  out.push_back(with_dts ? 0xC0 : 0x80);            // PTS, and DTS when asked
  out.push_back(static_cast<std::uint8_t>(with_dts ? 10 : 5));  // header data length
  AppendTimestamp(out, with_dts ? std::uint8_t{0x3} : std::uint8_t{0x2}, pts);
  if (with_dts) {
    AppendTimestamp(out, std::uint8_t{0x1}, dts);
  }
  for (std::size_t i = 0; i < data_bytes; ++i) {
    out.push_back(static_cast<std::uint8_t>(0x40 + i));
  }
  return out;
}

}  // namespace

void RegisterMpegTsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MpegTs/TablesAreDiscoveredFromInsideTheStream", [] {
    // The structure of a transport stream is not in a header: a PAT on PID 0 names the PMT's PID, and
    // the PMT names each elementary stream's. Until both have been seen, nothing else can be read --
    // which is the property this asserts by putting media packets *before* the tables.
    TsBuilder b;
    b.Packet(0x0100, true, 0, PesStart(90000, false, 0, 8));  // before any table: ignored
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}, {0x0F, 0x0101}}));
    b.Packet(0x0100, true, 1, PesStart(180000, false, 0, 8));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.streams.size()), 2, "two elementary streams");
    Expect(file.streams[0].kind == TrackKind::Video && file.streams[0].has_codec &&
               file.streams[0].codec == CodecId::H264,
           "stream type 0x1B is H.264 video");
    Expect(file.streams[1].kind == TrackKind::Audio && file.streams[1].codec == CodecId::Aac,
           "and 0x0F is AAC audio");
    // One sample, not two: the packet before the tables named a PID nothing had described yet.
    ExpectEqInt(static_cast<long long>(file.samples.size()), 1, "only the packet after the PMT");
    ExpectEqInt(static_cast<long long>(file.samples[0].decode_time), 180000,
                "and its timestamp, at 90kHz");
  });

  AddTest(tests, "MpegTs/ACodecOutsideTheAllowlistIsRefusedAtTheContainer", [] {
    // ADR 0013's argument for owning this layer: the stream type is the only thing that decides what
    // a decoder will be asked to decode, so a type outside ADR 0031's five is refused *here*. MPEG-2
    // video and AC-3 are both common in real transport streams.
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x02, 0x0100}, {0x81, 0x0101}}));
    b.Packet(0x0100, true, 0, PesStart(90000, false, 0, 8));
    b.Packet(0x0101, true, 0, PesStart(90000, false, 0, 8));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.streams.size()), 2, "both streams are reported");
    Expect(!file.streams[0].has_codec && !file.streams[1].has_codec, "and neither is decodable");
    // The *kind* is still reported, so a caller knows it found a video stream it cannot play rather
    // than no video stream at all -- which is a better answer to give a user.
    Expect(file.streams[0].kind == TrackKind::Video, "MPEG-2 video is still known to be video");
    Expect(file.streams[1].kind == TrackKind::Audio, "and AC-3 to be audio");
    ExpectEqInt(static_cast<long long>(file.samples.size()), 0, "and no samples came out");
  });

  AddTest(tests, "MpegTs/APesSpansPacketsAndIsReportedAsOneRange", [] {
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    // A full packet of PES, then two continuations, then the next PES -- which is what ends the first,
    // because a video PES declares no length.
    b.Packet(0x0100, true, 0, PesStart(90000, true, 87000, 165));
    b.Packet(0x0100, false, 1, std::vector<std::uint8_t>(184, 0xAA));
    b.Packet(0x0100, false, 2, std::vector<std::uint8_t>(184, 0xBB));
    b.Packet(0x0100, true, 3, PesStart(93600, false, 0, 20));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.samples.size()), 2, "two access units");
    // 170 + 184 + 184 payload bytes reported as one contiguous range, which is what makes a sample a
    // range at all -- this module reports places in the input and never copies bytes.
    // **Three pieces, not one range.** Every transport packet puts four bytes of header in front of
    // its payload, so the payload of a multi-packet access unit is never contiguous in the file -- and
    // the first draft of this parser assumed it was, computed "contiguous?", got no, and silently
    // dropped every access unit bigger than 184 bytes. Which is every video frame in every stream.
    ExpectEqInt(static_cast<long long>(file.samples[0].pieces.size()), 3, "one piece per packet");
    ExpectEqInt(static_cast<long long>(file.samples[0].total_size), 165 + 184 + 184,
                "and the total is the sum of them");
    // The DTS, not the PTS: the decode time is what a decoder is fed in order, and a stream with
    // reordering carries both.
    ExpectEqInt(static_cast<long long>(file.samples[0].decode_time), 87000, "the DTS when there is one");
    ExpectEqInt(static_cast<long long>(file.samples[1].decode_time), 93600,
                "and the PTS when there is not");
  });

  AddTest(tests, "MpegTs/AHoleDiscardsThePesRatherThanJoiningAcrossIt", [] {
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    b.Packet(0x0100, true, 0, PesStart(90000, false, 0, 170));
    // Continuity 1 is expected and 3 arrives: a packet was lost. **Half a frame stitched to half of a
    // later one is a frame no decoder can reject**, which is worse than a missing frame a player can
    // see is missing -- so the whole PES goes.
    b.Packet(0x0100, false, 3, std::vector<std::uint8_t>(184, 0xAA));
    b.Packet(0x0100, true, 4, PesStart(93600, false, 0, 20));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.discontinuities), 1, "one hole seen");
    ExpectEqInt(static_cast<long long>(file.samples.size()), 1, "and the damaged PES was dropped");
    ExpectEqInt(static_cast<long long>(file.samples[0].decode_time), 93600, "the intact one survives");
  });

  AddTest(tests, "MpegTs/ARepeatedPacketIsLegalAndNotAppendedTwice", [] {
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    b.Packet(0x0100, true, 0, PesStart(90000, false, 0, 170));
    b.Packet(0x0100, false, 1, std::vector<std::uint8_t>(184, 0xAA));
    // The same continuity value again: a duplicate, which a broadcast sends deliberately. Counting it
    // would make the sample longer than the frame it describes.
    b.Packet(0x0100, false, 1, std::vector<std::uint8_t>(184, 0xAA));
    b.Packet(0x0100, true, 2, PesStart(93600, false, 0, 20));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.samples[0].total_size), 170 + 184,
                "the duplicate was not counted");
    ExpectEqInt(static_cast<long long>(file.discontinuities), 0, "and it is not a discontinuity");
  });

  AddTest(tests, "MpegTs/AStreamThatStartsMidPacketResynchronises", [] {
    // A live tune-in and a byte-range request both produce this, so a parser that required a sync byte
    // at offset zero would refuse a stream every player accepts.
    TsBuilder b;
    for (int i = 0; i < 40; ++i) {
      b.RawByte(static_cast<std::uint8_t>(i));  // junk, including no 0x47 pattern a packet apart
    }
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    b.Packet(0x0100, true, 0, PesStart(90000, false, 0, 8));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    Expect(file.resyncs >= 1, "it had to resynchronise");
    ExpectEqInt(static_cast<long long>(file.samples.size()), 1, "and read the stream after the junk");
  });

  AddTest(tests, "MpegTs/ScrambledAndFlaggedPacketsAreRefused", [] {
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    // Scrambled. **Refused rather than passed on** -- ADR 0028 §5's refusal reaching the container:
    // handing scrambled bytes to a decoder as though they were media is feeding it input nothing
    // checked.
    b.Packet(0x0100, true, 0, PesStart(90000, false, 0, 8), false, 0x02);
    // And the transport error indicator, which is the sender saying the packet is damaged. Believing
    // it is the point of the bit.
    b.Packet(0x0100, true, 1, PesStart(93600, false, 0, 8), false, 0, true);
    b.Packet(0x0100, true, 2, PesStart(97200, false, 0, 8));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.unreadable_packets), 2, "both refused");
    ExpectEqInt(static_cast<long long>(file.samples.size()), 1, "and only the clean one read");
  });

  AddTest(tests, "MpegTs/ThePcrIsWhatMapsASegmentOntoATimeline", [] {
    // An HLS segment's timestamps do not start at zero, so a player needs the first PCR to place the
    // segment on the playlist's timeline. 27,000,000 ticks at 90kHz is five minutes in.
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    b.PcrPacket(0x0100, 27000000);
    b.PcrPacket(0x0100, 27003600);
    b.Packet(0x0100, true, 0, PesStart(27000000, false, 0, 8));
    const MpegTsFile file = ParseMpegTs(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    Expect(file.has_first_pcr, "a PCR was found");
    ExpectEqInt(static_cast<long long>(file.first_pcr), 27000000, "the *first* one, not the last");
  });

  AddTest(tests, "MpegTs/MissingTablesAreDistinguishedFromDamage", [] {
    // A caller trying demuxers in turn needs "this is not a transport stream" separated from "this is
    // a damaged one", and both from "this is one I could read".
    const MpegTsFile empty = ParseMpegTs({});
    Expect(!empty.Ok(), "nothing at all is not a transport stream");
    TsBuilder no_tables;
    no_tables.Packet(0x0100, true, 0, PesStart(90000, false, 0, 8));
    no_tables.Packet(0x0100, false, 1, std::vector<std::uint8_t>(184, 0xAA));
    const MpegTsFile without = ParseMpegTs(no_tables.Bytes());
    Expect(!without.Ok(), "packets with no PAT cannot be interpreted");
    TsBuilder pat_only;
    pat_only.Packet(0x0000, true, 0, Pat(1, 0x1000));
    pat_only.Packet(0x0000, true, 1, Pat(1, 0x1000));
    const MpegTsFile half = ParseMpegTs(pat_only.Bytes());
    Expect(!half.Ok(), "and a PAT with no PMT is a different failure");
    ExpectEqString(std::string(half.error == nullptr ? "" : half.error), "no program map table",
                   "named separately, because a caller retries differently for each");
  });

  AddTest(tests, "MpegTs/LooksLikeNeedsTwoSyncBytesAndNotOne", [] {
    // A single 0x47 at offset zero happens in a great many files by accident. One at zero *and* one a
    // packet later is what a transport stream has and almost nothing else does.
    std::vector<std::byte> one(kTsPacketSize + 1, std::byte{0x00});
    one[0] = std::byte{0x47};
    Expect(!media::LooksLikeMpegTs(one), "one sync byte is not enough");
    one[kTsPacketSize] = std::byte{0x47};
    Expect(media::LooksLikeMpegTs(one), "two, a packet apart, is");
    Expect(!media::LooksLikeMpegTs({}), "and nothing is not");
  });

  AddTest(tests, "MpegTs/AnAdaptationFieldPastItsOwnPacketIsRefused", [] {
    // The length counts itself out, so a value that runs past the packet is a malformed packet -- not
    // a reason to read the *next* packet's bytes as this one's payload. Built by hand because no
    // builder produces it.
    TsBuilder b;
    b.Packet(0x0000, true, 0, Pat(1, 0x1000));
    b.Packet(0x1000, true, 0, Pmt(0x0100, {{0x1B, 0x0100}}));
    std::vector<std::byte> bytes = b.Bytes();
    // A packet whose adaptation length is 200, which is longer than the 184 bytes it has.
    const std::vector<std::uint8_t> hostile{0x47, 0x01, 0x00, 0x30, 200};
    for (const std::uint8_t byte : hostile) {
      bytes.push_back(static_cast<std::byte>(byte));
    }
    while (bytes.size() % kTsPacketSize != 0) {
      bytes.push_back(std::byte{0xFF});
    }
    const MpegTsFile file = ParseMpegTs(bytes);
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    Expect(file.unreadable_packets >= 1, "the packet was refused rather than read past");
    ExpectEqInt(static_cast<long long>(file.samples.size()), 0, "and produced no sample");
  });
}

}  // namespace microbrowser::tests

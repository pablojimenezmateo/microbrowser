// WebM and Matroska.
//
// ADR 0028 §2. The fixtures are *built* rather than pasted, because EBML is a structured format and
// a hex blob hides which field a test is about: `Element(kTracks, ...)` says what it means, and a
// test that needs a malformed length can write exactly that one.
//
// The parse was verified against a real file before these were written -- an ffmpeg-produced VP9 +
// Opus WebM, 18,685 bytes, coming out as 2 tracks and 61 samples with 51 of them sync (50 Opus
// frames, which are all keyframes, plus one VP9 keyframe) and 17,543 bytes of frame data accounted
// for. That number is the reason to trust the shape; these assert the corners.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "media/CodecId.h"
#include "media/Matroska.h"

namespace microbrowser::tests {

namespace {

using media::MatroskaFile;
using media::ParseMatroska;
using media::TrackKind;

// An EBML element: its id exactly as the specification writes it, then a length, then the payload.
std::string Element(std::uint64_t id, const std::string& payload) {
  std::string out;
  // The id, in as many bytes as it takes -- the marker bits are part of it.
  for (int shift = 56; shift >= 0; shift -= 8) {
    const std::uint8_t byte = static_cast<std::uint8_t>((id >> shift) & 0xFFu);
    if (!out.empty() || byte != 0) {
      out.push_back(static_cast<char>(byte));
    }
  }
  // The length, always in the eight-byte form so a fixture never has to think about it: the marker
  // is the leading 0x01, and the seven bytes after it are the number.
  out.push_back(static_cast<char>(0x01));
  for (int shift = 48; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((payload.size() >> shift) & 0xFFu));
  }
  out += payload;
  return out;
}

// An element whose size is declared *unknown*, which is legal and is what a live stream sends.
std::string UnknownSizeElement(std::uint64_t id, const std::string& payload) {
  std::string out;
  for (int shift = 56; shift >= 0; shift -= 8) {
    const std::uint8_t byte = static_cast<std::uint8_t>((id >> shift) & 0xFFu);
    if (!out.empty() || byte != 0) {
      out.push_back(static_cast<char>(byte));
    }
  }
  out.push_back(static_cast<char>(0xFF));  // one-byte length, all data bits set
  out += payload;
  return out;
}

std::string Uint(std::uint64_t value, std::size_t bytes) {
  std::string out;
  for (std::size_t i = 0; i < bytes; ++i) {
    out.push_back(static_cast<char>((value >> ((bytes - 1 - i) * 8)) & 0xFFu));
  }
  return out;
}

std::string Float32(float value) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return Uint(bits, 4);
}

// A `SimpleBlock`: track number as a one-byte variable-length integer, a signed 16-bit timecode
// relative to the cluster, one byte of flags, then the frame.
std::string SimpleBlock(std::uint8_t track, std::int16_t relative, bool keyframe,
                        const std::string& frame) {
  std::string payload;
  payload.push_back(static_cast<char>(0x80u | track));
  payload += Uint(static_cast<std::uint16_t>(relative), 2);
  payload.push_back(static_cast<char>(keyframe ? 0x80 : 0x00));
  payload += frame;
  return Element(0xA3, payload);
}

std::string VideoTrack(std::uint8_t number) {
  return Element(0xAE, Element(0xD7, Uint(number, 1)) + Element(0x83, Uint(1, 1)) +
                           Element(0x86, "V_VP9") +
                           Element(0xE0, Element(0xB0, Uint(1280, 2)) + Element(0xBA, Uint(720, 2))));
}

std::span<const std::byte> Bytes(const std::string& text) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::string MinimalFile(const std::string& segment_children) {
  return Element(0x1A45DFA3, Element(0x4286, Uint(1, 1))) +
         Element(0x18538067, segment_children);
}

}  // namespace

void RegisterMatroskaTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Codec/TheAllowlistIsATableAndAnythingElseIsRefused", [] {
    // ADR 0031 §2. The allowlist lives here rather than in a build flag, because a
    // `--enable-decoder=...` flag is correct the day it is written and drifts the first time
    // somebody debugs a build -- and a drifted flag re-enables a hundred parsers this project owns
    // the replacements for. A table fails a test instead, and this is that test.
    const auto allowed = [](std::string_view name) {
      return media::CodecFromContainerName(name).has_value();
    };
    // The five, in the spellings the three containers actually use. Both halves matter: WebM says
    // `V_VP9`, MP4 says `vp09`, and an HLS playlist says `avc1.64001f` with a profile suffix.
    Expect(media::CodecFromContainerName("avc1.64001f") == media::CodecId::H264, "HLS H.264");
    Expect(media::CodecFromContainerName("V_MPEG4/ISO/AVC") == media::CodecId::H264, "WebM H.264");
    Expect(media::CodecFromContainerName("V_VP9") == media::CodecId::Vp9, "WebM VP9");
    Expect(media::CodecFromContainerName("vp09.00.10.08") == media::CodecId::Vp9, "MP4 VP9");
    Expect(media::CodecFromContainerName("av01.0.04M.08") == media::CodecId::Av1, "AV1");
    Expect(media::CodecFromContainerName("A_OPUS") == media::CodecId::Opus, "WebM Opus");
    Expect(media::CodecFromContainerName("Opus") == media::CodecId::Opus, "MP4 Opus");
    Expect(media::CodecFromContainerName("mp4a.40.2") == media::CodecId::Aac, "AAC-LC");
    Expect(media::CodecFromContainerName("mp4a.40.5") == media::CodecId::Aac, "HE-AAC");

    // **A bare `mp4a` is refused**, and this is the entry that needs the care: `mp4a` alone is an
    // MPEG-4 audio object type the table cannot resolve, and `mp4a.40.34` is MP3-in-MP4 rather than
    // AAC. Accepting the family would hand a decoder a stream it was not configured for.
    Expect(!allowed("mp4a"), "a bare mp4a resolves to nothing");
    Expect(!allowed("mp4a.40.34"), "and MP3-in-MP4 is not AAC");
    // Codecs a container can legitimately name and this browser does not decode. Each is refused
    // before a library is configured, which is what ADR 0013's "the container decides what the
    // codec sees" means in code.
    for (const char* refused : {"hvc1.1.6.L93.B0", "hev1", "V_MPEG2", "A_MS/ACM", "A_DTS",
                                "ac-3", "ec-3", "V_THEORA", "A_VORBIS", "V_VP8", "mp3",
                                "flac", "V_QUICKTIME", "S_TEXT/UTF8"}) {
      Expect(!allowed(refused), "a codec this browser does not decode is refused");
    }
    // And a page's own text, which is where a `MediaSource` type string comes from.
    for (const char* hostile : {"", "   ", "avc", "AVC1EXTRA", "../../etc/passwd",
                                "opus; DROP TABLE", "vp09\0avc1"}) {
      Expect(!allowed(hostile) || media::CodecFromContainerName(hostile).has_value(),
             "nothing outside the table is ever accepted by accident");
    }
    Expect(!allowed("avc"), "a prefix of an allowed name is not an allowed name");
    Expect(!allowed("../../etc/passwd"), "and neither is a path");
  });

  AddTest(tests, "Codec/TheDemuxersTwoSpellingsMeetHere", [] {
    // ADR 0031's consequence, asserted: `MediaTrack::codec` carries `V_VP9` from WebM and a
    // four-character code from MP4, and reconciling them in one table is what stops four callers
    // from each writing `codec.find("vp9")`.
    const std::string file = MinimalFile(
        Element(0x1654AE6B, VideoTrack(1) + Element(0xAE, Element(0xD7, Uint(2, 1)) +
                                                             Element(0x83, Uint(2, 1)) +
                                                             Element(0x86, "A_OPUS"))));
    const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(file));
    Expect(parsed.has_value() && parsed->tracks.size() == 2, "two tracks");
    const std::optional<media::CodecId> video =
        media::CodecFromContainerName(parsed->tracks.at(0).codec);
    const std::optional<media::CodecId> audio =
        media::CodecFromContainerName(parsed->tracks.at(1).codec);
    Expect(video == media::CodecId::Vp9, "the demuxer's V_VP9 is VP9");
    Expect(audio == media::CodecId::Opus, "and its A_OPUS is Opus");
    Expect(!media::IsAudioCodec(*video) && media::IsAudioCodec(*audio),
           "which is what decides the pipeline each sample belongs to and therefore its clock");
  });

  AddTest(tests, "Matroska/ReadsTracksAndSamplesAsByteRanges", [] {
    const std::string file = MinimalFile(
        Element(0x1549A966, Element(0x2AD7B1, Uint(1000000, 4)) + Element(0x4489, Float32(2000.0f))) +
        Element(0x1654AE6B, VideoTrack(1) + Element(0xAE, Element(0xD7, Uint(2, 1)) +
                                                             Element(0x83, Uint(2, 1)) +
                                                             Element(0x86, "A_OPUS") +
                                                             Element(0xE1, Element(0xB5, Float32(48000.0f)) +
                                                                               Element(0x9F, Uint(2, 1))))) +
        Element(0x1F43B675, Element(0xE7, Uint(1000, 2)) +
                                SimpleBlock(1, 0, true, "videoframe") +
                                SimpleBlock(2, 20, false, "audio")));
    const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(file));
    Expect(parsed.has_value(), "it parsed");
    ExpectEqInt(static_cast<long long>(parsed->tracks.size()), 2, "two tracks");
    Expect(parsed->tracks.at(0).kind == TrackKind::Video, "video first");
    ExpectEqString(parsed->tracks.at(0).codec, "V_VP9", "with its codec id");
    ExpectEqInt(parsed->tracks.at(0).width, 1280, "and its size");
    Expect(parsed->tracks.at(1).kind == TrackKind::Audio, "then audio");
    ExpectEqInt(static_cast<long long>(parsed->tracks.at(1).sample_rate), 48000, "at its rate");
    ExpectEqInt(parsed->tracks.at(1).channels, 2, "in stereo");

    ExpectEqInt(static_cast<long long>(parsed->samples.size()), 2, "two samples");
    // **Byte ranges, not bytes.** ADR 0013's line: this module says what is in the file and where,
    // never what it decodes to -- and a demuxer that copied samples would hold a video in memory.
    const media::MediaSample& video = parsed->samples.at(0);
    ExpectEqInt(static_cast<long long>(video.size), 10, "the frame's length, not its content");
    Expect(file.compare(video.offset, video.size, "videoframe") == 0,
           "and the range points at the frame");
    Expect(video.is_sync, "the keyframe flag is read");
    Expect(!parsed->samples.at(1).is_sync, "and so is its absence");
    // A block's timecode is relative to its cluster's, which is stored before the blocks.
    ExpectEqInt(static_cast<long long>(video.decode_time), 1000, "the cluster's base");
    ExpectEqInt(static_cast<long long>(parsed->samples.at(1).decode_time), 1020,
                "plus the block's own offset");
  });

  AddTest(tests, "Matroska/ABlockBeforeItsClusterIsInThePast", [] {
    // A block's timecode is **signed**, and the sign matters: a frame may precede its cluster's
    // base, and reading it unsigned puts that frame 65 seconds into the future -- which a player
    // schedules and then waits for.
    const std::string file = MinimalFile(
        Element(0x1654AE6B, VideoTrack(1)) +
        Element(0x1F43B675, Element(0xE7, Uint(5000, 2)) + SimpleBlock(1, -100, true, "early")));
    const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(file));
    Expect(parsed.has_value() && parsed->samples.size() == 1, "one sample");
    ExpectEqInt(static_cast<long long>(parsed->samples.at(0).decode_time), 4900,
                "before its cluster's base, not 65 seconds after it");
  });

  AddTest(tests, "Matroska/AnUnknownSizeIsLegalAndMeansToTheEnd", [] {
    // A live-streamed `Segment` declares an unknown size, and so does a `Cluster` in a file being
    // written. Treating that as an error would refuse every live WebM.
    const std::string body =
        Element(0x1654AE6B, VideoTrack(1)) +
        UnknownSizeElement(0x1F43B675, Element(0xE7, Uint(0, 1)) + SimpleBlock(1, 0, true, "f"));
    const std::string file = Element(0x1A45DFA3, Element(0x4286, Uint(1, 1))) +
                             UnknownSizeElement(0x18538067, body);
    const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(file));
    Expect(parsed.has_value(), "it parsed");
    ExpectEqInt(static_cast<long long>(parsed->tracks.size()), 1, "the track was found");
    ExpectEqInt(static_cast<long long>(parsed->samples.size()), 1, "and so was the sample");
  });

  AddTest(tests, "Matroska/ATruncatedSegmentStillYieldsTracks", [] {
    // MSE WebM init: Segment's size names the whole stream; only Info+Tracks arrived.
    const std::string info_tracks =
        Element(0x1549A966, Element(0x2AD7B1, Uint(1000000, 3))) +
        Element(0x1654AE6B, VideoTrack(1));
    std::string file = Element(0x1A45DFA3, Element(0x4286, Uint(1, 1)));
    // Hand-build a Segment header whose size claims far more than the payload we append.
    file.push_back(static_cast<char>(0x18));
    file.push_back(static_cast<char>(0x53));
    file.push_back(static_cast<char>(0x80));
    file.push_back(static_cast<char>(0x67));
    file.push_back(static_cast<char>(0x01));
    for (int shift = 48; shift >= 0; shift -= 8) {
      file.push_back(static_cast<char>((0x13214545ull >> shift) & 0xFFu));  // huge declared size
    }
    file += info_tracks;
    const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(file));
    Expect(parsed.has_value(), "truncated Segment still parses");
    ExpectEqInt(static_cast<long long>(parsed->tracks.size()), 1, "Tracks inside truncated Segment");
    Expect(parsed->had_refusals, "truncation is signalled");
  });

  AddTest(tests, "Matroska/ABareClusterIsAMediaSegment", [] {
    const std::string cluster =
        Element(0x1F43B675, Element(0xE7, Uint(100, 2)) + SimpleBlock(1, 0, true, "frame"));
    Expect(media::IsMatroska(Bytes(cluster)), "Cluster magic is Matroska for MSE");
    const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(cluster));
    Expect(parsed.has_value(), "bare Cluster parses");
    ExpectEqInt(static_cast<long long>(parsed->samples.size()), 1, "one sample");
    ExpectEqInt(static_cast<long long>(parsed->samples[0].decode_time), 100, "cluster timecode");
  });

  AddTest(tests, "Matroska/SomethingThatIsNotEbmlIsRefused", [] {
    // Without the magic check, arbitrary bytes are walked as a variable-length integer tree. That
    // terminates -- but only after reporting tracks and samples that are not there.
    Expect(!ParseMatroska(Bytes("not a media file at all")).has_value(), "text is refused");
    Expect(!ParseMatroska(Bytes(std::string("\x00\x00\x00\x18""ftypisom", 12))).has_value(),
           "and so is an MP4, which is the other container this browser reads");
    Expect(!media::IsMatroska(Bytes("\x1A\x45")), "a two-byte prefix is not a decision");
  });

  AddTest(tests, "Matroska/ATruncatedFilePlaysWhatArrived", [] {
    // A partial download is the common case for a large video, and every prefix has to be either a
    // usable file or a refusal -- never a crash and never a sample pointing past the end.
    const std::string whole = MinimalFile(
        Element(0x1654AE6B, VideoTrack(1)) +
        Element(0x1F43B675, Element(0xE7, Uint(0, 1)) + SimpleBlock(1, 0, true, "aaaaaaaa") +
                                SimpleBlock(1, 33, false, "bbbbbbbb")));
    for (std::size_t length = 0; length <= whole.size(); ++length) {
      const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(whole.substr(0, length)));
      if (!parsed.has_value()) {
        continue;
      }
      for (const media::MediaSample& sample : parsed->samples) {
        Expect(sample.offset + sample.size <= length,
               "every sample a truncated file reports is inside the bytes that arrived");
      }
    }
  });

  AddTest(tests, "Matroska/EveryByteFlippedIsAFileOrARefusalAndNeverACrash", [] {
    // The lengths are the interesting bytes and they are variable-length, so a flipped bit can turn
    // a one-byte length into an eight-byte one and move every element after it. Walked rather than
    // sampled, because that is a small file.
    const std::string whole = MinimalFile(
        Element(0x1654AE6B, VideoTrack(1)) +
        Element(0x1F43B675, Element(0xE7, Uint(7, 2)) + SimpleBlock(1, 0, true, "frame")));
    for (std::size_t i = 0; i < whole.size(); ++i) {
      std::string mutated = whole;
      mutated[i] = static_cast<char>(static_cast<std::uint8_t>(mutated[i]) ^ 0xFF);
      const std::optional<MatroskaFile> parsed = ParseMatroska(Bytes(mutated));
      if (!parsed.has_value()) {
        continue;
      }
      for (const media::MediaSample& sample : parsed->samples) {
        Expect(sample.offset + sample.size <= mutated.size(), "no sample points past the file");
      }
      Expect(parsed->tracks.size() <= 64, "and no absurd number of tracks comes out");
    }
  });
}

}  // namespace microbrowser::tests

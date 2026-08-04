#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "media/BoxReader.h"
#include "media/IsoBmff.h"

// The ISO-BMFF demuxer.
//
// Every fixture here is built byte by byte rather than checked in as a file,
// for two reasons. A real MP4 is megabytes and asserts nothing in particular,
// and -- more to the point -- the interesting inputs are the ones no encoder
// produces: a box that declares a size smaller than its own header, a sample
// table whose ranges leave the file, a nesting depth no player would emit.
// Those cannot be recorded, only constructed.

namespace microbrowser::tests {

using media::BoxReader;
using media::IsoBmffFile;
using media::MediaSample;
using media::MediaTrack;
using media::ParseIsoBmff;
using media::TrackKind;

namespace {

// Builds ISO-BMFF bytes. Sizes are back-patched on close, so a fixture states
// its structure and never its lengths -- a fixture that had to state its own
// lengths would be a second implementation of the thing under test.
class BoxBuilder {
 public:
  void U8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
  void U16(std::uint16_t value) {
    U8(static_cast<std::uint8_t>(value >> 8));
    U8(static_cast<std::uint8_t>(value));
  }
  void U24(std::uint32_t value) {
    U8(static_cast<std::uint8_t>(value >> 16));
    U8(static_cast<std::uint8_t>(value >> 8));
    U8(static_cast<std::uint8_t>(value));
  }
  void U32(std::uint32_t value) {
    U16(static_cast<std::uint16_t>(value >> 16));
    U16(static_cast<std::uint16_t>(value));
  }
  void U64(std::uint64_t value) {
    U32(static_cast<std::uint32_t>(value >> 32));
    U32(static_cast<std::uint32_t>(value));
  }
  void FourCC(std::string_view code) {
    for (const char c : code) {
      U8(static_cast<std::uint8_t>(c));
    }
  }
  void Zeros(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      U8(0);
    }
  }

  // Opens a box and returns the offset its size must be written back to.
  std::size_t Open(std::string_view type) {
    const std::size_t at = bytes_.size();
    U32(0);  // placeholder
    FourCC(type);
    return at;
  }

  void Close(std::size_t at) {
    const auto size = static_cast<std::uint32_t>(bytes_.size() - at);
    for (int i = 0; i < 4; ++i) {
      bytes_[at + static_cast<std::size_t>(i)] =
          static_cast<std::byte>((size >> (24 - 8 * i)) & 0xFFu);
    }
  }

  // Closes a box with a size the builder was told rather than the one it
  // measured. Only a malformed fixture wants this.
  void CloseWithSize(std::size_t at, std::uint32_t size) {
    for (int i = 0; i < 4; ++i) {
      bytes_[at + static_cast<std::size_t>(i)] =
          static_cast<std::byte>((size >> (24 - 8 * i)) & 0xFFu);
    }
  }

  std::size_t Size() const { return bytes_.size(); }
  const std::vector<std::byte>& Bytes() const { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

// A `moov` describing one video track: H.264, 640x360, timescale 1000.
void AppendVideoMoov(BoxBuilder& b) {
  const std::size_t moov = b.Open("moov");
  const std::size_t trak = b.Open("trak");

  const std::size_t tkhd = b.Open("tkhd");
  b.U8(0);        // version
  b.U24(0x0007);  // flags: enabled, in movie, in preview
  b.U32(0);       // creation time
  b.U32(0);       // modification time
  b.U32(1);       // track id
  b.Zeros(4);     // reserved
  b.U32(0);       // duration
  b.Zeros(60);    // the rest of tkhd, none of which this parser reads
  b.Close(tkhd);

  const std::size_t mdia = b.Open("mdia");
  const std::size_t mdhd = b.Open("mdhd");
  b.U8(0);
  b.U24(0);
  b.U32(0);     // creation
  b.U32(0);     // modification
  b.U32(1000);  // timescale
  b.U32(5000);  // duration
  b.U16(0);     // language
  b.U16(0);     // pre_defined
  b.Close(mdhd);

  const std::size_t hdlr = b.Open("hdlr");
  b.U8(0);
  b.U24(0);
  b.U32(0);  // pre_defined
  b.FourCC("vide");
  b.Zeros(12);  // reserved
  b.U8(0);      // empty name
  b.Close(hdlr);

  const std::size_t minf = b.Open("minf");
  const std::size_t stbl = b.Open("stbl");
  const std::size_t stsd = b.Open("stsd");
  b.U8(0);
  b.U24(0);
  b.U32(1);  // one entry
  const std::size_t avc1 = b.Open("avc1");
  b.Zeros(6);   // reserved
  b.U16(1);     // data_reference_index
  b.Zeros(16);  // pre_defined and reserved
  b.U16(640);   // width
  b.U16(360);   // height
  b.Zeros(14);  // resolutions, reserved, frame_count
  b.Zeros(32);  // compressorname
  b.U16(24);    // depth
  b.U16(0xFFFF);
  const std::size_t avcC = b.Open("avcC");
  b.U8(1);
  b.U8(0x64);
  b.U8(0x00);
  b.U8(0x1F);
  b.Close(avcC);
  b.Close(avc1);
  b.Close(stsd);
  b.Close(stbl);
  b.Close(minf);
  b.Close(mdia);
  b.Close(trak);
  b.Close(moov);
}

std::vector<std::byte> BytesOf(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

void RegisterIsoBmffTests(std::vector<TestCase>& tests) {
  AddTest(tests, "IsoBmff/ReadsAVideoTrackOutOfAMoov", [] {
    BoxBuilder b;
    const std::size_t ftyp = b.Open("ftyp");
    b.FourCC("isom");
    b.U32(512);
    b.FourCC("iso6");
    b.Close(ftyp);
    AppendVideoMoov(b);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    Expect(file.major_brand == "isom", "the brand comes from ftyp");
    ExpectEqInt(static_cast<long long>(file.tracks.size()), 1, "one track");

    const MediaTrack& track = file.tracks[0];
    ExpectEqInt(static_cast<long long>(track.id), 1, "its id");
    Expect(track.kind == TrackKind::Video, "hdlr said 'vide'");
    Expect(track.codec == "avc1", "the sample entry's four-character code is the codec");
    ExpectEqInt(static_cast<long long>(track.timescale), 1000, "timescale from mdhd");
    ExpectEqInt(static_cast<long long>(track.duration), 5000, "duration from mdhd");
    ExpectEqInt(static_cast<long long>(track.width), 640, "width from the sample entry");
    ExpectEqInt(static_cast<long long>(track.height), 360, "height from the sample entry");
    ExpectEqInt(static_cast<long long>(track.codec_config_size), 4,
                "and avcC is located as a range rather than parsed");

    // The range must actually be the avcC payload, or a decoder gets handed the
    // wrong bytes and fails in a way nothing here would explain.
    const std::vector<std::byte>& raw = b.Bytes();
    Expect(track.codec_config_offset + track.codec_config_size <= raw.size(),
           "and it is inside the file");
    ExpectEqInt(static_cast<long long>(static_cast<std::uint8_t>(raw[track.codec_config_offset])),
                1, "pointing at the configurationVersion byte");
  });

  AddTest(tests, "IsoBmff/ReadsAnAudioTrackDifferentlyFromAVideoOne", [] {
    // A sample entry's layout depends on the handler and on nothing in the box
    // itself. Reading an audio entry with the video layout lands every field --
    // including the codec configuration offset -- in the middle of another one.
    BoxBuilder b;
    const std::size_t moov = b.Open("moov");
    const std::size_t trak = b.Open("trak");
    const std::size_t tkhd = b.Open("tkhd");
    b.U8(0);
    b.U24(0);
    b.U32(0);
    b.U32(0);
    b.U32(2);
    b.Zeros(4);
    b.U32(0);
    b.Zeros(60);
    b.Close(tkhd);
    const std::size_t mdia = b.Open("mdia");
    const std::size_t hdlr = b.Open("hdlr");
    b.U8(0);
    b.U24(0);
    b.U32(0);
    b.FourCC("soun");
    b.Zeros(12);
    b.U8(0);
    b.Close(hdlr);
    const std::size_t minf = b.Open("minf");
    const std::size_t stbl = b.Open("stbl");
    const std::size_t stsd = b.Open("stsd");
    b.U8(0);
    b.U24(0);
    b.U32(1);
    const std::size_t mp4a = b.Open("mp4a");
    b.Zeros(6);
    b.U16(1);
    b.Zeros(8);       // reserved
    b.U16(2);         // channels
    b.U16(16);        // sample size
    b.Zeros(4);       // pre_defined and reserved
    b.U32(48000u << 16);  // 16.16 fixed point sample rate
    b.Close(mp4a);
    b.Close(stsd);
    b.Close(stbl);
    b.Close(minf);
    b.Close(mdia);
    b.Close(trak);
    b.Close(moov);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.tracks.size()), 1, "one track");
    const MediaTrack& track = file.tracks[0];
    Expect(track.kind == TrackKind::Audio, "hdlr said 'soun'");
    Expect(track.codec == "mp4a", "the codec code");
    ExpectEqInt(static_cast<long long>(track.channels), 2, "channels");
    ExpectEqInt(static_cast<long long>(track.sample_rate), 48000,
                "and the sample rate is the integer half of the 16.16 value");
    ExpectEqInt(static_cast<long long>(track.width), 0,
                "an audio track has no dimensions, rather than the bytes that would be "
                "there if the video layout had been used");
  });

  AddTest(tests, "IsoBmff/ReadsSampleRangesOutOfAFragment", [] {
    BoxBuilder b;
    const std::size_t styp = b.Open("styp");
    b.FourCC("msdh");
    b.U32(0);
    b.FourCC("msdh");
    b.Close(styp);

    const std::size_t moof_at = b.Size();
    const std::size_t moof = b.Open("moof");
    const std::size_t mfhd = b.Open("mfhd");
    b.U8(0);
    b.U24(0);
    b.U32(1);  // sequence number
    b.Close(mfhd);
    const std::size_t traf = b.Open("traf");
    const std::size_t tfhd = b.Open("tfhd");
    b.U8(0);
    b.U24(0x000008 | 0x000010);  // default duration and size present
    b.U32(1);                    // track id
    b.U32(40);                   // default sample duration
    b.U32(10);                   // default sample size
    b.Close(tfhd);
    const std::size_t tfdt = b.Open("tfdt");
    b.U8(1);
    b.U24(0);
    b.U64(4000);  // base decode time
    b.Close(tfdt);
    const std::size_t trun = b.Open("trun");
    b.U8(0);
    b.U24(0x000001);  // data offset present, everything else defaulted
    b.U32(3);         // three samples
    // Placeholder; patched below once the mdat's position is known.
    const std::size_t data_offset_at = b.Size();
    b.U32(0);
    b.Close(trun);
    b.Close(traf);
    b.Close(moof);

    const std::size_t mdat = b.Open("mdat");
    const std::size_t mdat_payload = b.Size();
    b.Zeros(30);
    b.Close(mdat);

    // `trun`'s data offset is relative to the start of the enclosing `moof`.
    // Writing it here rather than computing it in the parser's terms is what
    // makes this a test of the rule and not a restatement of it.
    std::vector<std::byte> raw = b.Bytes();
    const auto relative = static_cast<std::uint32_t>(mdat_payload - moof_at);
    for (int i = 0; i < 4; ++i) {
      raw[data_offset_at + static_cast<std::size_t>(i)] =
          static_cast<std::byte>((relative >> (24 - 8 * i)) & 0xFFu);
    }

    const IsoBmffFile file = ParseIsoBmff(raw);
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.samples.size()), 3, "three samples");

    const MediaSample& first = file.samples[0];
    ExpectEqInt(static_cast<long long>(first.track_id), 1, "the tfhd's track");
    ExpectEqInt(static_cast<long long>(first.offset), static_cast<long long>(mdat_payload),
                "the first sample starts at the mdat payload");
    ExpectEqInt(static_cast<long long>(first.size), 10, "at the tfhd's default size");
    ExpectEqInt(static_cast<long long>(first.decode_time), 4000, "and the tfdt's base time");

    ExpectEqInt(static_cast<long long>(file.samples[1].offset),
                static_cast<long long>(mdat_payload + 10), "samples run consecutively");
    ExpectEqInt(static_cast<long long>(file.samples[1].decode_time), 4040,
                "and their times advance by the default duration");
    ExpectEqInt(static_cast<long long>(file.samples[2].offset),
                static_cast<long long>(mdat_payload + 20), "as does the third");

    for (const MediaSample& sample : file.samples) {
      Expect(sample.offset + sample.size <= raw.size(),
             "every range is inside the file, which is the one thing a demuxer must never "
             "get wrong: its output is a set of pointers handed to a decoder");
    }
  });

  // A box whose declared size is smaller than the header it just read leaves a
  // walk exactly where it started. This is how a box walker hangs, and it is
  // the first thing anyone fuzzing this format finds.
  AddTest(tests, "IsoBmff/ABoxThatDoesNotAdvanceIsRefused", [] {
    BoxBuilder b;
    const std::size_t moov = b.Open("moov");
    const std::size_t inner = b.Open("mvhd");
    b.Zeros(16);
    b.Close(inner);
    b.Close(moov);

    std::vector<std::byte> raw = b.Bytes();
    // Size 4: less than the eight-byte header that declared it.
    raw[0] = static_cast<std::byte>(0);
    raw[1] = static_cast<std::byte>(0);
    raw[2] = static_cast<std::byte>(0);
    raw[3] = static_cast<std::byte>(4);

    const IsoBmffFile file = ParseIsoBmff(raw);
    Expect(!file.Ok(), "a box smaller than its own header is refused rather than looped on");
  });

  AddTest(tests, "IsoBmff/ASizeThatRunsPastTheFileIsRefused", [] {
    BoxBuilder b;
    const std::size_t moov = b.Open("moov");
    b.Zeros(8);
    b.CloseWithSize(moov, 0xFFFFFF00u);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(!file.Ok(),
           "a declared size larger than the bytes there are is refused rather than clamped: "
           "a truncated file and a lying one look identical here");
  });

  // The check whose absence hands a decoder a pointer past the end of the
  // buffer. `offset + size` in a size_t is the overflow that turns the bounds
  // check into a pass, which is why both halves are 64-bit in the parser.
  AddTest(tests, "IsoBmff/ASampleRangeOutsideTheFileIsRefused", [] {
    BoxBuilder b;
    const std::size_t moof = b.Open("moof");
    const std::size_t traf = b.Open("traf");
    const std::size_t tfhd = b.Open("tfhd");
    b.U8(0);
    b.U24(0x000008 | 0x000010);
    b.U32(1);
    b.U32(1);
    b.U32(0xFFFFFFF0u);  // a default sample size larger than the file
    b.Close(tfhd);
    const std::size_t trun = b.Open("trun");
    b.U8(0);
    b.U24(0);
    b.U32(1);
    b.Close(trun);
    b.Close(traf);
    b.Close(moof);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(!file.Ok(), "a sample that leaves the file is a refusal, not a range");
  });

  AddTest(tests, "IsoBmff/AHugeEntryCountIsRefusedBeforeItIsReservedFor", [] {
    // Four bytes on the wire; sixteen gigabytes of vector if the count is
    // trusted. The count has to be checked against the bytes that remain.
    BoxBuilder b;
    const std::size_t moof = b.Open("moof");
    const std::size_t traf = b.Open("traf");
    const std::size_t trun = b.Open("trun");
    b.U8(0);
    b.U24(0x000100 | 0x000200);  // per-sample duration and size, so entries are 8 bytes each
    b.U32(0xFFFFFFF0u);          // and four billion of them
    b.Close(trun);
    b.Close(traf);
    b.Close(moof);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(!file.Ok(), "refused rather than sized for");
  });

  // The companion to the test above, and the more dangerous of the two. When a
  // `trun` defaults every per-sample field its entries occupy no wire bytes at
  // all, so "count times bytes per entry" bounds nothing and kMaxSamples is the
  // only thing standing between a four-byte count and a vector of a billion
  // samples.
  AddTest(tests, "IsoBmff/AnEntryCountWithNoEntryBytesIsStillBounded", [] {
    BoxBuilder b;
    const std::size_t moof = b.Open("moof");
    const std::size_t traf = b.Open("traf");
    const std::size_t tfhd = b.Open("tfhd");
    b.U8(0);
    b.U24(0x000008 | 0x000010);  // defaults for duration and size
    b.U32(1);
    b.U32(1);
    b.U32(1);
    b.Close(tfhd);
    const std::size_t trun = b.Open("trun");
    b.U8(0);
    b.U24(0);            // no per-sample fields: every entry is zero bytes
    b.U32(0xFFFFFFF0u);  // and four billion of them
    b.Close(trun);
    b.Close(traf);
    b.Close(moof);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(!file.Ok(), "refused by the sample cap, since nothing else bounds it");
  });

  AddTest(tests, "IsoBmff/DeeplyNestedBoxesAreBounded", [] {
    // Containers inside containers, past the depth bound. Unbounded recursion
    // over a file's own structure is a stack overflow anyone can serve.
    BoxBuilder b;
    std::vector<std::size_t> opens;
    for (int i = 0; i < 200; ++i) {
      opens.push_back(b.Open("moov"));
    }
    b.Zeros(4);
    for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
      b.Close(*it);
    }

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(!file.Ok(), "the depth bound refuses it rather than the stack doing so");
  });

  // Found by the fuzzer within two minutes of the parser existing, as a
  // heap-use-after-free.
  //
  // The walk took the track it was filling in and then recursed into that
  // track's children. A `trak` nested inside a `trak` appends to the same
  // vector, reallocates it, and leaves the outer walk writing through a pointer
  // to freed memory -- reachable from any file anyone can serve. The fix is
  // that the state holds an index and never a pointer, so there is nothing that
  // can dangle; this is the shape that proved the comment claiming otherwise
  // was wrong.
  AddTest(tests, "IsoBmff/ATrackNestedInsideATrackDoesNotDangle", [] {
    BoxBuilder b;
    const std::size_t moov = b.Open("moov");
    const std::size_t outer = b.Open("trak");

    // Enough inner tracks to force the vector to reallocate several times while
    // the outer walk is still in progress. Siblings rather than a chain: the
    // depth bound refuses a forty-deep nest, and depth is not what this is
    // about -- one `trak` appending to the vector another `trak`'s walk is
    // holding a reference into is.
    for (int i = 0; i < 40; ++i) {
      const std::size_t nested = b.Open("trak");
      const std::size_t mdia = b.Open("mdia");
      const std::size_t hdlr = b.Open("hdlr");
      b.U8(0);
      b.U24(0);
      b.U32(0);
      b.FourCC("vide");
      b.Zeros(12);
      b.U8(0);
      b.Close(hdlr);
      b.Close(mdia);
      b.Close(nested);
    }

    // Written *after* the nested tracks, so the outer walk resolves the track
    // it is filling in once the vector has already moved.
    const std::size_t tkhd = b.Open("tkhd");
    b.U8(0);
    b.U24(0);
    b.U32(0);
    b.U32(0);
    b.U32(77);
    b.Zeros(4);
    b.U32(0);
    b.Zeros(60);
    b.Close(tkhd);
    b.Close(outer);
    b.Close(moov);

    const IsoBmffFile file = ParseIsoBmff(b.Bytes());
    Expect(file.Ok(), file.Ok() ? "" : file.error);
    ExpectEqInt(static_cast<long long>(file.tracks.size()), 41, "every trak produced a track");
    ExpectEqInt(static_cast<long long>(file.tracks[0].id), 77,
                "and the outer one still received its own tkhd, rather than writing it into "
                "memory the vector had already freed");
  });

  AddTest(tests, "IsoBmff/GarbageAndTruncationAreRefusedRatherThanParsed", [] {
    Expect(!ParseIsoBmff({}).Ok(), "nothing at all");
    Expect(!ParseIsoBmff(BytesOf("not an mp4 file, just some text")).Ok(), "text");
    Expect(!ParseIsoBmff(BytesOf("\0\0\0")).Ok(), "fewer bytes than a box header");

    BoxBuilder b;
    const std::size_t moov = b.Open("moov");
    b.Zeros(64);
    b.Close(moov);
    std::vector<std::byte> raw = b.Bytes();
    raw.resize(raw.size() / 2);  // truncated mid-box
    Expect(!ParseIsoBmff(raw).Ok(), "a file that stops in the middle of a box");
  });

  AddTest(tests, "IsoBmff/SniffingLooksAtBytesRatherThanAContentType", [] {
    BoxBuilder b;
    const std::size_t ftyp = b.Open("ftyp");
    b.FourCC("isom");
    b.U32(0);
    b.Close(ftyp);
    Expect(media::LooksLikeIsoBmff(b.Bytes()), "an ftyp box is the usual start");

    BoxBuilder segment;
    const std::size_t styp = segment.Open("styp");
    segment.FourCC("msdh");
    segment.Close(styp);
    Expect(media::LooksLikeIsoBmff(segment.Bytes()),
           "and a DASH media segment starts with styp and no ftyp at all");

    Expect(!media::LooksLikeIsoBmff(BytesOf("\x89PNG\r\n\x1a\n")), "a PNG is not one");
    Expect(!media::LooksLikeIsoBmff(BytesOf("abc")), "and neither is anything too short");
  });

  // The reader is the thing every rule above is enforced by, so its own failure
  // behaviour is worth stating directly: once failed, it stays failed and
  // returns zero. A parser that kept reading past a short read would be reading
  // whatever came next in memory.
  AddTest(tests, "IsoBmff/AFailedReaderStaysFailed", [] {
    const std::vector<std::byte> bytes = BytesOf("ab");
    BoxReader reader(bytes);
    ExpectEqInt(static_cast<long long>(reader.ReadU32()), 0, "a short read gives zero");
    Expect(!reader.Ok(), "and fails the reader");
    ExpectEqInt(static_cast<long long>(reader.ReadU8()), 0,
                "every later read gives zero even though a byte remains");
    Expect(reader.ReadFourCC().empty(), "including a type code");
    Expect(!reader.Skip(0), "and a skip of nothing still refuses");
    ExpectEqInt(static_cast<long long>(reader.Remaining()), 0,
                "a failed reader reports nothing remaining, so a loop over it terminates");
  });
}

}  // namespace microbrowser::tests

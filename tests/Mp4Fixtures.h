#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Building fragmented-MP4 bytes, for the tests that need some.
//
// Shared by IsoBmffTests.cpp and MediaSourceTests.cpp, and extracted the moment there were two: an
// fMP4 fixture builder copied into a second file is two builders that drift, and a *fixture* that
// drifts makes the tests over it agree with each other and with nothing else.
//
// Every fixture is built byte by byte rather than checked in as a file, for two reasons. A real MP4
// is megabytes and asserts nothing in particular, and -- more to the point -- the interesting inputs
// are the ones no encoder produces: a box that declares a size smaller than its own header, a sample
// table whose ranges leave the file, a nesting depth no player would emit. Those cannot be recorded,
// only constructed.

namespace microbrowser::tests {

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
inline void AppendVideoMoov(BoxBuilder& b) {
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

// An initialization segment: `ftyp` plus a `moov` with one H.264 track at timescale 1000. What a
// player appends first, and what a `SourceBuffer` needs before a media segment means anything.
inline std::vector<std::byte> Mp4InitSegment() {
  BoxBuilder b;
  const std::size_t ftyp = b.Open("ftyp");
  b.FourCC("iso5");
  b.U32(0);
  b.FourCC("iso5");
  b.Close(ftyp);
  AppendVideoMoov(b);
  return b.Bytes();
}

// A media segment: `moof` + `mdat` holding `count` samples of `duration` ticks each, starting at
// `base_decode_time`. At timescale 1000 a duration of 40 is 25 frames a second, so the defaults
// describe a tenth of a second of video.
inline std::vector<std::byte> Mp4MediaSegment(std::uint64_t base_decode_time, std::uint32_t count,
                                              std::uint32_t duration = 40,
                                              std::uint32_t sample_size = 10) {
  BoxBuilder b;
  const std::size_t moof_at = b.Size();
  const std::size_t moof = b.Open("moof");
  const std::size_t mfhd = b.Open("mfhd");
  b.U8(0);
  b.U24(0);
  b.U32(1);
  b.Close(mfhd);
  const std::size_t traf = b.Open("traf");
  const std::size_t tfhd = b.Open("tfhd");
  b.U8(0);
  b.U24(0x000008 | 0x000010);  // default duration and size present
  b.U32(1);                    // track id
  b.U32(duration);
  b.U32(sample_size);
  b.Close(tfhd);
  const std::size_t tfdt = b.Open("tfdt");
  b.U8(1);
  b.U24(0);
  b.U64(base_decode_time);
  b.Close(tfdt);
  const std::size_t trun = b.Open("trun");
  b.U8(0);
  b.U24(0x000001);  // data offset present, everything else defaulted
  b.U32(count);
  const std::size_t data_offset_at = b.Size();
  b.U32(0);
  b.Close(trun);
  b.Close(traf);
  b.Close(moof);

  const std::size_t mdat = b.Open("mdat");
  const std::size_t mdat_payload = b.Size();
  b.Zeros(static_cast<std::size_t>(count) * sample_size);
  b.Close(mdat);

  // `trun`'s data offset is relative to the start of the enclosing `moof`. Patched here rather than
  // computed in the parser's terms, which is what keeps a fixture from restating the rule it tests.
  std::vector<std::byte> raw = b.Bytes();
  const auto relative = static_cast<std::uint32_t>(mdat_payload - moof_at);
  for (int i = 0; i < 4; ++i) {
    raw[data_offset_at + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((relative >> (24 - 8 * i)) & 0xFFu);
  }
  return raw;
}

}  // namespace microbrowser::tests

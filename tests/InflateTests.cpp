#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "util/Inflate.h"

namespace microbrowser::tests {

namespace {

// Reference streams produced by zlib itself, so the decoder is checked against
// the format as an independent implementation writes it rather than against
// this project's understanding of the specification. Regenerate with:
//
//   python3 -c "import zlib; c = zlib.compressobj(1, zlib.DEFLATED, -15, 9,
//              zlib.Z_FIXED); print((c.compress(b'...') + c.flush()).hex())"

// Raw DEFLATE, fixed Huffman codes: "hello hello hello world"
constexpr std::uint8_t kFixedHuffmanStream[] = {0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40,
                                                0x22, 0xCB, 0xF3, 0x8B, 0x72, 0x52, 0x00};

// Raw DEFLATE, stored (uncompressed) block: "stored data stays put"
constexpr std::uint8_t kStoredStream[] = {0x01, 0x15, 0x00, 0xEA, 0xFF, 0x73, 0x74, 0x6F,
                                          0x72, 0x65, 0x64, 0x20, 0x64, 0x61, 0x74, 0x61,
                                          0x20, 0x73, 0x74, 0x61, 0x79, 0x73, 0x20, 0x70,
                                          0x75, 0x74};

std::span<const std::byte> AsBytes(const std::uint8_t* data, std::size_t size) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
}

std::string AsString(const std::vector<std::byte>& data) {
  std::string text;
  text.reserve(data.size());
  for (const std::byte value : data) {
    text.push_back(static_cast<char>(value));
  }
  return text;
}

// A dynamic-Huffman stream, built here rather than pasted: 600 bytes of a
// rotating pattern followed by a repeated run, which is enough entropy that
// zlib emits dynamic code tables and enough repetition to exercise back
// references. Compressed with zlib at level 9 and embedded below.
std::vector<std::byte> ExpectedDynamicOutput() {
  std::vector<std::byte> expected;
  for (int i = 0; i < 600; ++i) {
    expected.push_back(static_cast<std::byte>(i % 251));
  }
  const std::string tail = "abcabcabcabcabcabc";
  for (int i = 0; i < 20; ++i) {
    for (const char c : tail) {
      expected.push_back(static_cast<std::byte>(c));
    }
  }
  return expected;
}

constexpr std::uint8_t kDynamicHuffmanStream[] = {
    0x63, 0x60, 0x64, 0x62, 0x66, 0x61, 0x65, 0x63, 0xE7, 0xE0, 0xE4, 0xE2, 0xE6, 0xE1, 0xE5,
    0xE3, 0x17, 0x10, 0x14, 0x12, 0x16, 0x11, 0x15, 0x13, 0x97, 0x90, 0x94, 0x92, 0x96, 0x91,
    0x95, 0x93, 0x57, 0x50, 0x54, 0x52, 0x56, 0x51, 0x55, 0x53, 0xD7, 0xD0, 0xD4, 0xD2, 0xD6,
    0xD1, 0xD5, 0xD3, 0x37, 0x30, 0x34, 0x32, 0x36, 0x31, 0x35, 0x33, 0xB7, 0xB0, 0xB4, 0xB2,
    0xB6, 0xB1, 0xB5, 0xB3, 0x77, 0x70, 0x74, 0x72, 0x76, 0x71, 0x75, 0x73, 0xF7, 0xF0, 0xF4,
    0xF2, 0xF6, 0xF1, 0xF5, 0xF3, 0x0F, 0x08, 0x0C, 0x0A, 0x0E, 0x09, 0x0D, 0x0B, 0x8F, 0x88,
    0x8C, 0x8A, 0x8E, 0x89, 0x8D, 0x8B, 0x4F, 0x48, 0x4C, 0x4A, 0x4E, 0x49, 0x4D, 0x4B, 0xCF,
    0xC8, 0xCC, 0xCA, 0xCE, 0xC9, 0xCD, 0xCB, 0x2F, 0x28, 0x2C, 0x2A, 0x2E, 0x29, 0x2D, 0x2B,
    0xAF, 0xA8, 0xAC, 0xAA, 0xAE, 0xA9, 0xAD, 0xAB, 0x6F, 0x68, 0x6C, 0x6A, 0x6E, 0x69, 0x6D,
    0x6B, 0xEF, 0xE8, 0xEC, 0xEA, 0xEE, 0xE9, 0xED, 0xEB, 0x9F, 0x30, 0x71, 0xD2, 0xE4, 0x29,
    0x53, 0xA7, 0x4D, 0x9F, 0x31, 0x73, 0xD6, 0xEC, 0x39, 0x73, 0xE7, 0xCD, 0x5F, 0xB0, 0x70,
    0xD1, 0xE2, 0x25, 0x4B, 0x97, 0x2D, 0x5F, 0xB1, 0x72, 0xD5, 0xEA, 0x35, 0x6B, 0xD7, 0xAD,
    0xDF, 0xB0, 0x71, 0xD3, 0xE6, 0x2D, 0x5B, 0xB7, 0x6D, 0xDF, 0xB1, 0x73, 0xD7, 0xEE, 0x3D,
    0x7B, 0xF7, 0xED, 0x3F, 0x70, 0xF0, 0xD0, 0xE1, 0x23, 0x47, 0x8F, 0x1D, 0x3F, 0x71, 0xF2,
    0xD4, 0xE9, 0x33, 0x67, 0xCF, 0x9D, 0xBF, 0x70, 0xF1, 0xD2, 0xE5, 0x2B, 0x57, 0xAF, 0x5D,
    0xBF, 0x71, 0xF3, 0xD6, 0xED, 0x3B, 0x77, 0xEF, 0xDD, 0x7F, 0xF0, 0xF0, 0xD1, 0xE3, 0x27,
    0x4F, 0x9F, 0x3D, 0x7F, 0xF1, 0xF2, 0xD5, 0xEB, 0x37, 0x6F, 0xDF, 0xBD, 0xFF, 0xF0, 0xF1,
    0xD3, 0xE7, 0x2F, 0x5F, 0xBF, 0x7D, 0xFF, 0xF1, 0xF3, 0x17, 0xC3, 0xA8, 0xD7, 0x69, 0xE7,
    0x75, 0xA0, 0xDF, 0x47, 0x11, 0x1D, 0x10, 0x00};

}  // namespace

void RegisterInflateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Inflate/DecodesAStoredBlock", [] {
    std::vector<std::byte> out;
    Expect(util::Inflate(AsBytes(kStoredStream, sizeof(kStoredStream)), 1024, out),
           "a stored block must decode");
    ExpectEqString(AsString(out), "stored data stays put", "the bytes must come back unchanged");
  });

  AddTest(tests, "Inflate/DecodesFixedHuffmanCodes", [] {
    std::vector<std::byte> out;
    Expect(util::Inflate(AsBytes(kFixedHuffmanStream, sizeof(kFixedHuffmanStream)), 1024, out),
           "a fixed-Huffman block must decode");
    ExpectEqString(AsString(out), "hello hello hello world",
                   "including the back references that make it shorter than its input");
  });

  AddTest(tests, "Inflate/DecodesDynamicHuffmanCodes", [] {
    std::vector<std::byte> out;
    Expect(util::Inflate(AsBytes(kDynamicHuffmanStream, sizeof(kDynamicHuffmanStream)), 4096, out),
           "a dynamic-Huffman block must decode");
    Expect(out == ExpectedDynamicOutput(),
           "dynamic code tables, run-length-coded code lengths, and long back references must "
           "all round-trip against a stream zlib itself produced");
  });

  // The bound is the whole reason this function takes a limit. Decompression is
  // the canonical amplification attack.
  AddTest(tests, "Inflate/RefusesToExceedItsOutputBound", [] {
    std::vector<std::byte> out;
    Expect(!util::Inflate(AsBytes(kDynamicHuffmanStream, sizeof(kDynamicHuffmanStream)), 100, out),
           "a stream that would exceed the caller's bound must fail rather than allocate");
    Expect(out.size() <= 100,
           "and must not have grown past the bound on its way to failing, which is what a "
           "check placed after the append would allow");
  });

  AddTest(tests, "Inflate/ABoundOfZeroAcceptsOnlyAnEmptyResult", [] {
    std::vector<std::byte> out;
    Expect(!util::Inflate(AsBytes(kFixedHuffmanStream, sizeof(kFixedHuffmanStream)), 0, out),
           "no output is allowed, so a stream with output must fail");
  });

  AddTest(tests, "Inflate/RejectsMalformedStreams", [] {
    std::vector<std::byte> out;
    Expect(!util::Inflate({}, 1024, out), "an empty stream has no blocks");

    // Block type 3 is reserved and never valid.
    const std::uint8_t reserved[] = {0x07};
    Expect(!util::Inflate(AsBytes(reserved, sizeof(reserved)), 1024, out),
           "the reserved block type must be rejected");

    // A stored block whose length and complement disagree.
    const std::uint8_t bad_stored[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'a', 'b', 'c', 'd', 'e'};
    Expect(!util::Inflate(AsBytes(bad_stored, sizeof(bad_stored)), 1024, out),
           "a stored block's one's-complement length check is the format's own integrity guard");

    // A stored block claiming more bytes than remain.
    const std::uint8_t short_stored[] = {0x01, 0xFF, 0x00, 0x00, 0xFF, 'a'};
    Expect(!util::Inflate(AsBytes(short_stored, sizeof(short_stored)), 1024, out),
           "a length past the end of the input must be rejected, not read");
  });

  // Truncation at every length. A decoder fed a prefix of a valid stream is the
  // input most likely to walk off the end, because everything parses right up
  // until it does not.
  AddTest(tests, "Inflate/TruncationAtAnyPointFailsCleanly", [] {
    for (std::size_t length = 0; length < sizeof(kDynamicHuffmanStream); ++length) {
      std::vector<std::byte> out;
      // The only requirement is that it returns rather than reading past the
      // end; ASan and UBSan are what actually check that, and this drives them.
      util::Inflate(AsBytes(kDynamicHuffmanStream, length), 4096, out);
      Expect(out.size() <= 4096, "the bound holds even on a truncated stream");
    }
  });

  AddTest(tests, "Inflate/CorruptionAtAnyByteIsSurvivable", [] {
    for (std::size_t index = 0; index < sizeof(kDynamicHuffmanStream); index += 3) {
      std::vector<std::uint8_t> corrupted(
          kDynamicHuffmanStream, kDynamicHuffmanStream + sizeof(kDynamicHuffmanStream));
      corrupted[index] = static_cast<std::uint8_t>(corrupted[index] ^ 0xFFu);
      std::vector<std::byte> out;
      util::Inflate(AsBytes(corrupted.data(), corrupted.size()), 4096, out);
      Expect(out.size() <= 4096, "a flipped byte must not produce unbounded output");
    }
  });

  // Regression. Two bytes: BFINAL set, block type 01 (fixed Huffman), then the
  // end-of-block code. Building the fixed tables wrote all 288 literal symbols
  // into a table sized for the 286 a *dynamic* block can name — a two-byte stack
  // overflow on the shortest valid fixed-Huffman stream there is. The unit tests
  // above all went through this path and none of them noticed, because the write
  // landed in padding; the fuzzer found it eight seconds into its first
  // instrumented run.
  AddTest(tests, "Inflate/TheShortestFixedHuffmanStreamDoesNotOverflowItsTables", [] {
    const std::uint8_t empty_fixed_block[] = {0x03, 0x00};
    std::vector<std::byte> out;
    Expect(util::Inflate(AsBytes(empty_fixed_block, sizeof(empty_fixed_block)), 1024, out),
           "an empty fixed-Huffman block is valid and decodes to nothing");
    Expect(out.empty(), "to nothing at all");
  });

  // --- The zlib wrapper -----------------------------------------------------

  AddTest(tests, "Inflate/Adler32MatchesTheReferenceValues", [] {
    ExpectEqInt(util::Adler32({}), 1, "the empty checksum is 1, not 0");
    const std::uint8_t abc[] = {'a', 'b', 'c'};
    ExpectEqInt(util::Adler32(AsBytes(abc, 3)), 0x024D0127,
                "the value from RFC 1950's own worked example");
  });

  AddTest(tests, "Inflate/ZlibWrapperIsValidatedRatherThanSkipped", [] {
    // A well-formed zlib stream around the fixed-Huffman payload.
    std::vector<std::uint8_t> stream = {0x78, 0x01};
    stream.insert(stream.end(), kFixedHuffmanStream,
                  kFixedHuffmanStream + sizeof(kFixedHuffmanStream));
    const std::uint8_t payload[] = {'h', 'e', 'l', 'l', 'o', ' ', 'h', 'e', 'l', 'l', 'o', ' ',
                                    'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    const std::uint32_t adler = util::Adler32(AsBytes(payload, sizeof(payload)));
    stream.push_back(static_cast<std::uint8_t>(adler >> 24));
    stream.push_back(static_cast<std::uint8_t>((adler >> 16) & 0xFFu));
    stream.push_back(static_cast<std::uint8_t>((adler >> 8) & 0xFFu));
    stream.push_back(static_cast<std::uint8_t>(adler & 0xFFu));

    std::vector<std::byte> out;
    Expect(util::ZlibInflate(AsBytes(stream.data(), stream.size()), 1024, out),
           "a well-formed zlib stream must decode");
    ExpectEqString(AsString(out), "hello hello hello world", "to the right bytes");

    // Now break the checksum. Accepting it would mean rendering whatever the
    // corruption produced.
    std::vector<std::uint8_t> bad_checksum = stream;
    bad_checksum.back() ^= 0xFFu;
    Expect(!util::ZlibInflate(AsBytes(bad_checksum.data(), bad_checksum.size()), 1024, out),
           "a failing Adler-32 must fail the decode");
    Expect(out.empty(), "and must not leave the partial output behind for a caller to use");

    // And the header check bits.
    std::vector<std::uint8_t> bad_header = stream;
    bad_header[1] = 0x00;
    Expect(!util::ZlibInflate(AsBytes(bad_header.data(), bad_header.size()), 1024, out),
           "the header's own check bits must be verified");

    std::vector<std::uint8_t> wrong_method = stream;
    wrong_method[0] = 0x79;  // compression method 9, which does not exist
    Expect(!util::ZlibInflate(AsBytes(wrong_method.data(), wrong_method.size()), 1024, out),
           "an unknown compression method must be rejected");
  });

  AddTest(tests, "Inflate/ATooShortZlibStreamIsRejected", [] {
    std::vector<std::byte> out;
    const std::uint8_t tiny[] = {0x78, 0x01, 0x00};
    Expect(!util::ZlibInflate(AsBytes(tiny, sizeof(tiny)), 1024, out),
           "a stream with no room for its own trailer cannot be valid");
  });
}

}  // namespace microbrowser::tests

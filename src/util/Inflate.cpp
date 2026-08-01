#include "util/Inflate.h"

#include <array>
#include <cstdint>

#include "util/PerformanceCounters.h"

namespace microbrowser::util {

namespace {

constexpr int kMaxCodeLength = 15;
// The dynamic literal alphabet tops out at 286 (HLIT is 257 + 5 bits, but
// codes 286 and 287 are never valid in a dynamic block). The *fixed* alphabet
// defines all 288, and sizing the symbol table for the smaller of the two was a
// two-byte stack overflow that the fuzzer found on its first instrumented run.
constexpr int kLiteralCodes = 286;
constexpr int kFixedLiteralCodes = 288;
constexpr int kDistanceCodes = 30;
constexpr int kCodeLengthCodes = 19;

// Bit-at-a-time reader, LSB first, with a sticky failure flag.
//
// Sticky rather than throwing, for the same reason ipc::ByteReader is: a
// decoder can run straight through and check once, instead of testing after
// every read and getting one of them wrong.
class BitReader {
 public:
  explicit BitReader(std::span<const std::byte> data) : data_(data) {}

  bool Ok() const { return ok_; }

  // Up to 16 bits. Returns 0 and fails once the input runs out — never reads
  // past the end and never returns uninitialized bits as data.
  std::uint32_t Bits(int need) {
    while (bit_count_ < need) {
      if (!ok_ || position_ >= data_.size()) {
        ok_ = false;
        return 0;
      }
      buffer_ |= static_cast<std::uint32_t>(data_[position_++]) << bit_count_;
      bit_count_ += 8;
    }
    const std::uint32_t value = buffer_ & ((1u << need) - 1u);
    buffer_ >>= need;
    bit_count_ -= need;
    return value;
  }

  void AlignToByte() {
    buffer_ = 0;
    bit_count_ = 0;
  }

  std::size_t BytePosition() const { return position_; }
  std::span<const std::byte> Data() const { return data_; }
  void SkipBytes(std::size_t count) {
    if (count > data_.size() - position_) {
      ok_ = false;
      position_ = data_.size();
      return;
    }
    position_ += count;
  }
  void Fail() { ok_ = false; }

 private:
  std::span<const std::byte> data_;
  std::size_t position_ = 0;
  std::uint32_t buffer_ = 0;
  int bit_count_ = 0;
  bool ok_ = true;
};

// A canonical Huffman table, in the counts-and-symbols form the DEFLATE spec
// itself describes: `count[n]` codes of length n, and `symbol` holding the
// symbols ordered by code.
struct HuffmanTable {
  std::array<std::int16_t, kMaxCodeLength + 1> count{};
  std::array<std::int16_t, kFixedLiteralCodes> symbol{};
};

// Returns false for an over-subscribed set (more codes of some length than the
// tree can hold), which is malformed input rather than a corner case. An
// *under*-subscribed set is allowed only when it has at most one code, because
// a single distance code is legal and appears in real streams.
bool BuildHuffman(HuffmanTable& table, const std::int16_t* lengths, int count) {
  // The guard the original version lacked. Every caller is in this file and
  // every one of them is bounded, which is exactly the reasoning that let a
  // 288-symbol alphabet be written into a 286-symbol table.
  if (count < 0 || count > static_cast<int>(table.symbol.size())) {
    return false;
  }
  table.count.fill(0);
  for (int symbol = 0; symbol < count; ++symbol) {
    ++table.count[static_cast<std::size_t>(lengths[symbol])];
  }
  if (table.count[0] == count) {
    return true;  // no codes at all; decoding will fail if one is needed
  }

  int left = 1;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    left <<= 1;
    left -= table.count[static_cast<std::size_t>(length)];
    if (left < 0) {
      return false;  // over-subscribed
    }
  }

  std::array<std::int16_t, kMaxCodeLength + 1> offsets{};
  offsets[1] = 0;
  for (int length = 1; length < kMaxCodeLength; ++length) {
    offsets[static_cast<std::size_t>(length) + 1] = static_cast<std::int16_t>(
        offsets[static_cast<std::size_t>(length)] + table.count[static_cast<std::size_t>(length)]);
  }
  for (int symbol = 0; symbol < count; ++symbol) {
    if (lengths[symbol] != 0) {
      table.symbol[static_cast<std::size_t>(offsets[static_cast<std::size_t>(lengths[symbol])]++)] =
          static_cast<std::int16_t>(symbol);
    }
  }
  return true;
}

// One symbol, walked bit by bit down the code lengths.
int DecodeSymbol(BitReader& reader, const HuffmanTable& table) {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    code |= static_cast<int>(reader.Bits(1));
    if (!reader.Ok()) {
      return -1;
    }
    const int count = table.count[static_cast<std::size_t>(length)];
    if (code - count < first) {
      return table.symbol[static_cast<std::size_t>(index + (code - first))];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;  // no code of any valid length matched
}

// Length and distance bases and extra bits, from RFC 1951 section 3.2.5.
constexpr std::array<std::uint16_t, 29> kLengthBase = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13};

bool InflateBlock(BitReader& reader, const HuffmanTable& literals, const HuffmanTable& distances,
                  std::size_t max_output, std::vector<std::byte>& out) {
  while (true) {
    const int symbol = DecodeSymbol(reader, literals);
    if (symbol < 0) {
      return false;
    }
    if (symbol < 256) {
      if (out.size() >= max_output) {
        return false;
      }
      out.push_back(static_cast<std::byte>(symbol));
      continue;
    }
    if (symbol == 256) {
      return true;  // end of block
    }

    const int length_index = symbol - 257;
    if (length_index >= static_cast<int>(kLengthBase.size())) {
      return false;  // 286 and 287 are defined but never valid
    }
    const std::size_t length =
        kLengthBase[static_cast<std::size_t>(length_index)] +
        reader.Bits(kLengthExtra[static_cast<std::size_t>(length_index)]);

    const int distance_symbol = DecodeSymbol(reader, distances);
    if (distance_symbol < 0 || distance_symbol >= static_cast<int>(kDistanceBase.size())) {
      return false;
    }
    const std::size_t distance =
        kDistanceBase[static_cast<std::size_t>(distance_symbol)] +
        reader.Bits(kDistanceExtra[static_cast<std::size_t>(distance_symbol)]);
    if (!reader.Ok()) {
      return false;
    }
    // A distance reaching before the start of the output is the classic
    // out-of-bounds read in a DEFLATE implementation, and it is one comparison
    // away from being impossible.
    if (distance == 0 || distance > out.size()) {
      return false;
    }
    if (length > max_output - out.size()) {
      return false;
    }

    for (std::size_t i = 0; i < length; ++i) {
      // Read into a local first. `out` may reallocate inside push_back, which
      // would leave a reference into the old buffer dangling — the copy is not
      // a style choice.
      const std::byte value = out[out.size() - distance];
      out.push_back(value);
    }
  }
}

bool InflateStored(BitReader& reader, std::size_t max_output, std::vector<std::byte>& out) {
  reader.AlignToByte();
  const std::span<const std::byte> data = reader.Data();
  const std::size_t start = reader.BytePosition();
  if (data.size() - start < 4) {
    return false;
  }
  const auto length = static_cast<std::size_t>(
      static_cast<std::uint32_t>(data[start]) |
      (static_cast<std::uint32_t>(data[start + 1]) << 8));
  const auto complement = static_cast<std::uint16_t>(
      static_cast<std::uint32_t>(data[start + 2]) |
      (static_cast<std::uint32_t>(data[start + 3]) << 8));
  // The one's complement check is the format's own integrity guard; skipping it
  // means accepting a length that was never written.
  if (static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length)) != complement) {
    return false;
  }
  reader.SkipBytes(4);
  if (!reader.Ok() || length > data.size() - reader.BytePosition() ||
      length > max_output - out.size()) {
    return false;
  }
  const std::size_t from = reader.BytePosition();
  out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(from),
             data.begin() + static_cast<std::ptrdiff_t>(from + length));
  reader.SkipBytes(length);
  return reader.Ok();
}

void BuildFixedTables(HuffmanTable& literals, HuffmanTable& distances) {
  std::array<std::int16_t, kFixedLiteralCodes> lengths{};
  for (int symbol = 0; symbol < 144; ++symbol) {
    lengths[static_cast<std::size_t>(symbol)] = 8;
  }
  for (int symbol = 144; symbol < 256; ++symbol) {
    lengths[static_cast<std::size_t>(symbol)] = 9;
  }
  for (int symbol = 256; symbol < 280; ++symbol) {
    lengths[static_cast<std::size_t>(symbol)] = 7;
  }
  for (int symbol = 280; symbol < 288; ++symbol) {
    lengths[static_cast<std::size_t>(symbol)] = 8;
  }
  BuildHuffman(literals, lengths.data(), kFixedLiteralCodes);

  std::array<std::int16_t, kDistanceCodes> distance_lengths{};
  distance_lengths.fill(5);
  BuildHuffman(distances, distance_lengths.data(), kDistanceCodes);
}

bool ReadDynamicTables(BitReader& reader, HuffmanTable& literals, HuffmanTable& distances) {
  const int literal_count = static_cast<int>(reader.Bits(5)) + 257;
  const int distance_count = static_cast<int>(reader.Bits(5)) + 1;
  const int length_count = static_cast<int>(reader.Bits(4)) + 4;
  if (!reader.Ok() || literal_count > kLiteralCodes || distance_count > kDistanceCodes) {
    return false;
  }

  // The code-length alphabet arrives in this permuted order so that the most
  // common lengths come first and trailing zeros can be omitted.
  static constexpr std::array<std::uint8_t, kCodeLengthCodes> kOrder = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  std::array<std::int16_t, kCodeLengthCodes> code_lengths{};
  for (int i = 0; i < length_count; ++i) {
    code_lengths[kOrder[static_cast<std::size_t>(i)]] =
        static_cast<std::int16_t>(reader.Bits(3));
  }
  if (!reader.Ok()) {
    return false;
  }

  HuffmanTable length_table;
  if (!BuildHuffman(length_table, code_lengths.data(), kCodeLengthCodes)) {
    return false;
  }

  std::array<std::int16_t, kLiteralCodes + kDistanceCodes> lengths{};
  int index = 0;
  while (index < literal_count + distance_count) {
    const int symbol = DecodeSymbol(reader, length_table);
    if (symbol < 0) {
      return false;
    }
    if (symbol < 16) {
      lengths[static_cast<std::size_t>(index++)] = static_cast<std::int16_t>(symbol);
      continue;
    }

    std::int16_t value = 0;
    int repeat = 0;
    if (symbol == 16) {
      if (index == 0) {
        return false;  // nothing to repeat
      }
      value = lengths[static_cast<std::size_t>(index) - 1];
      repeat = 3 + static_cast<int>(reader.Bits(2));
    } else if (symbol == 17) {
      repeat = 3 + static_cast<int>(reader.Bits(3));
    } else {
      repeat = 11 + static_cast<int>(reader.Bits(7));
    }
    if (!reader.Ok() || index + repeat > literal_count + distance_count) {
      return false;  // a run that overflows the table is malformed, not clamped
    }
    for (int i = 0; i < repeat; ++i) {
      lengths[static_cast<std::size_t>(index++)] = value;
    }
  }

  // The end-of-block code must exist, or the stream can never terminate.
  if (lengths[256] == 0) {
    return false;
  }
  if (!BuildHuffman(literals, lengths.data(), literal_count)) {
    return false;
  }
  return BuildHuffman(distances, lengths.data() + literal_count, distance_count);
}

}  // namespace

bool Inflate(std::span<const std::byte> input, std::size_t max_output,
             std::vector<std::byte>& out) {
  out.clear();
  BitReader reader(input);
  AddPerformanceCounter(PerfCounterId::UtilInflateCalls);

  bool final_block = false;
  while (!final_block) {
    final_block = reader.Bits(1) != 0;
    const std::uint32_t type = reader.Bits(2);
    if (!reader.Ok()) {
      return false;
    }

    bool ok = false;
    switch (type) {
      case 0:
        ok = InflateStored(reader, max_output, out);
        break;
      case 1: {
        HuffmanTable literals;
        HuffmanTable distances;
        BuildFixedTables(literals, distances);
        ok = InflateBlock(reader, literals, distances, max_output, out);
        break;
      }
      case 2: {
        HuffmanTable literals;
        HuffmanTable distances;
        ok = ReadDynamicTables(reader, literals, distances) &&
             InflateBlock(reader, literals, distances, max_output, out);
        break;
      }
      default:
        return false;  // type 3 is reserved and never valid
    }
    if (!ok || !reader.Ok()) {
      return false;
    }
  }

  AddPerformanceCounter(PerfCounterId::UtilInflateBytes, out.size());
  return true;
}

std::uint32_t Adler32(std::span<const std::byte> data) {
  constexpr std::uint32_t kModulus = 65521;
  std::uint32_t low = 1;
  std::uint32_t high = 0;
  for (const std::byte value : data) {
    low = (low + static_cast<std::uint32_t>(value)) % kModulus;
    high = (high + low) % kModulus;
  }
  return (high << 16) | low;
}

bool ZlibInflate(std::span<const std::byte> input, std::size_t max_output,
                 std::vector<std::byte>& out) {
  out.clear();
  // Two header bytes and a four-byte Adler-32 trailer.
  if (input.size() < 6) {
    return false;
  }
  const auto cmf = static_cast<std::uint32_t>(input[0]);
  const auto flg = static_cast<std::uint32_t>(input[1]);
  if ((cmf & 0x0Fu) != 8) {
    return false;  // compression method must be deflate
  }
  if (((cmf & 0xF0u) >> 4) > 7) {
    return false;  // window larger than 32K
  }
  if (((cmf << 8) | flg) % 31 != 0) {
    return false;  // header check bits
  }
  if ((flg & 0x20u) != 0) {
    return false;  // a preset dictionary, which PNG never uses
  }

  const std::size_t trailer = input.size() - 4;
  if (!Inflate(input.subspan(2, trailer - 2), max_output, out)) {
    return false;
  }

  const std::uint32_t expected = (static_cast<std::uint32_t>(input[trailer]) << 24) |
                                 (static_cast<std::uint32_t>(input[trailer + 1]) << 16) |
                                 (static_cast<std::uint32_t>(input[trailer + 2]) << 8) |
                                 static_cast<std::uint32_t>(input[trailer + 3]);
  if (Adler32(out) != expected) {
    AddPerformanceCounter(PerfCounterId::UtilInflateChecksumFailures);
    out.clear();
    return false;
  }
  return true;
}

}  // namespace microbrowser::util

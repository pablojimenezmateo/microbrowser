#include "util/Inflate.h"

#include <array>
#include <cstdint>
#include <cstring>

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

  // Loads *up to* `need` bits into the buffer and says how many are really
  // there. Unlike `Bits` it does not fail at the end of the input: a Huffman
  // code shorter than the peek window is still decodable from the last few bits
  // of a stream, and a reader that failed on the peek could not decode it.
  // The caller is responsible for checking the code it matched against the
  // returned count — see DecodeSymbol.
  int Fill(int need) {
    while (bit_count_ < need && position_ < data_.size()) {
      buffer_ |= static_cast<std::uint32_t>(data_[position_++]) << bit_count_;
      bit_count_ += 8;
    }
    return bit_count_;
  }

  // The next `need` bits without consuming them, zero-filled past the end of
  // what `Fill` actually loaded.
  std::uint32_t Peek(int need) const { return buffer_ & ((1u << need) - 1u); }

  void Consume(int need) {
    buffer_ >>= need;
    bit_count_ -= need;
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

// How many bits the direct-index table covers.
//
// Nine, because DEFLATE's fixed literal alphabet tops out at 9 and a dynamic
// one puts its common symbols well inside that: the table is 512 entries -- one
// kilobyte, rebuilt per block -- and it decodes the overwhelming majority of
// symbols with one array read instead of one loop iteration and one function
// call *per bit*. Going wider costs build time on every block for codes that
// are rare by construction, since a long code is by definition an uncommon
// symbol.
constexpr int kFastBits = 9;
constexpr int kFastSize = 1 << kFastBits;

// A canonical Huffman table, in the counts-and-symbols form the DEFLATE spec
// itself describes: `count[n]` codes of length n, and `symbol` holding the
// symbols ordered by code.
//
// Plus `fast`, which is the same information indexed by the next `kFastBits` of
// input. An entry is `(length << 9) | symbol`, and zero means "no code of
// `kFastBits` or fewer starts this way" -- unambiguous because a real entry has
// a length of at least one and is therefore at least 512.
struct HuffmanTable {
  std::array<std::int16_t, kMaxCodeLength + 1> count{};
  std::array<std::int16_t, kFixedLiteralCodes> symbol{};
  std::array<std::uint16_t, kFastSize> fast{};
};

// The low `length` bits of `code`, reversed. DEFLATE writes Huffman codes
// most-significant bit first and everything else least-significant bit first,
// so the direct-index table has to be built in the order the reader will see.
std::uint32_t ReverseBits(std::uint32_t code, int length) {
  std::uint32_t reversed = 0;
  for (int i = 0; i < length; ++i) {
    reversed = (reversed << 1) | ((code >> i) & 1u);
  }
  return reversed;
}

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

  // The direct-index table, from the canonical code assignment the loop above
  // just implied: codes of each length run consecutively in `symbol` order, and
  // the first code of length n+1 is (first code of length n + count[n]) << 1.
  table.fast.fill(0);
  std::uint32_t code = 0;
  int index = 0;
  for (int length = 1; length <= kMaxCodeLength; ++length) {
    const int at_this_length = table.count[static_cast<std::size_t>(length)];
    for (int i = 0; i < at_this_length; ++i, ++index, ++code) {
      if (length > kFastBits) {
        continue;  // decoded by the bit walk below; too long to index
      }
      const std::int16_t decoded = table.symbol[static_cast<std::size_t>(index)];
      const auto entry =
          static_cast<std::uint16_t>((static_cast<std::uint32_t>(length) << 9) |
                                     static_cast<std::uint32_t>(decoded));
      // Every index whose low `length` bits are this code, since the bits above
      // it belong to whatever symbol comes next and are not known yet.
      for (std::uint32_t fill = ReverseBits(code, length);
           fill < static_cast<std::uint32_t>(kFastSize); fill += 1u << length) {
        table.fast[fill] = entry;
      }
    }
    code <<= 1;
  }
  return true;
}

// One symbol.
//
// The common case is one array read: peek `kFastBits`, index the table, consume
// the length it reports. This used to be a loop that called `Bits(1)` once *per
// bit* of every code, which is why gzip decoded at around a tenth of the speed
// it should -- see TD-0006.
int DecodeSymbol(BitReader& reader, const HuffmanTable& table) {
  const int available = reader.Fill(kFastBits);
  const std::uint16_t entry = table.fast[reader.Peek(kFastBits)];
  if (entry != 0) {
    const int length = static_cast<int>(entry >> 9);
    // Past the end of the input `Peek` zero-fills, so a code may have "matched"
    // on bits that are not there. It is a real match only when every bit it
    // used exists.
    if (length > available) {
      reader.Fail();
      return -1;
    }
    reader.Consume(length);
    return static_cast<int>(entry & 0x1FFu);
  }

  // A code longer than the window. Rare by construction -- a long code is an
  // uncommon symbol -- so this stays the original bit walk rather than growing
  // a second table.
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

    // Grown once and then copied, rather than one `push_back` per byte: a
    // match averages a few dozen bytes and every one of them was costing a
    // capacity check and a possible reallocation. The pointers are taken
    // *after* the resize for the reason the old comment gave -- the buffer may
    // move -- which is why this is a resize followed by a copy and not a copy
    // into a reserved tail.
    const std::size_t start = out.size();
    out.resize(start + length);
    std::byte* destination = out.data() + start;
    const std::byte* source = out.data() + start - distance;
    if (distance >= length) {
      // No overlap: the whole match is already in the output and can move in
      // one go.
      std::memcpy(destination, source, length);
    } else {
      // Overlapping, which in LZ77 is not a mistake but the way a run is
      // encoded -- `distance` 1 and `length` 200 means two hundred copies of
      // one byte. It has to be copied forwards, one byte at a time, because
      // each byte written is an input to a later one. `memmove` would be
      // wrong here, not merely slower.
      for (std::size_t i = 0; i < length; ++i) {
        destination[i] = source[i];
      }
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

std::uint32_t Crc32(std::span<const std::byte> data) {
  // The table is built once on first use rather than baked in, because a 1KB
  // constant nobody can verify by eye is worse than eight lines that generate
  // it.
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      result[n] = c;
    }
    return result;
  }();

  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte value : data) {
    crc = table[(crc ^ static_cast<std::uint32_t>(value)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
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

std::optional<std::uint32_t> GzipDeclaredSize(std::span<const std::byte> input) {
  if (input.size() < 18) {  // the smallest member that could hold a trailer
    return std::nullopt;
  }
  const std::size_t at = input.size() - 4;
  return static_cast<std::uint32_t>(input[at]) |
         (static_cast<std::uint32_t>(input[at + 1]) << 8) |
         (static_cast<std::uint32_t>(input[at + 2]) << 16) |
         (static_cast<std::uint32_t>(input[at + 3]) << 24);
}

bool GzipInflate(std::span<const std::byte> input, std::size_t max_output,
                 std::vector<std::byte>& out) {
  out.clear();
  // Ten header bytes and an eight-byte trailer, before any of the optional
  // fields. Anything shorter cannot be a member however it is read, and
  // checking it here is what makes every subspan below in range.
  constexpr std::size_t kFixedHeader = 10;
  constexpr std::size_t kTrailer = 8;
  if (input.size() < kFixedHeader + kTrailer) {
    return false;
  }
  if (input[0] != std::byte{0x1F} || input[1] != std::byte{0x8B}) {
    return false;  // not a gzip member
  }
  if (input[2] != std::byte{8}) {
    return false;  // compression method must be deflate
  }
  const auto flags = static_cast<std::uint32_t>(input[3]);
  if ((flags & 0xE0u) != 0) {
    return false;  // reserved bits, which a conforming writer leaves clear
  }

  // The optional fields, each of which is a length the sender chose. Every step
  // is bounded against `limit` rather than against the whole input, because the
  // trailer is not part of the header however long the sender claims a field is.
  const std::size_t limit = input.size() - kTrailer;
  std::size_t at = kFixedHeader;
  if ((flags & 0x04u) != 0) {  // FEXTRA
    if (limit - at < 2) {
      return false;
    }
    const std::size_t extra = static_cast<std::size_t>(input[at]) |
                              (static_cast<std::size_t>(input[at + 1]) << 8);
    at += 2;
    if (limit - at < extra) {
      return false;
    }
    at += extra;
  }
  for (const std::uint32_t field : {0x08u, 0x10u}) {  // FNAME, FCOMMENT
    if ((flags & field) == 0) {
      continue;
    }
    while (true) {
      if (at >= limit) {
        return false;  // an unterminated string is a truncated member
      }
      const bool end = input[at] == std::byte{0};
      ++at;
      if (end) {
        break;
      }
    }
  }
  if ((flags & 0x02u) != 0) {  // FHCRC
    if (limit - at < 2) {
      return false;
    }
    const std::uint32_t stored = static_cast<std::uint32_t>(input[at]) |
                                 (static_cast<std::uint32_t>(input[at + 1]) << 8);
    if ((Crc32(input.subspan(0, at)) & 0xFFFFu) != stored) {
      AddPerformanceCounter(PerfCounterId::UtilInflateChecksumFailures);
      return false;
    }
    at += 2;
  }

  if (!Inflate(input.subspan(at, limit - at), max_output, out)) {
    return false;
  }

  const auto trailer_word = [input](std::size_t offset) {
    return static_cast<std::uint32_t>(input[offset]) |
           (static_cast<std::uint32_t>(input[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(input[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(input[offset + 3]) << 24);
  };
  const std::uint32_t expected_crc = trailer_word(limit);
  const std::uint32_t expected_size = trailer_word(limit + 4);
  // ISIZE is the length modulo 2^32, which is what a 4GB member would have
  // wrapped. `max_output` is far below that here, so the comparison is exact.
  if (Crc32(out) != expected_crc ||
      static_cast<std::uint32_t>(out.size() & 0xFFFFFFFFu) != expected_size) {
    AddPerformanceCounter(PerfCounterId::UtilInflateChecksumFailures);
    out.clear();
    return false;
  }
  return true;
}

}  // namespace microbrowser::util

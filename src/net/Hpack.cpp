#include "net/Hpack.h"

#include <array>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::net::hpack {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// RFC 7541 Appendix A. Index 1 through 61; entry 0 is a placeholder so that the
// array index and the protocol index are the same number, because an off-by-one
// here is a header that decodes to a different field rather than a crash.
struct StaticEntry {
  std::string_view name;
  std::string_view value;
};

constexpr std::array<StaticEntry, 62> kStaticTable = {{
    {"", ""},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

constexpr std::size_t kStaticEntries = kStaticTable.size() - 1;

// RFC 7541 Appendix B, as code *lengths* for symbols 0..255 plus EOS at 256.
//
// The Huffman code there is canonical — symbols of equal length get consecutive
// codes in symbol order, and each length starts where the previous one left off
// shifted up by one. So the codes follow from the lengths and do not have to be
// written down, which matters because a length table can be checked: the sum of
// 2^-length over a complete prefix code is exactly one (Kraft), and `HpackTests`
// asserts it. A single mistyped entry there fails the build. A mistyped *code*
// in a code table would not.
constexpr std::array<std::uint8_t, 257> kHuffmanLengths = {{
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    6,  10, 10, 12, 13, 6,  8,  11, 10, 10, 8,  11, 8,  6,  6,  6,
    5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  7,  8,  15, 6,  12, 10,
    13, 6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  8,  7,  8,  13, 19, 13, 14, 6,
    15, 5,  6,  5,  6,  5,  6,  6,  6,  5,  7,  7,  6,  6,  6,  5,
    6,  7,  6,  5,  5,  6,  7,  7,  7,  7,  7,  15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
    30,
}};

constexpr unsigned kMaxCodeBits = 30;
constexpr std::uint16_t kEndOfString = 256;

// The canonical code, built once. Two views of the same assignment: `codes`
// for encoding, and the per-length first-code/first-index pair for decoding,
// which is the standard canonical decoder and needs no tree.
struct HuffmanCode {
  std::array<std::uint32_t, 257> codes{};
  // Indexed by length. `count[l]` symbols of length `l` start at code
  // `first_code[l]` and at `first_symbol[l]` in `sorted`.
  std::array<std::uint32_t, kMaxCodeBits + 1> first_code{};
  std::array<std::uint32_t, kMaxCodeBits + 1> first_symbol{};
  std::array<std::uint16_t, kMaxCodeBits + 1> count{};
  std::array<std::uint16_t, 257> sorted{};
};

HuffmanCode BuildHuffmanCode() {
  HuffmanCode built;
  for (std::size_t symbol = 0; symbol < kHuffmanLengths.size(); ++symbol) {
    ++built.count[kHuffmanLengths[symbol]];
  }
  std::uint32_t code = 0;
  std::uint32_t index = 0;
  for (unsigned length = 1; length <= kMaxCodeBits; ++length) {
    built.first_code[length] = code;
    built.first_symbol[length] = index;
    for (std::size_t symbol = 0; symbol < kHuffmanLengths.size(); ++symbol) {
      if (kHuffmanLengths[symbol] != length) {
        continue;
      }
      built.codes[symbol] = code;
      built.sorted[index] = static_cast<std::uint16_t>(symbol);
      ++code;
      ++index;
    }
    code <<= 1;
  }
  return built;
}

const HuffmanCode& Huffman() {
  static const HuffmanCode built = BuildHuffmanCode();
  return built;
}

// RFC 7541 §4.1: name plus value plus 32, the constant standing for the
// per-entry overhead a real implementation pays. It is part of the protocol
// rather than an estimate — without it a table of ten thousand empty headers
// would cost nothing by the peer's accounting and a great deal by ours.
std::size_t EntrySize(const Header& field) {
  return field.name.size() + field.value.size() + 32;
}

}  // namespace

bool IsSensitiveHeader(std::string_view name) {
  return util::EqualsAsciiCaseInsensitive(name, "cookie") ||
         util::EqualsAsciiCaseInsensitive(name, "authorization") ||
         util::EqualsAsciiCaseInsensitive(name, "proxy-authorization") ||
         util::EqualsAsciiCaseInsensitive(name, "set-cookie");
}

// --- Huffman ----------------------------------------------------------------

unsigned HuffmanCodeLength(std::uint16_t symbol) {
  return symbol < kHuffmanLengths.size() ? kHuffmanLengths[symbol] : 0;
}

std::size_t HuffmanEncodedSize(std::string_view text) {
  std::size_t bits = 0;
  for (const char c : text) {
    bits += kHuffmanLengths[static_cast<unsigned char>(c)];
  }
  return (bits + 7) / 8;
}

void HuffmanEncode(std::string_view text, std::string& out) {
  const HuffmanCode& table = Huffman();
  std::uint64_t accumulator = 0;
  unsigned held = 0;
  for (const char c : text) {
    const auto symbol = static_cast<unsigned char>(c);
    const unsigned length = kHuffmanLengths[symbol];
    accumulator = (accumulator << length) | table.codes[symbol];
    held += length;
    while (held >= 8) {
      held -= 8;
      out.push_back(static_cast<char>((accumulator >> held) & 0xFF));
    }
  }
  if (held > 0) {
    // Padded with the EOS prefix, which is all ones. Any other padding is what
    // a decoder is required to reject, so producing any other padding would be
    // producing something a conforming peer must refuse.
    const unsigned pad = 8 - held;
    accumulator = (accumulator << pad) | ((1u << pad) - 1);
    out.push_back(static_cast<char>(accumulator & 0xFF));
  }
}

bool HuffmanDecode(std::span<const std::byte> data, std::size_t max_output, std::string& out) {
  const HuffmanCode& table = Huffman();
  std::uint32_t code = 0;
  unsigned length = 0;
  std::size_t produced = 0;
  for (const std::byte byte : data) {
    for (int bit = 7; bit >= 0; --bit) {
      code = (code << 1) | ((static_cast<unsigned>(byte) >> bit) & 1u);
      ++length;
      if (length > kMaxCodeBits) {
        return false;
      }
      const std::uint16_t entries = table.count[length];
      if (entries == 0 || code < table.first_code[length] ||
          code - table.first_code[length] >= entries) {
        continue;
      }
      const std::uint16_t symbol =
          table.sorted[table.first_symbol[length] + (code - table.first_code[length])];
      // §5.2: an encoded EOS is not an end marker, it is a malformed string.
      // Treating it as a terminator is how two implementations end up
      // disagreeing about where a header value stopped.
      if (symbol == kEndOfString) {
        return false;
      }
      if (++produced > max_output) {
        return false;
      }
      out.push_back(static_cast<char>(symbol));
      code = 0;
      length = 0;
    }
  }
  // What is left must be a padding: fewer than eight bits, and all of them the
  // EOS prefix, which is all ones.
  if (length >= 8) {
    return false;
  }
  const std::uint32_t all_ones = (1u << length) - 1;
  return length == 0 || code == all_ones;
}

// --- Decoder ----------------------------------------------------------------

bool Decoder::Fail(const char* reason) {
  if (error_ == nullptr) {
    error_ = reason;
    AddPerformanceCounter(PerfCounterId::NetHpackFailures);
  }
  return false;
}

// RFC 7541 §5.1. The value is a prefix, and 2^n - 1 in the prefix means "the
// rest follows", seven bits at a time.
bool Decoder::ReadInteger(Cursor& cursor, unsigned prefix_bits, std::uint64_t& out) {
  if (cursor.Empty()) {
    return Fail("truncated integer");
  }
  const std::uint32_t mask = (1u << prefix_bits) - 1;
  std::uint64_t value = static_cast<unsigned>(cursor.data[cursor.at]) & mask;
  ++cursor.at;
  if (value < mask) {
    out = value;
    return true;
  }
  // Bounded at ten continuation bytes, which is more than a 64-bit value can
  // use. Without it a peer can spend a kilobyte saying "zero" and, on an
  // unbounded accumulator, roll it over into a small number — which is how a
  // length check gets passed by a value that is not small at all.
  unsigned shift = 0;
  for (int step = 0; step < 10; ++step) {
    if (cursor.Empty()) {
      return Fail("truncated integer");
    }
    const auto byte = static_cast<unsigned>(cursor.data[cursor.at]);
    ++cursor.at;
    const std::uint64_t chunk = byte & 0x7Fu;
    if (shift >= 63 || chunk > (UINT64_MAX >> shift) ||
        value > UINT64_MAX - (chunk << shift)) {
      return Fail("integer overflow");
    }
    value += chunk << shift;
    if ((byte & 0x80u) == 0) {
      out = value;
      return true;
    }
    shift += 7;
  }
  return Fail("integer too long");
}

bool Decoder::ReadString(Cursor& cursor, std::string& out) {
  if (cursor.Empty()) {
    return Fail("truncated string");
  }
  const bool huffman = (static_cast<unsigned>(cursor.data[cursor.at]) & 0x80u) != 0;
  std::uint64_t length = 0;
  if (!ReadInteger(cursor, 7, length)) {
    return false;
  }
  // Against what arrived, before anything is reserved. A declared length is a
  // number from the peer and nothing else.
  if (length > cursor.Remaining()) {
    return Fail("string runs past the block");
  }
  if (length > kMaxHeaderStringBytes) {
    return Fail("string exceeds its bound");
  }
  const auto span = cursor.data.subspan(cursor.at, static_cast<std::size_t>(length));
  cursor.at += static_cast<std::size_t>(length);
  out.clear();
  if (!huffman) {
    out.assign(reinterpret_cast<const char*>(span.data()), span.size());
    return true;
  }
  if (!HuffmanDecode(span, kMaxHeaderStringBytes, out)) {
    return Fail("malformed huffman string");
  }
  return true;
}

bool Decoder::Lookup(std::uint64_t index, Header& out) const {
  if (index == 0) {
    return false;
  }
  if (index <= kStaticEntries) {
    const StaticEntry& entry = kStaticTable[static_cast<std::size_t>(index)];
    out.name = entry.name;
    out.value = entry.value;
    return true;
  }
  const std::uint64_t dynamic = index - kStaticEntries - 1;
  if (dynamic >= table_.size()) {
    return false;
  }
  out = table_[static_cast<std::size_t>(dynamic)];
  return true;
}

void Decoder::SetCapacity(std::size_t capacity) {
  capacity_ = capacity;
  while (table_bytes_ > capacity_ && !table_.empty()) {
    table_bytes_ -= EntrySize(table_.back());
    table_.pop_back();
  }
}

void Decoder::Insert(Header field) {
  const std::size_t size = EntrySize(field);
  // §4.4: an entry larger than the whole table empties it and is not stored.
  // That is not an error — it is how a peer says "forget everything", and a
  // decoder that treated it as one would close connections real servers open.
  if (size > capacity_) {
    table_.clear();
    table_bytes_ = 0;
    return;
  }
  while (table_bytes_ + size > capacity_ && !table_.empty()) {
    table_bytes_ -= EntrySize(table_.back());
    table_.pop_back();
  }
  table_bytes_ += size;
  table_.push_front(std::move(field));
}

bool Decoder::Decode(std::span<const std::byte> block, std::vector<Header>& out) {
  if (Failed()) {
    return false;
  }
  AddPerformanceCounter(PerfCounterId::NetHpackBlockBytes, block.size());
  Cursor cursor{block, 0};
  std::size_t list_bytes = 0;
  for (const Header& already : out) {
    list_bytes += EntrySize(already);
  }

  while (!cursor.Empty()) {
    const auto lead = static_cast<unsigned>(cursor.data[cursor.at]);
    Header field;
    bool index_it = false;

    if ((lead & 0x80u) != 0) {
      // §6.1 indexed header field.
      std::uint64_t index = 0;
      if (!ReadInteger(cursor, 7, index)) {
        return false;
      }
      if (!Lookup(index, field)) {
        return Fail("index out of range");
      }
    } else if ((lead & 0xE0u) == 0x20u) {
      // §6.3 dynamic table size update. Bounded by what *we* advertised rather
      // than by what the peer asks for: this is the one message whose whole
      // content is "make the receiver hold more state".
      std::uint64_t capacity = 0;
      if (!ReadInteger(cursor, 5, capacity)) {
        return false;
      }
      if (capacity > kDynamicTableBytes) {
        return Fail("dynamic table size above the advertised maximum");
      }
      SetCapacity(static_cast<std::size_t>(capacity));
      continue;
    } else {
      // §6.2, in its three forms. They differ only in the prefix width and in
      // whether the field is inserted; the never-indexed form is treated
      // exactly like the not-indexed one on the way in, because its extra
      // meaning is an instruction to intermediaries and this browser is not one.
      const unsigned prefix = (lead & 0x40u) != 0 ? 6u : 4u;
      index_it = (lead & 0x40u) != 0;
      std::uint64_t index = 0;
      if (!ReadInteger(cursor, prefix, index)) {
        return false;
      }
      if (index == 0) {
        if (!ReadString(cursor, field.name)) {
          return false;
        }
      } else {
        Header named;
        if (!Lookup(index, named)) {
          return Fail("index out of range");
        }
        field.name = std::move(named.name);
      }
      if (!ReadString(cursor, field.value)) {
        return false;
      }
    }

    if (field.name.empty()) {
      return Fail("empty header name");
    }
    list_bytes += EntrySize(field);
    if (list_bytes > kMaxHeaderListBytes) {
      return Fail("header list exceeds its bound");
    }
    if (out.size() >= kMaxHeaderCount) {
      return Fail("too many header fields");
    }
    AddPerformanceCounter(PerfCounterId::NetHpackDecodedBytes,
                          field.name.size() + field.value.size());
    if (index_it) {
      Insert(field);
    }
    out.push_back(std::move(field));
  }
  return true;
}

// --- Encoder ----------------------------------------------------------------

namespace {

// §5.1, the write side. `pattern` is the bits above the prefix, already in
// place.
void WriteInteger(std::uint64_t value, unsigned prefix_bits, unsigned pattern, std::string& out) {
  const unsigned mask = (1u << prefix_bits) - 1;
  if (value < mask) {
    out.push_back(static_cast<char>(pattern | static_cast<unsigned>(value)));
    return;
  }
  out.push_back(static_cast<char>(pattern | mask));
  value -= mask;
  while (value >= 128) {
    out.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

void WriteString(std::string_view text, std::string& out) {
  // Per string rather than per connection: Huffman is a win on a path and a
  // loss on an already-compact token, and the flag exists to be decided each
  // time.
  if (HuffmanEncodedSize(text) < text.size()) {
    std::string coded;
    HuffmanEncode(text, coded);
    WriteInteger(coded.size(), 7, 0x80u, out);
    out += coded;
    return;
  }
  WriteInteger(text.size(), 7, 0x00u, out);
  out += text;
}

// The static table index for an exact (name, value) match, or for the name
// alone, or zero. One pass, because the two answers come from the same scan and
// asking twice is how they end up disagreeing.
void StaticIndex(const Header& field, std::size_t& exact, std::size_t& by_name) {
  exact = 0;
  by_name = 0;
  for (std::size_t i = 1; i <= kStaticEntries; ++i) {
    if (kStaticTable[i].name != field.name) {
      continue;
    }
    if (by_name == 0) {
      by_name = i;
    }
    if (kStaticTable[i].value == field.value) {
      exact = i;
      return;
    }
  }
}

}  // namespace

void Encode(std::span<const Header> headers, std::string& out) {
  for (const Header& field : headers) {
    std::size_t exact = 0;
    std::size_t by_name = 0;
    StaticIndex(field, exact, by_name);
    if (exact != 0 && !IsSensitiveHeader(field.name)) {
      WriteInteger(exact, 7, 0x80u, out);
      continue;
    }
    // Never "with incremental indexing". This encoder holds no dynamic table,
    // so a field it told the peer to insert would be one the peer could index
    // and this side could not — two ends disagreeing about the table, which is
    // the whole of HPACK's CVE history.
    //
    // Sensitive fields go out never-indexed, which is a stronger statement than
    // "I did not index this": it forbids every intermediary from indexing it
    // too. That is what keeps a session cookie out of a compression side
    // channel.
    const unsigned pattern = IsSensitiveHeader(field.name) ? 0x10u : 0x00u;
    WriteInteger(by_name, 4, pattern, out);
    if (by_name == 0) {
      WriteString(field.name, out);
    }
    WriteString(field.value, out);
  }
}

}  // namespace microbrowser::net::hpack

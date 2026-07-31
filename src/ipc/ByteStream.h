#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::ipc {

// Little-endian, length-prefixed, fixed-width primitive encoding.
//
// Two properties matter more than compactness here:
//
//   * The reader never trusts its input. Every read is bounds-checked and sets
//     a sticky failure flag rather than throwing or reading past the end. This
//     is a seam that will one day carry bytes from a sandboxed renderer, so it
//     is written to that standard now, when the cost is zero, rather than
//     retrofitted when it isn't.
//   * Decoding is total. A truncated or malformed frame yields nullopt, never a
//     half-populated message.
//
// Explicitly not a general serialization framework. It handles the primitives
// the message set actually uses; a message needing more grows this file with a
// reviewed method, not with an escape hatch.

class ByteWriter {
 public:
  void WriteU8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

  void WriteU32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      WriteU8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void WriteI32(std::int32_t value) { WriteU32(static_cast<std::uint32_t>(value)); }

  void WriteU64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      WriteU8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  // Bit-pattern transfer, not a decimal round-trip: both ends are the same
  // binary on the same machine, and a text form would lose the low bit of a
  // sub-pixel layout coordinate.
  void WriteF32(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(bits);
  }

  void WriteString(std::string_view text) {
    WriteU32(static_cast<std::uint32_t>(text.size()));
    for (const char c : text) {
      WriteU8(static_cast<std::uint8_t>(c));
    }
  }

  const std::vector<std::byte>& Bytes() const { return bytes_; }
  std::vector<std::byte> Take() { return std::move(bytes_); }
  std::size_t Size() const { return bytes_.size(); }

 private:
  std::vector<std::byte> bytes_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  // Sticky: once a read fails, every later read fails too, so a decoder can run
  // straight through and check Ok() once at the end instead of after each field.
  bool Ok() const { return ok_; }
  bool AtEnd() const { return position_ >= bytes_.size(); }
  std::size_t Remaining() const { return ok_ ? bytes_.size() - position_ : 0; }

  std::uint8_t ReadU8() {
    if (!ok_ || position_ + 1 > bytes_.size()) {
      ok_ = false;
      return 0;
    }
    return static_cast<std::uint8_t>(bytes_[position_++]);
  }

  std::uint32_t ReadU32() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(ReadU8()) << shift;
    }
    return ok_ ? value : 0;
  }

  std::int32_t ReadI32() { return static_cast<std::int32_t>(ReadU32()); }

  std::uint64_t ReadU64() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(ReadU8()) << shift;
    }
    return ok_ ? value : 0;
  }

  float ReadF32() {
    const std::uint32_t bits = ReadU32();
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return ok_ ? value : 0.0f;
  }

  // The length prefix is checked against what is actually left before any
  // allocation happens. A hostile frame claiming a 4 GiB string must not cause
  // a 4 GiB reserve.
  std::string ReadString() {
    const std::uint32_t length = ReadU32();
    if (!ok_ || length > Remaining()) {
      ok_ = false;
      return {};
    }
    std::string text;
    text.resize(length);
    for (std::uint32_t i = 0; i < length; ++i) {
      text[i] = static_cast<char>(ReadU8());
    }
    return ok_ ? text : std::string{};
  }

  // Same guard for repeated fields: a count is only honored once it is possible
  // for that many elements to exist in the bytes that remain.
  std::optional<std::uint32_t> ReadCount(std::size_t min_bytes_per_element) {
    const std::uint32_t count = ReadU32();
    if (!ok_) {
      return std::nullopt;
    }
    if (min_bytes_per_element > 0 &&
        static_cast<std::uint64_t>(count) * min_bytes_per_element > Remaining()) {
      ok_ = false;
      return std::nullopt;
    }
    return count;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  bool ok_ = true;
};

}  // namespace microbrowser::ipc

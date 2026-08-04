#include "media/BoxReader.h"

#include <utility>

namespace microbrowser::media {

namespace {

// The fixed part of every box: a 32-bit size and a four-character type.
constexpr std::uint64_t kBoxHeaderSize = 8;
// The `largesize` form adds a 64-bit size after the type.
constexpr std::uint64_t kLargeBoxHeaderSize = 16;
// A `uuid` box adds a 16-byte extended type after that.
constexpr std::uint64_t kExtendedTypeSize = 16;

}  // namespace

bool BoxReader::Want(std::size_t count) {
  if (!ok_) {
    return false;
  }
  // Subtraction rather than `offset_ + count > size()`: the addition is the
  // overflow, and the subtraction cannot be one because offset_ is an invariant
  // of this class rather than a number from the file.
  if (count > bytes_.size() - offset_) {
    ok_ = false;
    return false;
  }
  return true;
}

std::uint8_t BoxReader::ReadU8() {
  if (!Want(1)) {
    return 0;
  }
  return static_cast<std::uint8_t>(bytes_[offset_++]);
}

std::uint16_t BoxReader::ReadU16() {
  if (!Want(2)) {
    return 0;
  }
  const auto high = static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes_[offset_]));
  const auto low = static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes_[offset_ + 1]));
  offset_ += 2;
  return static_cast<std::uint16_t>((high << 8) | low);
}

std::uint32_t BoxReader::ReadU24() {
  if (!Want(3)) {
    return 0;
  }
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 3; ++i) {
    value = (value << 8) | static_cast<std::uint8_t>(bytes_[offset_ + i]);
  }
  offset_ += 3;
  return value;
}

std::uint32_t BoxReader::ReadU32() {
  if (!Want(4)) {
    return 0;
  }
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8) | static_cast<std::uint8_t>(bytes_[offset_ + i]);
  }
  offset_ += 4;
  return value;
}

std::uint64_t BoxReader::ReadU64() {
  if (!Want(8)) {
    return 0;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint8_t>(bytes_[offset_ + i]);
  }
  offset_ += 8;
  return value;
}

std::string BoxReader::ReadFourCC() {
  if (!Want(4)) {
    return {};
  }
  std::string type(4, '\0');
  for (std::size_t i = 0; i < 4; ++i) {
    type[i] = static_cast<char>(static_cast<std::uint8_t>(bytes_[offset_ + i]));
  }
  offset_ += 4;
  return type;
}

bool BoxReader::Skip(std::uint64_t count) {
  if (!ok_) {
    return false;
  }
  // Compared in 64 bits against the remaining bytes before it is narrowed. The
  // count arrives from a box size, so it is an attacker's number all the way
  // up to 2^64 - 1 and must never reach the size_t addition.
  if (count > static_cast<std::uint64_t>(bytes_.size() - offset_)) {
    ok_ = false;
    return false;
  }
  offset_ += static_cast<std::size_t>(count);
  return true;
}

BoxReader BoxReader::Sub(std::uint64_t count) {
  if (!ok_ || count > static_cast<std::uint64_t>(bytes_.size() - offset_)) {
    ok_ = false;
    BoxReader failed(std::span<const std::byte>{});
    failed.Fail();
    return failed;
  }
  const auto size = static_cast<std::size_t>(count);
  BoxReader child(bytes_.subspan(offset_, size));
  offset_ += size;
  return child;
}

std::span<const std::byte> BoxReader::Rest() const {
  if (!ok_) {
    return {};
  }
  return bytes_.subspan(offset_);
}

BoxHeader ReadBoxHeader(BoxReader& reader) {
  BoxHeader header;
  if (reader.Remaining() < kBoxHeaderSize) {
    // Not a failure of the reader: running out of boxes is how a well-formed
    // walk ends. The caller distinguishes the two by asking reader.Ok().
    return header;
  }

  const std::uint64_t declared = reader.ReadU32();
  header.type = reader.ReadFourCC();
  std::uint64_t header_size = kBoxHeaderSize;

  std::uint64_t total = declared;
  if (declared == 1) {
    // The `largesize` escape.
    total = reader.ReadU64();
    header_size = kLargeBoxHeaderSize;
  } else if (declared == 0) {
    // "To the end of the enclosing box", which for this reader is the end of
    // its span. Computed rather than trusted, so it is always exactly right.
    total = kBoxHeaderSize + static_cast<std::uint64_t>(reader.Remaining());
  }

  if (header.type == "uuid") {
    std::string extended(kExtendedTypeSize, '\0');
    for (std::size_t i = 0; i < kExtendedTypeSize; ++i) {
      extended[i] = static_cast<char>(reader.ReadU8());
    }
    header.extended_type = std::move(extended);
    header_size += kExtendedTypeSize;
  }

  if (!reader.Ok()) {
    return header;
  }
  // **The non-advancing box.** A size smaller than the header it just claimed
  // would leave the walk exactly where it started, and a `while (more boxes)`
  // loop over it never terminates. This is the single most common way a box
  // walker hangs on a malformed file, and it costs one comparison to remove.
  if (total < header_size) {
    reader.Fail();
    return header;
  }
  header.payload_size = total - header_size;
  if (header.payload_size > static_cast<std::uint64_t>(reader.Remaining())) {
    // The declared size runs past the bytes there are. Refused rather than
    // clamped: a truncated file and a lying one look identical here, and
    // parsing the clamp would mean reading fields out of the next box.
    reader.Fail();
    return header;
  }
  header.valid = true;
  return header;
}

}  // namespace microbrowser::media

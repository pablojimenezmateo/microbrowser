#include "media/Ump.h"

#include <cstdint>

#include "util/SaturatingMath.h"

namespace microbrowser::media {

namespace {

constexpr std::uint64_t kMediaPart = 21;
constexpr std::size_t kMaxParts = 256;
constexpr std::size_t kMaxExtractedBytes = 64u * 1024u * 1024u;

// UMP varints: size encoded in the leading bits of the first byte, value in the rest (5-byte form
// ignores those bits and reads a little-endian u32). Same shape as RFC 8794 with YouTube's 5-byte
// deviation -- see docs linked from ADR 0028's YouTube notes.
bool ReadVarInt(std::span<const std::byte> input, std::size_t& at, std::uint64_t& value) {
  if (at >= input.size()) {
    return false;
  }
  const std::uint8_t prefix = static_cast<std::uint8_t>(input[at]);
  int size = 0;
  for (int shift = 1; shift <= 5; ++shift) {
    if ((prefix & static_cast<std::uint8_t>(128 >> (shift - 1))) == 0) {
      size = shift;
      break;
    }
  }
  if (size < 1 || size > 5) {
    return false;
  }
  if (input.size() - at < static_cast<std::size_t>(size)) {
    return false;
  }
  ++at;
  switch (size) {
    case 1:
      value = prefix;
      return true;
    case 2:
      value = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(input[at])) << 6) |
              (prefix & 0x3Fu);
      ++at;
      return true;
    case 3: {
      const std::uint64_t lo = static_cast<std::uint8_t>(input[at]);
      const std::uint64_t hi = static_cast<std::uint8_t>(input[at + 1]);
      at += 2;
      value = (lo | (hi << 8)) | (prefix & 0x1Fu);
      return true;
    }
    case 4: {
      const std::uint64_t b0 = static_cast<std::uint8_t>(input[at]);
      const std::uint64_t b1 = static_cast<std::uint8_t>(input[at + 1]);
      const std::uint64_t b2 = static_cast<std::uint8_t>(input[at + 2]);
      at += 3;
      value = (b0 | (b1 << 8) | (b2 << 16)) | (prefix & 0x0Fu);
      return true;
    }
    default: {
      // Five-byte form: ignore the lower bits of the prefix; next four bytes are LE u32.
      const std::uint64_t b0 = static_cast<std::uint8_t>(input[at]);
      const std::uint64_t b1 = static_cast<std::uint8_t>(input[at + 1]);
      const std::uint64_t b2 = static_cast<std::uint8_t>(input[at + 2]);
      const std::uint64_t b3 = static_cast<std::uint8_t>(input[at + 3]);
      at += 4;
      value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
      return true;
    }
  }
}

}  // namespace

std::optional<std::vector<std::byte>> ExtractUmpMedia(std::span<const std::byte> input) {
  if (input.empty()) {
    return std::nullopt;
  }
  // Cheap reject: EBML and ISO-BMFF are not UMP. Avoid walking every MP4 as varints.
  if (input.size() >= 4) {
    const auto b0 = static_cast<std::uint8_t>(input[0]);
    const auto b1 = static_cast<std::uint8_t>(input[1]);
    const auto b2 = static_cast<std::uint8_t>(input[2]);
    const auto b3 = static_cast<std::uint8_t>(input[3]);
    // EBML header magic 1A 45 DF A3, or a bare Cluster (MSE WebM media segment).
    if ((b0 == 0x1A && b1 == 0x45 && b2 == 0xDF && b3 == 0xA3) ||
        (b0 == 0x1F && b1 == 0x43 && b2 == 0xB6 && b3 == 0x75)) {
      return std::nullopt;
    }
    // ISO-BMFF box: size then 'ftyp'/'moof'/'styp'/'sidx'.
    if (input.size() >= 8) {
      const char t0 = static_cast<char>(input[4]);
      const char t1 = static_cast<char>(input[5]);
      const char t2 = static_cast<char>(input[6]);
      const char t3 = static_cast<char>(input[7]);
      if ((t0 == 'f' && t1 == 't' && t2 == 'y' && t3 == 'p') ||
          (t0 == 'm' && t1 == 'o' && t2 == 'o' && t3 == 'f') ||
          (t0 == 's' && t1 == 't' && t2 == 'y' && t3 == 'p') ||
          (t0 == 's' && t1 == 'i' && t2 == 'd' && t3 == 'x')) {
        return std::nullopt;
      }
    }
  }

  std::vector<std::byte> out;
  std::size_t at = 0;
  std::size_t parts = 0;
  bool saw_media = false;
  while (at < input.size() && parts < kMaxParts) {
    std::uint64_t type = 0;
    std::uint64_t size = 0;
    const std::size_t start = at;
    if (!ReadVarInt(input, at, type) || !ReadVarInt(input, at, size)) {
      // Truncated header: keep what we extracted if any MEDIA arrived; else refuse.
      break;
    }
    if (size > input.size() - at) {
      // Partial trailing part -- stop without inventing bytes.
      at = start;
      break;
    }
    const std::span<const std::byte> payload = input.subspan(at, static_cast<std::size_t>(size));
    at = util::SaturatingAdd(at, static_cast<std::size_t>(size));
    ++parts;
    if (type != kMediaPart) {
      continue;
    }
    // MEDIA: one-byte header id, then media.
    if (payload.size() < 2) {
      continue;
    }
    const std::span<const std::byte> media = payload.subspan(1);
    if (out.size() > kMaxExtractedBytes || media.size() > kMaxExtractedBytes - out.size()) {
      return std::nullopt;
    }
    out.insert(out.end(), media.begin(), media.end());
    saw_media = true;
  }
  if (!saw_media || out.empty()) {
    return std::nullopt;
  }
  return out;
}

}  // namespace microbrowser::media

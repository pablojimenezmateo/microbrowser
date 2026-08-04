#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace microbrowser::media {

// A bounds-checked cursor over ISO base media file bytes.
//
// Every byte this reads was chosen by whoever served the file, so the shape
// here is the same one net/HttpMessage.h and gfx/PngDecoder.h use: a reader
// that cannot be made to read past its end, and that **fails stickily**. Once
// `Ok()` is false it stays false and every subsequent read returns zero. That
// is not tidiness -- a parser that keeps going after a short read is a parser
// whose next field is whatever happened to be in memory, and the whole class of
// "the length said 40 but there were 4 bytes left" bugs lives in the gap
// between a failed read and the code that did not check it.
//
// Sizes are the other half. An ISO-BMFF box declares its own length, in the
// file, as a number the attacker wrote. Every one of them is widened to 64 bits
// before it is compared and never trusted as an offset.
class BoxReader {
 public:
  explicit BoxReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool Ok() const { return ok_; }
  std::size_t Offset() const { return offset_; }
  std::size_t Remaining() const { return ok_ ? bytes_.size() - offset_ : 0; }
  bool AtEnd() const { return Remaining() == 0; }

  // Big-endian, which is what every field in this container format is.
  std::uint8_t ReadU8();
  std::uint16_t ReadU16();
  std::uint32_t ReadU24();
  std::uint32_t ReadU32();
  std::uint64_t ReadU64();
  std::int32_t ReadI32() { return static_cast<std::int32_t>(ReadU32()); }

  // A four-character type code, as four bytes. Returned as a string rather than
  // a uint32 because every use of it is a comparison against a literal and
  // `type == "moov"` is the version of that a reader can check.
  //
  // Non-printable bytes are kept rather than sanitized: the value is compared
  // against known codes and never logged or shown, so mangling it would only
  // make two different unknown boxes look like the same one.
  std::string ReadFourCC();

  // Advances without reading. Fails rather than clamping: a skip past the end
  // means the size that produced it was wrong, and continuing from the end of
  // the buffer would parse the next box out of nothing.
  bool Skip(std::uint64_t count);

  // A sub-reader over the next `count` bytes, and advances past them. The
  // returned reader is already failed if there are not that many, so a caller
  // that forgets to check gets an empty parse rather than the rest of the file.
  BoxReader Sub(std::uint64_t count);

  // Everything not yet read, without advancing.
  std::span<const std::byte> Rest() const;

  // Marks this reader failed. For a caller that has decided the bytes are
  // wrong for a reason the reader itself cannot see -- a version it does not
  // implement, a count that disagrees with a length.
  void Fail() { ok_ = false; }

 private:
  bool Want(std::size_t count);

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
  bool ok_ = true;
};

// One box header: its type, and where its payload is.
//
// `payload_size` is already resolved against the three ways ISO-BMFF spells a
// size -- a 32-bit size, the 64-bit `largesize` escape, and size 0 meaning "to
// the end of the enclosing box" -- and already checked against the bytes that
// actually remain. A caller therefore never does size arithmetic itself, which
// is where this format's overflows live.
struct BoxHeader {
  std::string type;
  std::uint64_t payload_size = 0;
  // The `uuid` escape: a 16-byte extended type. Empty unless `type` is "uuid".
  // Kept because skipping a uuid box requires knowing it had one, and because
  // an encrypted-media box is a uuid box.
  std::string extended_type;
  bool valid = false;
};

// Reads one box header from `reader` and leaves it positioned at the payload.
// Returns a header with `valid` false at the end of the input, on a size the
// bytes cannot support, or on a size smaller than the header it just read --
// that last one is the classic infinite loop in a box walker, because a box of
// size 0 that is not the "to the end" escape advances nothing.
BoxHeader ReadBoxHeader(BoxReader& reader);

}  // namespace microbrowser::media

#include "ipc/DecoderMessage.h"

#include <limits>

#include "ipc/ByteStream.h"

namespace microbrowser::ipc {

namespace {

constexpr std::size_t kLengthPrefixBytes = 4;
constexpr std::size_t kMaxErrorReasonBytes = 64;

void WriteI64(ByteWriter& writer, std::int64_t value) {
  writer.WriteU64(static_cast<std::uint64_t>(value));
}

std::int64_t ReadI64(ByteReader& reader) {
  return static_cast<std::int64_t>(reader.ReadU64());
}

void WriteBlob(ByteWriter& writer, std::span<const std::uint8_t> bytes) {
  writer.WriteU32(static_cast<std::uint32_t>(bytes.size()));
  for (const std::uint8_t byte : bytes) {
    writer.WriteU8(byte);
  }
}

bool ReadBlob(ByteReader& reader, std::vector<std::uint8_t>& out, std::size_t max_bytes) {
  const std::uint32_t length = reader.ReadU32();
  if (!reader.Ok() || static_cast<std::size_t>(length) > max_bytes ||
      static_cast<std::size_t>(length) > reader.Remaining()) {
    return false;
  }
  out.resize(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    out[i] = reader.ReadU8();
  }
  return reader.Ok();
}

std::vector<std::uint8_t> FinishMessage(ByteWriter& writer) {
  const std::size_t body_size = writer.Size();
  ByteWriter framed;
  framed.WriteU32(static_cast<std::uint32_t>(body_size));
  for (const std::byte byte : writer.Bytes()) {
    framed.WriteU8(static_cast<std::uint8_t>(byte));
  }
  std::vector<std::uint8_t> out;
  out.reserve(framed.Size());
  for (const std::byte byte : framed.Bytes()) {
    out.push_back(static_cast<std::uint8_t>(byte));
  }
  return out;
}

ByteWriter BeginBody(DecoderMessageKind kind) {
  ByteWriter writer;
  writer.WriteU32(kDecoderProtocolVersion);
  writer.WriteU8(static_cast<std::uint8_t>(kind));
  return writer;
}

bool ReadBodyHeader(ByteReader& reader, DecoderMessageKind& kind) {
  if (reader.ReadU32() != kDecoderProtocolVersion) {
    return false;
  }
  const std::uint8_t kind_byte = reader.ReadU8();
  if (!reader.Ok()) {
    return false;
  }
  switch (static_cast<DecoderMessageKind>(kind_byte)) {
    case DecoderMessageKind::Configure:
    case DecoderMessageKind::Sample:
    case DecoderMessageKind::Frame:
    case DecoderMessageKind::Error:
    case DecoderMessageKind::Flush:
      kind = static_cast<DecoderMessageKind>(kind_byte);
      return reader.Ok();
    default:
      return false;
  }
}

bool FrameDimensionsAreValid(std::uint32_t width, std::uint32_t height, std::size_t byte_count) {
  if (width == 0 && height == 0) {
    return true;
  }
  if (width == 0 || height == 0) {
    return false;
  }
  if (width > kMaxFrameDimension || height > kMaxFrameDimension) {
    return false;
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (pixels > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) / 4u) {
    return false;
  }
  const std::uint64_t max_bytes = pixels * 4u;
  if (byte_count > max_bytes) {
    return false;
  }
  // Dimensions that imply more pixels than the blob carries are refused before allocation.
  return max_bytes <= byte_count;
}

bool DecodeConfigurePayload(ByteReader& reader, ConfigureMessage& message) {
  const std::uint8_t codec = reader.ReadU8();
  if (!reader.Ok() || !IsKnownWireCodec(codec)) {
    return false;
  }
  message.codec = static_cast<WireCodec>(codec);
  return ReadBlob(reader, message.extra_data, kMaxDecoderMessageBytes) && reader.AtEnd();
}

bool DecodeSamplePayload(ByteReader& reader, SampleMessage& message) {
  message.timestamp_us = ReadI64(reader);
  message.is_sync = reader.ReadU8() != 0;
  if (!reader.Ok()) {
    return false;
  }
  return ReadBlob(reader, message.bytes, kMaxDecoderMessageBytes) && reader.AtEnd();
}

bool DecodeFramePayload(ByteReader& reader, FrameMessage& message) {
  message.timestamp_us = ReadI64(reader);
  message.width = reader.ReadU32();
  message.height = reader.ReadU32();
  message.sample_count = reader.ReadU32();
  message.channels = reader.ReadU8();
  if (!reader.Ok()) {
    return false;
  }
  if (!ReadBlob(reader, message.bytes, kMaxDecoderMessageBytes)) {
    return false;
  }
  if (!FrameDimensionsAreValid(message.width, message.height, message.bytes.size())) {
    return false;
  }
  return reader.AtEnd();
}

bool DecodeErrorPayload(ByteReader& reader, ErrorMessage& message) {
  const std::uint32_t length = reader.ReadU32();
  if (!reader.Ok() || length > kMaxErrorReasonBytes || length > reader.Remaining()) {
    return false;
  }
  message.reason.resize(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    message.reason[i] = static_cast<char>(reader.ReadU8());
  }
  return reader.Ok() && reader.AtEnd();
}

DecoderDecodeResult MakeResult(DecoderDecode status, std::size_t consumed = 0) {
  DecoderDecodeResult result;
  result.status = status;
  result.consumed = consumed;
  return result;
}

DecoderDecodeResult DecodeBody(std::span<const std::uint8_t> body) {
  const std::span<const std::byte> body_bytes(reinterpret_cast<const std::byte*>(body.data()),
                                              body.size());
  ByteReader reader(body_bytes);
  DecoderMessageKind kind = DecoderMessageKind::Error;
  if (!ReadBodyHeader(reader, kind)) {
    return MakeResult(DecoderDecode::Failed);
  }

  DecoderDecodeResult result;
  result.status = DecoderDecode::Ok;
  result.message.kind = kind;
  result.consumed = body.size();

  switch (kind) {
    case DecoderMessageKind::Configure:
      if (!DecodeConfigurePayload(reader, result.message.configure)) {
        return MakeResult(DecoderDecode::Failed);
      }
      break;
    case DecoderMessageKind::Sample:
      if (!DecodeSamplePayload(reader, result.message.sample)) {
        return MakeResult(DecoderDecode::Failed);
      }
      break;
    case DecoderMessageKind::Frame:
      if (!DecodeFramePayload(reader, result.message.frame)) {
        return MakeResult(DecoderDecode::Failed);
      }
      break;
    case DecoderMessageKind::Error:
      if (!DecodeErrorPayload(reader, result.message.error)) {
        return MakeResult(DecoderDecode::Failed);
      }
      break;
    case DecoderMessageKind::Flush:
      if (!reader.AtEnd()) {
        return MakeResult(DecoderDecode::Failed);
      }
      break;
  }

  return result;
}

}  // namespace

bool IsKnownWireCodec(std::uint8_t value) {
  switch (static_cast<WireCodec>(value)) {
    case WireCodec::H264:
    case WireCodec::Vp9:
    case WireCodec::Av1:
    case WireCodec::Aac:
    case WireCodec::Opus:
      return true;
  }
  return false;
}

std::vector<std::uint8_t> EncodeConfigure(const ConfigureMessage& message) {
  ByteWriter writer = BeginBody(DecoderMessageKind::Configure);
  writer.WriteU8(static_cast<std::uint8_t>(message.codec));
  WriteBlob(writer, message.extra_data);
  return FinishMessage(writer);
}

std::vector<std::uint8_t> EncodeSample(const SampleMessage& message) {
  ByteWriter writer = BeginBody(DecoderMessageKind::Sample);
  WriteI64(writer, message.timestamp_us);
  writer.WriteU8(message.is_sync ? 1u : 0u);
  WriteBlob(writer, message.bytes);
  return FinishMessage(writer);
}

std::vector<std::uint8_t> EncodeFrame(const FrameMessage& message) {
  ByteWriter writer = BeginBody(DecoderMessageKind::Frame);
  WriteI64(writer, message.timestamp_us);
  writer.WriteU32(message.width);
  writer.WriteU32(message.height);
  writer.WriteU32(message.sample_count);
  writer.WriteU8(message.channels);
  WriteBlob(writer, message.bytes);
  return FinishMessage(writer);
}

std::vector<std::uint8_t> EncodeError(const ErrorMessage& message) {
  ByteWriter writer = BeginBody(DecoderMessageKind::Error);
  const std::string_view reason = message.reason;
  writer.WriteU32(static_cast<std::uint32_t>(reason.size()));
  for (const char c : reason) {
    writer.WriteU8(static_cast<std::uint8_t>(c));
  }
  return FinishMessage(writer);
}

std::vector<std::uint8_t> EncodeFlush() {
  ByteWriter writer = BeginBody(DecoderMessageKind::Flush);
  return FinishMessage(writer);
}

DecoderDecodeResult DecodeDecoderMessage(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kLengthPrefixBytes) {
    return MakeResult(DecoderDecode::Incomplete);
  }

  const std::span<const std::byte> prefix_bytes(reinterpret_cast<const std::byte*>(bytes.data()),
                                                kLengthPrefixBytes);
  ByteReader prefix_reader(prefix_bytes);
  const std::uint32_t body_length = prefix_reader.ReadU32();
  if (!prefix_reader.Ok()) {
    return MakeResult(DecoderDecode::Failed);
  }
  if (static_cast<std::size_t>(body_length) > kMaxDecoderMessageBytes) {
    // A length larger than we will ever accept is malformed, not a reason to buffer forever.
    return MakeResult(DecoderDecode::Failed);
  }

  const std::size_t frame_size = kLengthPrefixBytes + static_cast<std::size_t>(body_length);
  if (bytes.size() < frame_size) {
    return MakeResult(DecoderDecode::Incomplete);
  }

  const std::span<const std::uint8_t> body = bytes.subspan(kLengthPrefixBytes, body_length);
  DecoderDecodeResult result = DecodeBody(body);
  if (result.status == DecoderDecode::Ok) {
    result.consumed = frame_size;
  } else {
    result.consumed = 0;
  }
  return result;
}

}  // namespace microbrowser::ipc

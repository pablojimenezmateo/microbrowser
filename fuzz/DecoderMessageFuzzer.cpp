#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ipc/DecoderMessage.h"

// Decoder replies are the dangerous direction: a compromised child may send anything.
// This target feeds arbitrary bytes to the reply reader and checks that a decoded
// Frame or Error re-encodes to the same value.
namespace {

bool FrameEquivalent(const microbrowser::ipc::FrameMessage& left,
                     const microbrowser::ipc::FrameMessage& right) {
  return left.timestamp_us == right.timestamp_us && left.width == right.width &&
         left.height == right.height && left.sample_count == right.sample_count &&
         left.channels == right.channels && left.bytes == right.bytes;
}

bool ErrorEquivalent(const microbrowser::ipc::ErrorMessage& left,
                     const microbrowser::ipc::ErrorMessage& right) {
  return left.reason == right.reason;
}

void RoundTripReply(const microbrowser::ipc::DecoderMessage& message) {
  std::vector<std::uint8_t> encoded;
  switch (message.kind) {
    case microbrowser::ipc::DecoderMessageKind::Frame:
      encoded = microbrowser::ipc::EncodeFrame(message.frame);
      break;
    case microbrowser::ipc::DecoderMessageKind::Error:
      encoded = microbrowser::ipc::EncodeError(message.error);
      break;
    default:
      return;
  }

  const microbrowser::ipc::DecoderDecodeResult again =
      microbrowser::ipc::DecodeDecoderMessage(encoded);
  if (again.status != microbrowser::ipc::DecoderDecode::Ok) {
    __builtin_trap();
  }
  if (message.kind == microbrowser::ipc::DecoderMessageKind::Frame &&
      !FrameEquivalent(message.frame, again.message.frame)) {
    __builtin_trap();
  }
  if (message.kind == microbrowser::ipc::DecoderMessageKind::Error &&
      !ErrorEquivalent(message.error, again.message.error)) {
    __builtin_trap();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> input(data, size);
  const microbrowser::ipc::DecoderDecodeResult decoded =
      microbrowser::ipc::DecodeDecoderMessage(input);
  if (decoded.status == microbrowser::ipc::DecoderDecode::Ok) {
    if (decoded.message.kind == microbrowser::ipc::DecoderMessageKind::Frame ||
        decoded.message.kind == microbrowser::ipc::DecoderMessageKind::Error) {
      RoundTripReply(decoded.message);
    }
  }
  return 0;
}

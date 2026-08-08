// The sandboxed media decoder process. ADR 0031 §3-4, session 27.
//
// Reads framed `ipc::DecoderMessage` records from stdin and writes Frame/Error replies to stdout.
// Configure then Sample, with optional Flush. One process owns one codec instance.
//
// Smoke test (protocol round-trip without real codec bytes):
//   ./build/microbrowser/microbrowser_decoder <<'EOF' | xxd
//   ./build/microbrowser/microbrowser_tests DecoderMessage
//
// The browser and microbrowser_snapshot resolve the decoder as a sibling of the
// running executable (e.g. build/microbrowser/microbrowser_decoder next to
// microbrowser_snapshot), or from MICROBROWSER_DECODER when set.
//
// Manual pipe with python (flush-only after configure):
//   python3 - <<'PY' | ./build/microbrowser/microbrowser_decoder | wc -c
//   import struct
//   def frame(body):
//       return struct.pack('<I', len(body)) + body
//   body = struct.pack('<I', 1) + bytes([1, 5]) + struct.pack('<I', 0)  # Configure Opus, no extra
//   import sys
//   sys.stdout.buffer.write(frame(body))
//   sys.stdout.buffer.write(frame(struct.pack('<I', 1) + bytes([5])))  # Flush
//   PY

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ipc/DecoderMessage.h"
#include "platform/Sandbox.h"
#include "DecoderBackend.h"

namespace {

bool WriteStdout(std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(1, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

void EmitError(const std::string& reason) {
  const std::vector<std::uint8_t> encoded = microbrowser::ipc::EncodeError({reason});
  if (!WriteStdout(encoded)) {
    std::cerr << "microbrowser_decoder: stdout write failed\n";
    std::exit(1);
  }
}

class DecoderProcess {
 public:
  explicit DecoderProcess(microbrowser::decoder_tool::FrameEmitter emit) : emit_(std::move(emit)) {}

  void Handle(const microbrowser::ipc::DecoderMessage& message) {
    switch (message.kind) {
      case microbrowser::ipc::DecoderMessageKind::Configure:
        backend_ = microbrowser::decoder_tool::CreateBackend(message.configure.codec, emit_);
        if (backend_ == nullptr) {
          EmitError("codec");
          backend_.reset();
          return;
        }
        {
          std::string error;
          if (!backend_->Configure(message.configure.extra_data, error)) {
            EmitError(error);
            backend_.reset();
          }
        }
        break;
      case microbrowser::ipc::DecoderMessageKind::Sample:
        if (backend_ == nullptr) {
          EmitError("not_configured");
          return;
        }
        {
          std::string error;
          if (!backend_->DecodeSample(message.sample, error)) {
            EmitError(error);
            backend_.reset();
          }
        }
        break;
      case microbrowser::ipc::DecoderMessageKind::Flush:
        if (backend_ == nullptr) {
          EmitError("not_configured");
          return;
        }
        {
          std::string error;
          if (!backend_->Flush(error)) {
            EmitError(error);
            backend_.reset();
          }
        }
        break;
      case microbrowser::ipc::DecoderMessageKind::Frame:
      case microbrowser::ipc::DecoderMessageKind::Error:
        EmitError("unexpected");
        break;
    }
  }

 private:
  microbrowser::decoder_tool::FrameEmitter emit_;
  std::unique_ptr<microbrowser::decoder_tool::DecoderBackend> backend_;
};

}  // namespace

int main() {
  if (microbrowser::platform::SandboxAvailable()) {
    if (!microbrowser::platform::ApplySandbox(microbrowser::platform::SandboxPolicy::MediaDecoder)) {
      std::cerr << "microbrowser_decoder: sandbox refused\n";
      return 1;
    }
  }

  const auto emit_frame = [](const microbrowser::ipc::FrameMessage& frame) {
    const std::vector<std::uint8_t> encoded = microbrowser::ipc::EncodeFrame(frame);
    if (!WriteStdout(encoded)) {
      std::cerr << "microbrowser_decoder: stdout write failed\n";
      std::exit(1);
    }
  };

  DecoderProcess process(emit_frame);
  std::vector<std::uint8_t> buffer;
  std::array<std::uint8_t, 4096> chunk{};

  while (true) {
    const ssize_t read_count =
        ::read(0, chunk.data(), chunk.size());
    if (read_count == 0) {
      break;
    }
    if (read_count < 0) {
      EmitError("stdin");
      return 1;
    }
    buffer.insert(buffer.end(), chunk.begin(),
                  chunk.begin() + read_count);

    std::size_t offset = 0;
    while (offset < buffer.size()) {
      const microbrowser::ipc::DecoderDecodeResult decoded =
          microbrowser::ipc::DecodeDecoderMessage(
              std::span<const std::uint8_t>(buffer.data() + offset, buffer.size() - offset));
      if (decoded.status == microbrowser::ipc::DecoderDecode::Incomplete) {
        break;
      }
      if (decoded.status == microbrowser::ipc::DecoderDecode::Failed) {
        EmitError("protocol");
        return 1;
      }
      process.Handle(decoded.message);
      offset += decoded.consumed;
    }
    if (offset > 0) {
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    }
  }

  return 0;
}

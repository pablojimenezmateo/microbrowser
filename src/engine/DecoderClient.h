#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ipc/DecoderMessage.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::engine {

// One out-of-process decoder instance. ADR 0031 §3-4.
//
// Spawns `microbrowser_decoder` and speaks framed `ipc::DecoderMessage` records on pipes. The child
// applies its own sandbox; this side never links a codec library. One client owns one stream -- a
// video track and an audio track are two clients, not one with a mode flag.
class DecoderClient {
 public:
  DecoderClient();
  ~DecoderClient();

  DecoderClient(const DecoderClient&) = delete;
  DecoderClient& operator=(const DecoderClient&) = delete;

  // Starts the child if it is not running. False when the binary cannot be found or `exec` fails.
  bool Start();

  bool IsRunning() const { return child_pid_ > 0; }

  // Sends a configure message. The child must be running and not yet configured.
  bool Configure(ipc::WireCodec codec, std::span<const std::uint8_t> extra_data);

  bool PushSample(std::int64_t timestamp_us, bool is_sync, std::span<const std::uint8_t> bytes);
  bool Flush();

  // Reads every complete reply currently available. Malformed output or an Error message stops the
  // child and appends a reason to `error_out` when provided.
  std::vector<ipc::FrameMessage> PollFrames(std::string* error_out = nullptr);

  // True when the child exited on its own.
  bool ChildExited(std::string* reason_out = nullptr);

  std::optional<util::WaitDescriptor> Interest() const;

  static std::string FindDecoderBinary();

 private:
  bool WriteMessage(std::span<const std::uint8_t> bytes);
  bool ReadMore();
  void StopChild(const char* reason);

  std::string binary_path_;
  int to_child_ = -1;
  int from_child_ = -1;
  pid_t child_pid_ = -1;
  std::vector<std::uint8_t> read_buffer_;
  bool configured_ = false;
};

}  // namespace microbrowser::engine

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ipc/DecoderMessage.h"

namespace microbrowser::decoder_tool {

using FrameEmitter = std::function<void(ipc::FrameMessage)>;

class DecoderBackend {
 public:
  virtual ~DecoderBackend() = default;

  virtual bool Configure(std::span<const std::uint8_t> extra_data, std::string& error) = 0;
  virtual bool DecodeSample(const ipc::SampleMessage& sample, std::string& error) = 0;
  virtual bool Flush(std::string& error) = 0;
};

std::unique_ptr<DecoderBackend> CreateBackend(ipc::WireCodec codec, FrameEmitter emit);

}  // namespace microbrowser::decoder_tool

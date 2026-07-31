#include "ipc/InProcessTransport.h"

#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::ipc {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

void InProcessChannel::UiSide::Send(const UiToEngine& message) {
  AddPerformanceCounter(PerfCounterId::IpcMessagesSent);
  channel_.to_engine_.push_back(message);
}

std::optional<EngineToUi> InProcessChannel::UiSide::TryReceive() {
  if (channel_.to_ui_.empty()) {
    return std::nullopt;
  }
  AddPerformanceCounter(PerfCounterId::IpcMessagesReceived);
  EngineToUi message = std::move(channel_.to_ui_.front());
  channel_.to_ui_.pop_front();
  return message;
}

void InProcessChannel::EngineSide::Send(const EngineToUi& message) {
  AddPerformanceCounter(PerfCounterId::IpcMessagesSent);
  channel_.to_ui_.push_back(message);
}

std::optional<UiToEngine> InProcessChannel::EngineSide::TryReceive() {
  if (channel_.to_engine_.empty()) {
    return std::nullopt;
  }
  AddPerformanceCounter(PerfCounterId::IpcMessagesReceived);
  UiToEngine message = std::move(channel_.to_engine_.front());
  channel_.to_engine_.pop_front();
  return message;
}

}  // namespace microbrowser::ipc

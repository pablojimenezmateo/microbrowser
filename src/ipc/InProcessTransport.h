#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "ipc/Transport.h"

namespace microbrowser::ipc {

// A pair of endpoints backed by two queues, for running the UI and engine in
// one thread of one process.
//
// Messages are moved, not encoded — paying a serialize/deserialize round trip
// to hand a vector between two objects on the same stack would be pure waste.
// Serialization is still exercised on every message by IpcMessageTests, so the
// wire format cannot rot while this transport is the one in use.
//
// Single-threaded by design: there are no locks here, because M0 has no second
// thread and adding a mutex "just in case" would hide the moment that stops
// being true. Whatever makes the engine concurrent brings its own transport.
class InProcessChannel {
 private:
  // Declared before the accessors that return them: a member function body is
  // compiled as if it appeared after the class, but a return *type* must be
  // complete at the point of declaration.
  class UiSide final : public UiEndpoint {
   public:
    explicit UiSide(InProcessChannel& channel) : channel_(channel) {}
    void Send(const UiToEngine& message) override;
    std::optional<EngineToUi> TryReceive() override;

   private:
    InProcessChannel& channel_;
  };

  class EngineSide final : public EngineEndpoint {
   public:
    explicit EngineSide(InProcessChannel& channel) : channel_(channel) {}
    void Send(const EngineToUi& message) override;
    std::optional<UiToEngine> TryReceive() override;

   private:
    InProcessChannel& channel_;
  };

 public:
  UiEndpoint& Ui() { return ui_endpoint_; }
  EngineEndpoint& Engine() { return engine_endpoint_; }

  // Queue depths, for tests and for the idle-loop check that nothing is left
  // pending when the loop decides to block.
  std::size_t PendingForEngine() const { return to_engine_.size(); }
  std::size_t PendingForUi() const { return to_ui_.size(); }

 private:
  std::deque<UiToEngine> to_engine_;
  std::deque<EngineToUi> to_ui_;
  UiSide ui_endpoint_{*this};
  EngineSide engine_endpoint_{*this};
};

}  // namespace microbrowser::ipc

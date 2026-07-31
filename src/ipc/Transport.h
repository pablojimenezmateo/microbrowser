#pragma once

#include <optional>

#include "ipc/Message.h"

namespace microbrowser::ipc {

// The swap point between "one process" and "many processes".
//
// M0 ships InProcessTransport: two queues, no encoding, no syscalls. The
// eventual SocketTransport writes the same messages as length-prefixed frames
// over a socketpair. Nothing above this interface changes when that happens,
// which is the entire reason the interface exists this early.
//
// Deliberately non-blocking. A blocking receive would put the UI thread to
// sleep on the engine, and the whole idle-CPU story depends on the UI thread
// sleeping in exactly one place: the platform event wait.

class UiEndpoint {
 public:
  virtual ~UiEndpoint() = default;

  virtual void Send(const UiToEngine& message) = 0;
  virtual std::optional<EngineToUi> TryReceive() = 0;
};

class EngineEndpoint {
 public:
  virtual ~EngineEndpoint() = default;

  virtual void Send(const EngineToUi& message) = 0;
  virtual std::optional<UiToEngine> TryReceive() = 0;
};

}  // namespace microbrowser::ipc

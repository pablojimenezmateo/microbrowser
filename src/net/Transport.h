#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "util/WaitDescriptor.h"

namespace microbrowser::net {

// What one attempt at making progress did.
//
// `Blocked` is the state that makes the loop's promise keepable: it means
// "nothing to do yet, wait on Interest() and ask again", and it is the one
// answer that must never be turned into a retry. A caller that spun on Blocked
// would be the polling loop ADR 0011 exists to prevent.
enum class IoStatus {
  Ready,
  Blocked,
  Closed,  // the peer closed cleanly
  Failed,
};

struct IoResult {
  IoStatus status = IoStatus::Failed;
  std::size_t bytes = 0;
};

// A byte stream to one server.
//
// One of the few genuinely polymorphic boundaries in the project, and it earns
// it the same way `ipc::Transport` does: there really are two implementations
// that differ at runtime, and the alternative is a network stack that cannot be
// tested without a network. A test drives a scripted stream; production drives
// a socket with TLS on top.
//
// Deliberately narrow. Start, advance, send, receive, close — no timeouts, no
// retries, no redirect following, no header knowledge. Everything above this
// line is policy and belongs where it can be tested without I/O.
//
// **Nothing here blocks.** That is the ADR 0011 change and it is the whole of
// it: every call returns immediately, `Blocked` means "ask again after the
// loop's wait", and `Interest()` says what that wait must watch. The one
// remaining exception is name resolution, which is written down in
// SocketTransport.h along with what it would take to remove.
class Transport {
 public:
  virtual ~Transport() = default;

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // Begins connecting. False means it could not be started at all: a name that
  // does not resolve, a socket that cannot be created. True means the
  // connection is *under way* and not that it is open — nothing may be sent
  // until `Advance()` answers `Ready`.
  //
  // `secure` means TLS. It is a parameter rather than a separate class because
  // the caller must not be able to reach the plaintext path by picking a
  // different type — the decision comes from the URL scheme, which came from
  // the privacy layer.
  virtual bool StartConnect(std::string_view host, std::uint16_t port, bool secure) = 0;

  // Drives connection setup and the TLS handshake forward. `Ready` means the
  // stream is open and may be written to.
  virtual IoStatus Advance() = 0;

  // A partial write is normal and is not an error: `bytes` says how much went
  // out, and the caller sends the rest on a later turn.
  virtual IoResult Send(std::span<const std::byte> data) = 0;

  // `Closed` means the peer closed; `Failed` means an error. The two are
  // distinguished because a body delimited by connection close is complete on
  // the first and truncated on the second.
  virtual IoResult Receive(std::span<std::byte> out) = 0;

  virtual void Close() = 0;

  // What to wait on, and for what. Absent when this transport cannot block —
  // a scripted one in a test is always ready, and handing the loop a descriptor
  // for it would be a lie that costs a wakeup.
  virtual std::optional<util::WaitDescriptor> Interest() const = 0;

 protected:
  Transport() = default;
};

// Creates connections. A factory rather than a bare `new` at the call site so
// that the whole network stack can be pointed at a scripted transport in a
// test without any production code knowing.
class TransportFactory {
 public:
  virtual ~TransportFactory() = default;
  virtual std::unique_ptr<Transport> Create() = 0;
};

}  // namespace microbrowser::net

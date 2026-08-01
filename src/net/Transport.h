#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace microbrowser::net {

// A byte stream to one server.
//
// One of the few genuinely polymorphic boundaries in the project, and it earns
// it the same way `ipc::Transport` does: there really are two implementations
// that differ at runtime, and the alternative is a network stack that cannot be
// tested without a network. A test drives a scripted stream; production drives
// a socket with TLS on top.
//
// Deliberately narrow. Connect, send, receive, close — no timeouts, no retries,
// no redirect following, no header knowledge. Everything above this line is
// policy and belongs where it can be tested without I/O.
class Transport {
 public:
  virtual ~Transport() = default;

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // `secure` means TLS. It is a parameter rather than a separate class because
  // the caller must not be able to reach the plaintext path by picking a
  // different type — the decision comes from the URL scheme, which came from
  // the privacy layer.
  virtual bool Connect(std::string_view host, std::uint16_t port, bool secure) = 0;
  virtual bool Send(std::span<const std::byte> data) = 0;

  // Bytes read into `out`. Zero means the peer closed; nullopt means an error.
  // The two are distinguished because a body delimited by connection close is
  // complete on the first and truncated on the second.
  virtual std::optional<std::size_t> Receive(std::span<std::byte> out) = 0;

  virtual void Close() = 0;

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

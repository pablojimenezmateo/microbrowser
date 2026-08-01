#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/Transport.h"

namespace microbrowser::tests {

// A scripted connection. This is the entire reason `net::Transport` is a
// virtual boundary: without it none of the logic above it -- redirects, cookie
// round trips, cache behaviour, policy re-entry, and now the engine loading a
// page -- could be tested without a network.
//
// Shared support rather than a copy per test file, because a second copy is a
// second thing to keep in step with the interface.
class ScriptedTransport : public net::Transport {
 public:
  struct Exchange {
    std::string expected_host;
    std::uint16_t expected_port = 0;
    bool expected_secure = false;
    std::string response;
  };

  // What the code under test actually did, for a test that cares about the
  // request rather than the response.
  struct Log {
    std::vector<std::string> requests;
    std::vector<std::string> hosts;
    std::vector<bool> secure;
  };

  ScriptedTransport(std::vector<Exchange>& script, std::size_t& cursor, Log& log)
      : script_(script), cursor_(cursor), log_(log) {}

  bool Connect(std::string_view host, std::uint16_t port, bool secure) override;
  bool Send(std::span<const std::byte> data) override;
  std::optional<std::size_t> Receive(std::span<std::byte> out) override;
  void Close() override;

 private:
  std::vector<Exchange>& script_;
  std::size_t& cursor_;
  Log& log_;
  std::string request_;
  std::string pending_;
  bool sent_ = false;
};

class ScriptedFactory : public net::TransportFactory {
 public:
  std::unique_ptr<net::Transport> Create() override {
    return std::make_unique<ScriptedTransport>(script, cursor, log);
  }

  std::vector<ScriptedTransport::Exchange> script;
  std::size_t cursor = 0;
  ScriptedTransport::Log log;
};

// Wraps `body` in a minimal 200 response with an explicit length, which is the
// framing every test here wants and none of them wants to spell out.
std::string OkResponse(std::string_view content_type, std::string_view body);

}  // namespace microbrowser::tests

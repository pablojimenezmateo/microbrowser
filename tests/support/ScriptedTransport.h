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
#include "util/WaitDescriptor.h"

namespace microbrowser::tests {

// A scripted connection. This is the entire reason `net::Transport` is a
// virtual boundary: without it none of the logic above it -- redirects, cookie
// round trips, cache behaviour, policy re-entry, and now the engine loading a
// page -- could be tested without a network.
//
// Shared support rather than a copy per test file, because a second copy is a
// second thing to keep in step with the interface.
//
// Since ADR 0011 it also has a *held* mode, and that is what makes arrival
// order testable. A held connection answers `Blocked` until the test releases
// it, so a test can deliver three responses in any order it likes and assert
// that the page comes out the same. Nondeterminism by arrival order is the
// failure mode of asynchronous loading, and it is the one that decays silently.
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
  //
  // Indexed by exchange rather than by the order things happened. With requests
  // running concurrently those are two different orders, and a log in
  // completion order would make every assertion in every existing test depend
  // on how fast a canned response came back.
  struct Log {
    std::vector<std::string> requests;
    std::vector<std::string> hosts;
    std::vector<bool> secure;
  };

  class Factory;

  explicit ScriptedTransport(Factory& factory) : factory_(factory) {}
  ~ScriptedTransport() override;

  bool StartConnect(std::string_view host, std::uint16_t port, bool secure) override;
  net::IoStatus Advance() override;
  net::IoResult Send(std::span<const std::byte> data) override;
  net::IoResult Receive(std::span<std::byte> out) override;
  void Close() override;
  std::optional<util::WaitDescriptor> Interest() const override;

  // What this connection asked for. Empty until the request has been sent.
  const std::string& Request() const { return request_; }
  bool Released() const { return released_; }
  void Release() { released_ = true; }

 private:
  Factory& factory_;
  std::string request_;
  std::string pending_;
  std::size_t index_ = 0;
  bool sent_ = false;
  bool released_ = true;
};

class ScriptedTransport::Factory : public net::TransportFactory {
 public:
  // Whether a connection answers as soon as it is asked, or waits to be let go.
  enum class Delivery {
    Immediate,
    Held,
  };

  std::unique_ptr<net::Transport> Create() override;

  // Releases the one live connection whose request line contains `needle`.
  // Returns false when there is none, so a test cannot silently release nothing
  // and then assert on a page that never changed.
  bool Release(std::string_view needle);
  // Releases every live connection, in the order they were opened.
  std::size_t ReleaseAll();
  // How many connections are open and not yet released. The per-partition bound
  // is asserted against this.
  std::size_t Held() const;

  std::vector<Exchange> script;
  std::size_t cursor = 0;
  Log log;
  Delivery delivery = Delivery::Immediate;

 private:
  friend class ScriptedTransport;

  void Register(ScriptedTransport& transport);
  void Forget(ScriptedTransport& transport);

  // Non-owning. A connection removes itself in its destructor, which is what
  // makes "a navigation cancels everything" observable from a test: the entries
  // disappear when the requests do.
  std::vector<ScriptedTransport*> live_;
};

// The name every fixture in the suite already uses. Kept so that the arrival of
// a nested class does not rewrite all of them.
using ScriptedFactory = ScriptedTransport::Factory;

// Wraps `body` in a minimal 200 response with an explicit length, which is the
// framing every test here wants and none of them wants to spell out.
std::string OkResponse(std::string_view content_type, std::string_view body);

}  // namespace microbrowser::tests

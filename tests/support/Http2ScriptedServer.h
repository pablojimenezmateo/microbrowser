#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net/Hpack.h"
#include "net/Transport.h"

namespace microbrowser::tests {

// A transport that plays an HTTP/2 server, framing and all.
//
// Not a canned byte stream, and that is the point. `ScriptedTransport` answers
// one request with one response because that is what HTTP/1.1 is; HTTP/2's
// whole claim is that those are no longer the same shape, and a test that
// pre-baked the server's bytes would have to know the stream ids the client
// was going to choose. So this one *reads* what the client sent -- preface,
// SETTINGS, HEADERS, HPACK and all -- and answers each stream by its `:path`.
//
// Small enough to read in one sitting, and it is the only thing in the suite
// that exercises this browser's HTTP/2 in both directions at once: a bug in the
// request encoder that the response decoder happens to tolerate would pass
// every unit test and fail here.
class Http2ScriptedTransport : public net::Transport {
 public:
  class Factory;

  explicit Http2ScriptedTransport(Factory& factory) : factory_(&factory) {}
  ~Http2ScriptedTransport() override;

  bool StartConnect(std::string_view partition, std::string_view host, std::uint16_t port,
                    bool secure) override;
  net::IoStatus Advance() override;
  net::IoResult Send(std::span<const std::byte> data) override;
  net::IoResult Receive(std::span<std::byte> out) override;
  void Close() override;
  std::optional<util::WaitDescriptor> Interest() const override;
  std::string_view NegotiatedProtocol() const override;

 private:
  // Reads whatever whole frames the client has written and answers them.
  void ConsumeClientFrames();
  void Respond(std::uint32_t stream, const std::vector<net::hpack::Header>& request);

  Factory* factory_;
  net::hpack::Decoder decoder_;
  std::string from_client_;
  std::string to_client_;
  std::size_t read_at_ = 0;
  bool saw_preface_ = false;
  // The header block being assembled, and whose. The client splits one across
  // CONTINUATIONs when its cookies are large enough, and a test server that
  // could not reassemble would fail exactly the case worth testing.
  std::uint32_t assembling_ = 0;
  std::string block_;
  bool closed_ = false;
};

class Http2ScriptedTransport::Factory : public net::TransportFactory {
 public:
  // What the server answers for one path. A missing path is a 404, so a test
  // that asserts on a response it forgot to script fails rather than hangs.
  struct Route {
    std::string path;
    int status = 200;
    std::string content_type = "text/plain";
    std::string body;
    // Sent as `location`, which makes the response a redirect the client has to
    // follow -- on the same connection, which is the interesting half.
    std::string location;
  };

  std::unique_ptr<net::Transport> Create() override;

  const Route* Find(std::string_view path) const;

  // Whether a connection's handshake finishes at once or waits to be let go.
  //
  // **Held is what makes a concurrency test mean anything.** With an immediate
  // handshake each request connects, negotiates and finishes before the next
  // one even asks the pool for a connection, so six requests reuse one session
  // whether or not the pool coalesces anything -- the test passes for the wrong
  // reason. A real handshake takes turns of the loop, and during those turns the
  // other five requests are already asking.
  enum class Handshake {
    Immediate,
    Held,
  };

  // Lets every held handshake finish.
  void CompleteHandshakes() { handshake = Handshake::Immediate; }

  std::vector<Route> routes;
  Handshake handshake = Handshake::Immediate;
  // How many sockets were opened. **This is the number TD-0008 is about**: six
  // concurrent requests to one origin must produce one.
  std::size_t connects = 0;
  // Every `:path` the server was asked for, in the order the requests arrived.
  std::vector<std::string> paths;
  // Whether ALPN says `h2`. False makes this an ordinary transport that will
  // never be handed to a session, which is how a test asks for the HTTP/1.1
  // path from the same fixture.
  bool speaks_http2 = true;
};

using Http2ScriptedFactory = Http2ScriptedTransport::Factory;

}  // namespace microbrowser::tests

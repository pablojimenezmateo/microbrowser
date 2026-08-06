#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "net/Http2Session.h"
#include "net/Transport.h"

namespace {

// The server's half of one connection, from the fuzzer, handed over a few bytes
// at a time. The chunking matters: HTTP/2 frames straddle reads on a real
// socket, and a session that only ever saw whole frames would have its
// reassembly path unfuzzed -- which is where the length arithmetic is.
class FuzzTransport : public microbrowser::net::Transport {
 public:
  FuzzTransport(const std::uint8_t* data, std::size_t size, std::size_t chunk)
      : data_(data), size_(size), chunk_(std::max<std::size_t>(chunk, 1)) {}

  bool StartConnect(std::string_view, std::string_view, std::uint16_t, bool) override {
    return true;
  }
  microbrowser::net::IoStatus Advance() override { return microbrowser::net::IoStatus::Ready; }
  microbrowser::net::IoResult Send(std::span<const std::byte> data) override {
    written_ += data.size();
    return {microbrowser::net::IoStatus::Ready, data.size()};
  }
  microbrowser::net::IoResult Receive(std::span<std::byte> out) override {
    if (at_ >= size_) {
      return {microbrowser::net::IoStatus::Closed, 0};
    }
    const std::size_t take = std::min({out.size(), chunk_, size_ - at_});
    std::memcpy(out.data(), data_ + at_, take);
    at_ += take;
    return {microbrowser::net::IoStatus::Ready, take};
  }
  void Close() override {}
  std::optional<microbrowser::util::WaitDescriptor> Interest() const override {
    return std::nullopt;
  }

  std::size_t written() const { return written_; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t at_ = 0;
  std::size_t chunk_;
  std::size_t written_ = 0;
};

}  // namespace

// One HTTP/2 connection, with the server played by the fuzzer.
//
// This reaches further than any other target in the tree, and deliberately: the
// bytes go through frame reassembly, padding arithmetic, the HEADERS /
// CONTINUATION state machine, HPACK, flow-control accounting and response
// assembly, all with three streams open so that demultiplexing is exercised
// rather than bypassed. Everything a server can say to a browser is in here.
//
// The properties:
//
//  - no input reads out of bounds, whatever lengths and pad bytes it declares;
//  - a stream is never both complete and failed, because the caller acts on the
//    first answer it gets and the two mean opposite things;
//  - a completed response's body is within the bound that was checked as it
//    arrived, not after;
//  - and the session never writes unboundedly in response to reading. A peer
//    that can make a receiver emit more than it sends has a reflection
//    amplifier, and RST_STREAM and WINDOW_UPDATE are both one frame per event.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using microbrowser::net::Http2Session;
  if (size < 2) {
    return 0;
  }
  // The first byte steers the read size, so the fuzzer can find the boundary
  // cases in reassembly without having to discover them through the payload.
  const std::size_t chunk = static_cast<std::size_t>(data[0]) * 7 + 1;
  auto transport = std::make_unique<FuzzTransport>(data + 1, size - 1, chunk);
  const FuzzTransport* wire = transport.get();
  Http2Session session(std::move(transport));

  microbrowser::net::HttpHeaders headers;
  headers.Add("accept", "*/*");
  std::vector<Http2Session::StreamId> ids;
  for (int i = 0; i < 3; ++i) {
    Http2Session::Request request;
    request.method = "GET";
    request.scheme = "https";
    request.authority = "example.com";
    request.target = "/a";
    request.headers = &headers;
    if (const auto id = session.StartRequest(request)) {
      ids.push_back(*id);
    }
  }

  for (int turn = 0; turn < 32 && session.Advance(); ++turn) {
  }

  for (const Http2Session::StreamId id : ids) {
    const Http2Session::StreamState state = session.StateOf(id);
    if (state == Http2Session::StreamState::Failed && session.ErrorOf(id) == nullptr) {
      __builtin_trap();  // a failure the caller cannot render
    }
    if (state == Http2Session::StreamState::Complete) {
      const microbrowser::net::HttpResponse response = session.TakeResponse(id);
      if (response.status < 200 || response.status > 599) {
        __builtin_trap();  // an informational status is not a completed response
      }
      if (response.body.size() > 64u * 1024u * 1024u) {
        __builtin_trap();
      }
      if (session.StateOf(id) != Http2Session::StreamState::Unknown) {
        __builtin_trap();  // taking a response forgets its stream
      }
    }
  }

  // The preface, our SETTINGS, one window update, three requests and then at
  // most one reply per frame that arrived. Generous, and still an amplification
  // bound: without one, a run of PINGs would be answered forever.
  if (wire->written() > 4096 + size * 32) {
    __builtin_trap();
  }
  return 0;
}

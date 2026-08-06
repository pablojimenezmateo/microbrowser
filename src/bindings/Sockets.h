#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace microbrowser::bindings {

// What `WebSocket` is answered through.
//
// ADR 0020 §5, and the same inversion `GeometrySource`, `NetworkSource` and
// `StorageSource` use: declared here, implemented by `src/engine`. This module may see
// `util`, `js`, `dom` and `html` -- not `net`, not `csp`, not `url` -- so nothing on
// this side can open a socket, resolve a URL, or decide whether `connect-src` allows
// one. All three of those happen on the far side, which is what makes the *policy*
// unreachable from the code a page's script drives.
//
// A socket is named by an id rather than by a pointer, for the reason a fetch is: the
// object script holds lives in the JavaScript heap where the collector can see it, and
// the engine only needs to recognise which connection an event belongs to. A pointer
// across this seam would be a lifetime two garbage collectors would have to agree
// about.
class SocketSource {
 public:
  SocketSource() = default;
  SocketSource(const SocketSource&) = delete;
  SocketSource& operator=(const SocketSource&) = delete;
  virtual ~SocketSource() = default;

  // Opens one. Zero means it was refused before anything was sent -- a URL that does
  // not parse, a scheme that is not `ws`/`wss`, or `connect-src`. The caller cannot
  // tell which, deliberately: a page that could distinguish "blocked by policy" from
  // "bad URL" could map the user's policy.
  virtual std::uint64_t OpenSocket(std::string_view url) = 0;

  // Queues a message. False when the socket is not open, which script turns into an
  // `InvalidStateError`.
  virtual bool SendSocket(std::uint64_t id, std::string_view data, bool text) = 0;

  // Starts the closing handshake. Idempotent: a page that calls `close()` twice is
  // ordinary, not an error.
  virtual void CloseSocket(std::uint64_t id, std::uint16_t code, std::string_view reason) = 0;

  // How much this socket has queued and not yet written, which is `bufferedAmount`. A
  // page uses it to decide whether to send more, so it is a real number rather than
  // zero -- a `bufferedAmount` that always answered zero would make a page that paces
  // itself send without limit.
  virtual std::uint64_t SocketBufferedAmount(std::uint64_t id) = 0;

  // `EventSource`, on the same seam and for the same reason: the questions are identical
  // -- resolve a URL, ask `connect-src`, pass the privacy layer, keep the connection with
  // the document -- and two interfaces asking them would be two places for the answers to
  // drift. Zero means refused, exactly as for a socket.
  virtual std::uint64_t OpenEventSource(std::string_view url) = 0;
  virtual void CloseEventSource(std::uint64_t id) = 0;
};

}  // namespace microbrowser::bindings

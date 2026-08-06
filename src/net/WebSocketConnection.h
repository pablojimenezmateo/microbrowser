#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/Transport.h"
#include "net/WebSocketFrames.h"
#include "privacy/Verdict.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::net {

// One WebSocket, from the handshake to the close.
//
// ADR 0020 §5, and **the first connection in this browser that stays open with no
// request outstanding.** That is the interesting thing about it, and the reason the
// zero-idle-CPU invariant survives it has to be stated rather than assumed: the socket
// is a descriptor handed to the idle wait, the loop blocks on it, and a server that
// says nothing costs nothing. There is no timer here, no poll, and no keepalive --
// `Interest()` is the whole of how this object asks to be woken.
//
// What follows from that, and is written here because it is easy to lose:
//
//   * **Ping is answered, never originated.** A keepalive on a timer is exactly the
//     thing that would turn an idle page into a waking one. We reply to a ping with a
//     pong and we never send one ourselves. If a server drops the connection for that,
//     the connection drops -- which costs nothing while idle.
//   * **A `privacy::Verdict` is required to construct one**, like every other request
//     in this module: `wss://` is a request the user caused and it passes the same
//     layer. There is no overload without one.
//   * **It dies with the page that opened it.** The owner is the engine's table, a
//     navigation clears the table, and closing the transport is what the destructor
//     does. No connection outlives the document that asked for it.
class WebSocketConnection {
 public:
  enum class State : std::uint8_t {
    Connecting,  // TCP/TLS, then the HTTP upgrade
    Open,        // the handshake was accepted
    Closing,     // a close frame went out or came in
    Closed,      // the transport is gone
  };

  // What `Advance` did, so the caller knows whether anything is worth reporting to
  // script without inspecting the whole object.
  struct Progress {
    bool opened = false;   // the handshake completed on this turn
    bool closed = false;   // the connection reached Closed on this turn
    bool failed = false;   // ... because something went wrong rather than cleanly
    // Complete messages, reassembled. Text and binary are distinguished because
    // script sees a string for one and bytes for the other, and guessing from the
    // payload is how a binary message becomes mojibake.
    std::vector<std::pair<bool, std::string>> messages;  // (is_text, data)
  };

  WebSocketConnection(std::unique_ptr<Transport> transport, const privacy::Verdict& verdict,
                      std::string host, std::uint16_t port, std::string target, bool secure,
                      std::string key);

  WebSocketConnection(const WebSocketConnection&) = delete;
  WebSocketConnection& operator=(const WebSocketConnection&) = delete;
  ~WebSocketConnection();

  State GetState() const { return state_; }
  // The close code the peer sent, or 1006 when the connection dropped without one --
  // which is the code the specification reserves for exactly that and is what script
  // must see rather than a made-up 1000.
  std::uint16_t CloseCode() const { return close_code_; }
  const std::string& CloseReason() const { return close_reason_; }
  // Whether the peer closed cleanly, which is a separate question from the code: a
  // dropped connection has no code and was not clean.
  bool ClosedCleanly() const { return clean_close_; }

  // Everything that can happen without blocking. Called from the same loop turn a
  // socket became readable on.
  Progress Advance();

  // Queues a message. False when the connection is not open, which is what script sees
  // as an `InvalidStateError` -- and the queue is why `send` does not block: a partial
  // write is normal and the rest goes out on a later turn.
  bool Send(std::string_view data, bool text);

  // Starts the closing handshake: a close frame out, then the transport when the peer
  // answers or the connection drops.
  void Close(std::uint16_t code = 1000, std::string_view reason = {});

  // What to wait on. Absent once closed, which is what takes it out of the idle wait
  // rather than a flag the loop has to remember to check.
  std::optional<util::WaitDescriptor> Interest() const;

  // Whether anything is queued to send, which the loop needs so that a message written
  // by script on this turn goes out before the process blocks.
  bool HasPendingWrites() const { return !outgoing_.empty(); }

 private:
  void Fail();
  void SendRaw(std::vector<std::byte> bytes);
  bool ReadHandshake();
  void Flush();

  std::unique_ptr<Transport> transport_;
  privacy::Verdict verdict_;
  std::string host_;
  std::uint16_t port_ = 0;
  std::string target_;
  bool secure_ = true;
  std::string key_;
  State state_ = State::Connecting;
  bool sent_request_ = false;
  bool sent_close_ = false;
  bool clean_close_ = false;
  std::uint16_t close_code_ = 1006;
  std::string close_reason_;
  // Bytes that arrived and have not been framed yet, and bytes queued to go out. Both
  // bounded: a peer that sends a frame header and then nothing must not be able to make
  // this grow, and a page that calls `send` in a loop must not either.
  std::vector<std::byte> incoming_;
  std::vector<std::byte> outgoing_;
  // A message being reassembled from fragments, and what kind it started as. The
  // opcode of a continuation frame says nothing, so the *first* frame is the only place
  // this is known.
  std::string fragment_;
  bool fragment_is_text_ = true;
  bool fragmenting_ = false;
};

}  // namespace microbrowser::net

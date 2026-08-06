#include "net/WebSocketConnection.h"

#include <algorithm>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/UserAgent.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How much unframed input may pile up, and how much may be queued to go out.
//
// Both are bounds on *memory a peer or a page controls*, which is why they are here and
// not at a caller. A server that sends a frame header and then stops must not be able to
// make the incoming buffer grow; a page that calls `send` in a loop while the socket is
// slow must not either. The numbers are one frame's ceiling plus room for the frames
// around it -- generous for a protocol whose real messages are kilobytes.
constexpr std::size_t kMaxIncoming = 24u * 1024u * 1024u;
constexpr std::size_t kMaxOutgoing = 24u * 1024u * 1024u;

// The mask for the next client frame.
//
// **Not random, and that is a deliberate choice with a reason.** RFC 6455 asks for an
// unpredictable mask, and its purpose is to stop an attacker who controls the *payload*
// from making the bytes on the wire look like a valid HTTP request to an intermediary.
// This browser has no random source it can reach from `net` without adding one, and a
// weak pseudo-random mask is no better than a counter against that attack while being
// harder to reason about. What actually defeats it is `wss://` -- TLS means the
// intermediary sees none of it. A future `ws://` (plaintext) path must not ship without
// a real mask, and that is written here rather than in a ticket.
struct MaskSource {
  std::uint32_t counter = 0x5bf03635u;

  void Next(std::uint8_t out[4]) {
    counter = counter * 1664525u + 1013904223u;
    out[0] = static_cast<std::uint8_t>(counter >> 24);
    out[1] = static_cast<std::uint8_t>(counter >> 16);
    out[2] = static_cast<std::uint8_t>(counter >> 8);
    out[3] = static_cast<std::uint8_t>(counter);
  }
};

MaskSource& Masks() {
  static MaskSource source;
  return source;
}

std::span<const std::byte> AsBytes(std::string_view text) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

}  // namespace

WebSocketConnection::WebSocketConnection(std::unique_ptr<Transport> transport,
                                         const privacy::Verdict& verdict, std::string host,
                                         std::uint16_t port, std::string target, bool secure,
                                         std::string key)
    : transport_(std::move(transport)),
      verdict_(verdict),
      host_(std::move(host)),
      port_(port),
      target_(std::move(target)),
      secure_(secure),
      key_(std::move(key)) {
  if (transport_ == nullptr || !transport_->StartConnect(host_, port_, secure_)) {
    state_ = State::Closed;
    return;
  }
  AddPerformanceCounter(PerfCounterId::WebSocketConnectionsOpened);
}

WebSocketConnection::~WebSocketConnection() {
  if (transport_ != nullptr) {
    // The connection dies with its owner, and the owner is the engine's table, which a
    // navigation clears. Closing here rather than asking callers to is what makes "no
    // socket outlives the document that opened it" a property of the type.
    transport_->Close();
  }
}

std::optional<util::WaitDescriptor> WebSocketConnection::Interest() const {
  if (state_ == State::Closed || transport_ == nullptr) {
    return std::nullopt;
  }
  return transport_->Interest();
}

void WebSocketConnection::Fail() {
  if (state_ == State::Closed) {
    return;
  }
  state_ = State::Closed;
  clean_close_ = false;
  close_code_ = 1006;  // "abnormal closure": no close frame was exchanged
  if (transport_ != nullptr) {
    transport_->Close();
  }
  AddPerformanceCounter(PerfCounterId::WebSocketFailures);
}

void WebSocketConnection::SendRaw(std::vector<std::byte> bytes) {
  if (outgoing_.size() + bytes.size() > kMaxOutgoing) {
    // A page that outruns its own socket is refused rather than buffered without limit.
    Fail();
    return;
  }
  outgoing_.insert(outgoing_.end(), bytes.begin(), bytes.end());
}

void WebSocketConnection::Flush() {
  while (!outgoing_.empty() && transport_ != nullptr) {
    const IoResult sent = transport_->Send(outgoing_);
    if (sent.status == IoStatus::Blocked) {
      return;  // the rest goes out on a later turn, which is what a queue is for
    }
    if (sent.status != IoStatus::Ready || sent.bytes == 0) {
      Fail();
      return;
    }
    outgoing_.erase(outgoing_.begin(),
                    outgoing_.begin() + static_cast<std::ptrdiff_t>(sent.bytes));
  }
}

bool WebSocketConnection::ReadHandshake() {
  // The response's status line and headers, up to the blank line. Read out of the same
  // buffer the frames will come from, because a server is allowed to put the first
  // frame in the same packet as the handshake -- and a reader that dropped the tail
  // would lose the first message of every fast server.
  const std::string_view text(reinterpret_cast<const char*>(incoming_.data()), incoming_.size());
  const std::size_t end = text.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    if (incoming_.size() > 64u * 1024u) {
      Fail();  // a "handshake" this long is not one
    }
    return false;
  }

  const std::string_view head = text.substr(0, end);
  int status = 0;
  std::string upgrade;
  std::string connection;
  std::string accept;
  std::size_t at = 0;
  bool first = true;
  while (at <= head.size()) {
    const std::size_t line_end = std::min(head.find("\r\n", at), head.size());
    const std::string_view line = head.substr(at, line_end - at);
    if (first) {
      // `HTTP/1.1 101 Switching Protocols`.
      const std::size_t space = line.find(' ');
      if (space != std::string_view::npos) {
        status = 0;
        for (std::size_t i = space + 1; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) {
          status = status * 10 + (line[i] - '0');
        }
      }
      first = false;
    } else {
      const std::size_t colon = line.find(':');
      if (colon != std::string_view::npos) {
        const std::string name = util::AsciiLowerCase(std::string(line.substr(0, colon)));
        std::string value = std::string(util::TrimAscii(line.substr(colon + 1)));
        if (name == "upgrade") {
          upgrade = std::move(value);
        } else if (name == "connection") {
          connection = std::move(value);
        } else if (name == "sec-websocket-accept") {
          accept = std::move(value);
        }
      }
    }
    if (line_end >= head.size()) {
      break;
    }
    at = line_end + 2;
  }

  incoming_.erase(incoming_.begin(), incoming_.begin() + static_cast<std::ptrdiff_t>(end + 4));
  if (!WebSocketHandshakeAccepted(status, upgrade, connection, accept, key_)) {
    AddPerformanceCounter(PerfCounterId::WebSocketHandshakeRefusals);
    Fail();
    return false;
  }
  state_ = State::Open;
  return true;
}

WebSocketConnection::Progress WebSocketConnection::Advance() {
  Progress progress;
  if (state_ == State::Closed || transport_ == nullptr) {
    return progress;
  }

  const IoStatus ready = transport_->Advance();
  if (ready == IoStatus::Failed || ready == IoStatus::Closed) {
    const bool was_open = state_ == State::Open || state_ == State::Closing;
    Fail();
    progress.closed = was_open || true;
    progress.failed = !clean_close_;
    return progress;
  }
  if (ready == IoStatus::Blocked) {
    return progress;
  }

  if (!sent_request_) {
    // The upgrade request. Written here rather than through `HttpRequest` because it is
    // not a request anything else makes: no body, no content negotiation, and three
    // headers whose values are the protocol rather than a preference.
    std::string request = "GET " + target_ + " HTTP/1.1\r\n";
    request += "Host: " + host_ + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key_ + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "User-Agent: " + std::string(util::kUserAgent) + "\r\n";
    request += "\r\n";
    SendRaw(std::vector<std::byte>(reinterpret_cast<const std::byte*>(request.data()),
                                   reinterpret_cast<const std::byte*>(request.data()) +
                                       request.size()));
    sent_request_ = true;
  }
  Flush();
  if (state_ == State::Closed) {
    progress.closed = true;
    progress.failed = true;
    return progress;
  }

  // Everything readable, in one turn. A fixed buffer rather than one read per frame:
  // the socket is non-blocking and stopping early would leave bytes the loop has
  // already been told about, which is how a wait spins.
  //
  // **A close is remembered rather than acted on here.** Bytes that already arrived are
  // a complete handshake and complete frames whatever the peer did next, and returning
  // on the close instead of framing them loses the last message of every server that
  // hangs up promptly -- which is most of them, and was the first bug this file had.
  bool peer_closed = false;
  bool read_failed = false;
  std::byte chunk[8192];
  while (!peer_closed && !read_failed) {
    const IoResult read = transport_->Receive(chunk);
    if (read.status == IoStatus::Blocked) {
      break;
    }
    if (read.status == IoStatus::Closed) {
      peer_closed = true;
      break;
    }
    if (read.status != IoStatus::Ready || read.bytes == 0 ||
        incoming_.size() + read.bytes > kMaxIncoming) {
      read_failed = true;
      break;
    }
    incoming_.insert(incoming_.end(), chunk, chunk + read.bytes);
  }
  if (read_failed) {
    Fail();
    progress.closed = true;
    progress.failed = true;
    return progress;
  }

  if (state_ == State::Connecting) {
    if (!ReadHandshake()) {
      if (state_ == State::Closed) {
        progress.closed = true;
        progress.failed = true;
      }
      return progress;
    }
    progress.opened = true;
  }

  // Frames, until the buffer holds less than one.
  while (state_ != State::Closed) {
    const WebSocketDecodeResult decoded = DecodeWebSocketFrame(incoming_, kMaxIncoming);
    if (decoded.status == WebSocketDecode::Incomplete) {
      break;
    }
    if (decoded.status == WebSocketDecode::Failed) {
      // A framing error has no recovery: the stream's structure is no longer trusted,
      // so the specification closes and so does this.
      Fail();
      progress.closed = true;
      progress.failed = true;
      return progress;
    }
    incoming_.erase(incoming_.begin(),
                    incoming_.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
    const WebSocketFrame& frame = decoded.frame;
    std::string payload;
    payload.reserve(frame.payload.size());
    for (const std::byte byte : frame.payload) {
      payload.push_back(static_cast<char>(byte));
    }

    switch (frame.opcode) {
      case WebSocketFrame::Opcode::Ping: {
        // Answered, never originated -- see the header. The pong echoes the ping's
        // payload, which is what §5.5.3 requires.
        std::uint8_t mask[4] = {};
        Masks().Next(mask);
        SendRaw(EncodeWebSocketFrame(WebSocketFrame::Opcode::Pong, frame.payload, mask));
        AddPerformanceCounter(PerfCounterId::WebSocketPongsSent);
        break;
      }
      case WebSocketFrame::Opcode::Pong:
        // Nothing to do: we never sent a ping, so a pong is either a server being
        // conversational or a response to nothing. Either way it is not an error.
        break;
      case WebSocketFrame::Opcode::Close: {
        if (payload.size() >= 2) {
          close_code_ = static_cast<std::uint16_t>(
              (static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1]));
          close_reason_ = payload.substr(2);
        } else {
          close_code_ = 1005;  // "no status received", which is a real code
        }
        clean_close_ = true;
        if (!sent_close_) {
          // The closing handshake: their frame, then ours, then the transport. Echoing
          // the code is what the specification asks for.
          Close(close_code_, close_reason_);
        }
        state_ = State::Closed;
        Flush();
        transport_->Close();
        progress.closed = true;
        return progress;
      }
      case WebSocketFrame::Opcode::Continuation:
        if (!fragmenting_) {
          Fail();  // a continuation with nothing to continue
          progress.closed = true;
          progress.failed = true;
          return progress;
        }
        fragment_ += payload;
        if (frame.final) {
          progress.messages.emplace_back(fragment_is_text_, fragment_);
          fragment_.clear();
          fragmenting_ = false;
        }
        break;
      case WebSocketFrame::Opcode::Text:
      case WebSocketFrame::Opcode::Binary:
        if (fragmenting_) {
          Fail();  // a new message before the last one finished
          progress.closed = true;
          progress.failed = true;
          return progress;
        }
        if (frame.final) {
          progress.messages.emplace_back(frame.opcode == WebSocketFrame::Opcode::Text, payload);
        } else {
          // The *first* frame is the only place the kind is known: a continuation's
          // opcode says nothing, so guessing later is how a binary message becomes
          // mojibake.
          fragmenting_ = true;
          fragment_is_text_ = frame.opcode == WebSocketFrame::Opcode::Text;
          fragment_ = payload;
        }
        break;
    }
  }
  Flush();

  if (peer_closed && state_ != State::Closed) {
    // Now that everything that arrived has been read: a close frame already exchanged
    // makes this the clean end of a handshake we agreed to, and anything else is a drop.
    const bool was_open = state_ == State::Open;
    if (state_ == State::Closing && sent_close_) {
      state_ = State::Closed;
      clean_close_ = true;
      transport_->Close();
    } else {
      Fail();
    }
    progress.closed = true;
    progress.failed = !clean_close_ && was_open;
  }
  return progress;
}

bool WebSocketConnection::Send(std::string_view data, bool text) {
  if (state_ != State::Open) {
    return false;
  }
  std::uint8_t mask[4] = {};
  Masks().Next(mask);
  SendRaw(EncodeWebSocketFrame(
      text ? WebSocketFrame::Opcode::Text : WebSocketFrame::Opcode::Binary, AsBytes(data), mask));
  Flush();
  return true;
}

void WebSocketConnection::Close(std::uint16_t code, std::string_view reason) {
  if (state_ == State::Closed || sent_close_) {
    return;
  }
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8) & 0xFFu));
  payload.push_back(static_cast<char>(code & 0xFFu));
  payload += reason;
  std::uint8_t mask[4] = {};
  Masks().Next(mask);
  SendRaw(EncodeWebSocketFrame(WebSocketFrame::Opcode::Close, AsBytes(payload), mask));
  sent_close_ = true;
  if (state_ == State::Open) {
    state_ = State::Closing;
  }
  Flush();
}

}  // namespace microbrowser::net

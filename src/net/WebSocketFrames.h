#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::net {

// The WebSocket wire format, RFC 6455 §5, and the handshake that opens it.
//
// ADR 0020 §5. Separated from the connection that carries it because this half is a
// *parser over bytes a server chose* and the other half is a socket: everything here
// is a pure function, which is what makes it fuzzable and what makes the connection
// testable without one.
//
// Three properties are the reason it is written rather than taken from a library:
//
//   * **A frame's length is up to 63 bits, from the wire.** The 8-byte form can
//     declare 9 exabytes, so the bound is not optional and cannot live at the caller:
//     a decoder that returned "need more bytes" for an absurd length would ask the
//     connection to buffer it.
//   * **A client frame must be masked and a server frame must not be.** The mask is
//     not security -- it defeats proxy cache poisoning -- but the *rule* is
//     load-bearing: a browser that accepted a masked server frame would accept a
//     frame a proxy could have written.
//   * **Control frames cannot be fragmented and cannot exceed 125 bytes.** Both are
//     refusals rather than accommodations, because a fragmented `close` is a state
//     machine two implementations disagree about.

// What a decoded frame is. `payload` is unmasked and complete for this frame; putting
// fragments back together is the connection's business, since a continuation frame is
// only meaningful against what came before it.
struct WebSocketFrame {
  enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
  };

  Opcode opcode = Opcode::Text;
  bool final = true;
  std::vector<std::byte> payload;

  bool IsControl() const {
    return opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
  }
};

// What a decode attempt produced.
enum class WebSocketDecode {
  // A whole frame came out, and `consumed` bytes of input belonged to it.
  Ok,
  // Nothing is wrong; there are not enough bytes yet. The caller waits and asks again
  // with more. Distinguished from Failed because the two mean opposite things about
  // the connection: one keeps it, the other closes it.
  Incomplete,
  // The bytes cannot be a frame, or break a rule that has no recovery. The connection
  // must close -- the specification says so, and it is also the only safe answer for a
  // stream whose framing is no longer trusted.
  Failed,
};

struct WebSocketDecodeResult {
  WebSocketDecode status = WebSocketDecode::Incomplete;
  WebSocketFrame frame;
  std::size_t consumed = 0;
};

// One frame off the front of `input`, which is a server's bytes.
//
// `max_payload` bounds a single frame's payload; a longer declared length is `Failed`
// rather than `Incomplete`, and that distinction is the bound doing its work -- an
// absurd length must not become an instruction to buffer.
WebSocketDecodeResult DecodeWebSocketFrame(std::span<const std::byte> input,
                                           std::size_t max_payload = 16u * 1024u * 1024u);

// One frame to send, masked. `mask` is the four key bytes; the caller supplies them
// because randomness is not this function's business and a test needs them fixed.
//
// Every client frame is masked -- there is no unmasked spelling here, because a
// browser that could send one would be a browser a proxy could be taught to cache.
std::vector<std::byte> EncodeWebSocketFrame(WebSocketFrame::Opcode opcode,
                                            std::span<const std::byte> payload,
                                            const std::uint8_t mask[4], bool final = true);

// The `Sec-WebSocket-Accept` value a server must return for this key: the base64 of
// SHA-1 over the key concatenated with RFC 6455's magic GUID.
//
// Checking it is what makes the handshake a handshake rather than a 101 status: a
// server that echoes the status without computing this has not agreed to speak the
// protocol, and something in between may have answered instead.
std::string WebSocketAcceptFor(std::string_view key);

// Whether a server's response opened the connection. Takes the status and the header
// values rather than an `HttpResponse` so that this file stays a pure function of
// bytes -- and so that the *caller* is the one that had to have parsed HTTP.
bool WebSocketHandshakeAccepted(int status, std::string_view upgrade, std::string_view connection,
                                std::string_view accept, std::string_view sent_key);

}  // namespace microbrowser::net
